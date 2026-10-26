/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesdchar.h"
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include "aesd_ioctl.h"

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR("Hafsa Shaukat"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    // get the aesd device structure associated with this inode
    struct aesd_dev *dev;

    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);

    // store the device pointer for future read and write operations
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */

    // no additional cleanup is needed when closing the device
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
	struct aesd_dev *dev = filp->private_data;
	struct aesd_buffer_entry *entry;
	size_t entry_offset = 0;
	size_t bytes_to_copy;
    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    if (*f_pos < 0)
        return -EINVAL;

    if (mutex_lock_interruptible(&dev->lock))
        return -ERESTARTSYS;

    // find the buffer entry corresponding to the current file offset
    entry = aesd_circular_buffer_find_entry_offset_for_fpos(
        &dev->buffer, *f_pos, &entry_offset);

    if (entry == NULL) {
        // no more data available
        goto out;
    }

    // limit the read to the current entry and requested count
    bytes_to_copy = min(count, entry->size - entry_offset);

    // copy the data from kernel space to user space
    if (copy_to_user(buf, entry->buffptr + entry_offset, bytes_to_copy)) {
        retval = -EFAULT;
        goto out;
    }

    // update the file offset
    *f_pos += bytes_to_copy;
    retval = bytes_to_copy;

out:
    mutex_unlock(&dev->lock);
    return retval;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
	struct aesd_dev *dev = filp->private_data;
	struct aesd_buffer_entry entry;
	char *new_buffer;
	char *newline;
	char *user_data;
	size_t total_size;
	size_t command_size;
	size_t remaining_size;
    ssize_t retval = count;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);

    if (count == 0)
	    return 0;

    if (mutex_lock_interruptible(&dev->lock))
	    return -ERESTARTSYS;

    // check for overflow before allocating memory
    if (count > SIZE_MAX - dev->pending_size) {
	    retval = -ENOMEM;
	    goto out;
    }

    total_size = dev->pending_size + count;

    // copy user data into temporary kernel memory first
    user_data = memdup_user(buf, count);
    if (IS_ERR(user_data)) {
	    retval = PTR_ERR(user_data);
	    goto out;
    }

    // allocate memory for the pending data and new write
    new_buffer = krealloc(dev->pending_write, total_size, GFP_KERNEL);

    if (!new_buffer) {
	    kfree(user_data);
	    retval = -ENOMEM;
	    goto out;
    }

    dev->pending_write = new_buffer;

    // append the validated user data to the pending command
    memcpy(dev->pending_write + dev->pending_size, user_data, count);
    kfree(user_data);

    dev->pending_size = total_size;

    // process each completed newline-terminated command
    while ((newline = memchr(dev->pending_write, '\n',
                             dev->pending_size)) != NULL) {

        command_size = newline - dev->pending_write + 1;
        remaining_size = dev->pending_size - command_size;

        // allocate memory for the completed command
        entry.buffptr = kmalloc(command_size, GFP_KERNEL);
        if (!entry.buffptr) {
		retval = -ENOMEM;
		goto out;
        }

        memcpy((void *)entry.buffptr, dev->pending_write, command_size);
        entry.size = command_size;

        // free the oldest command when the buffer is full
        if (dev->buffer.full) {
		kfree(dev->buffer.entry[dev->buffer.in_offs].buffptr);
        }

        aesd_circular_buffer_add_entry(&dev->buffer, &entry);

        // keep any data remaining after the completed command
        if (remaining_size > 0) {
		memmove(dev->pending_write,
				dev->pending_write + command_size, 
				remaining_size);
        }
	
	dev->pending_size = remaining_size;
    }

    // release pending storage when no incomplete command remains
    if (dev->pending_size == 0) {
	    kfree(dev->pending_write);
	    dev->pending_write = NULL;
    }

out:
	mutex_unlock(&dev->lock);
    	return retval;
    
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
	struct aesd_dev *dev = filp->private_data;
	loff_t total_size = 0;
	loff_t new_pos;
	int i;
	
	if (mutex_lock_interruptible(&dev->lock))
		return -ERESTARTSYS;
	
	// calculate the total size of all stored commands
	for (i = 0; i < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED; i++) {
		total_size += dev->buffer.entry[i].size;
	}
	// calculate the new file position
	switch (whence) {
		case SEEK_SET:
			new_pos = offset;
			break;
		case SEEK_CUR:
			new_pos = filp->f_pos + offset;
			break;
		case SEEK_END:
			new_pos = total_size + offset;
			break;
		default:
			mutex_unlock(&dev->lock);
			return -EINVAL;
	}
	
	// reject invalid file positions
	if (new_pos < 0 || new_pos > total_size) {
		mutex_unlock(&dev->lock);
		return -EINVAL;
	}
	
	// update the file position
	filp->f_pos = new_pos;
	
	mutex_unlock(&dev->lock);
	
	return new_pos;
}

static long aesd_unlocked_ioctl(struct file *filp,
                                unsigned int cmd,
                                unsigned long arg)
{
	struct aesd_dev *dev = filp->private_data;
    	struct aesd_seekto seekto;
    	struct aesd_circular_buffer *buffer = &dev->buffer;
    	size_t absolute_offset = 0;
    	unsigned int command_count;
    	unsigned int index;
    	unsigned int i;
    	long retval = 0;

	// only support the Assignment 9 seek command
	if (cmd != AESDCHAR_IOCSEEKTO)
		return -ENOTTY;

	// copy the seek request from user space
	if (copy_from_user(&seekto, (void __user *)arg,
                       sizeof(seekto)))
		return -EFAULT;

    	if (mutex_lock_interruptible(&dev->lock))
        	return -ERESTARTSYS;

	// determine the number of stored commmands
 	if (buffer->full) {
		command_count = AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;
	} else {
		command_count = buffer->in_offs;
	}

    	// check that the requested command exists
    	if (seekto.write_cmd >= command_count) {
        	retval = -EINVAL;
        	goto out;
    	}

    	// calculate the absolute offset of the requested command
    	for (i = 0; i < seekto.write_cmd; i++) {
        	index = (buffer->out_offs + i) %
                	AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

        	absolute_offset += buffer->entry[index].size;
    	}

    	// find the requested command in the circular buffer
    	index = (buffer->out_offs + seekto.write_cmd) %
            	AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED;

    	// validate the offset within the command
    	if (seekto.write_cmd_offset >= buffer->entry[index].size) {
        	retval = -EINVAL;
        	goto out;
    	}

    	// update the file position
    	absolute_offset += seekto.write_cmd_offset;
    	filp->f_pos = absolute_offset;

out:
	mutex_unlock(&dev->lock);
    	return retval;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .llseek =   aesd_llseek,
    .unlocked_ioctl = aesd_unlocked_ioctl,
    .open =     aesd_open,
    .release =  aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    // initialize the circular buffer
    aesd_circular_buffer_init(&aesd_device.buffer);

    // initialize the mutex
    mutex_init(&aesd_device.lock);

    // initialize pending write storage
    aesd_device.pending_write = NULL;
    aesd_device.pending_size = 0;

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
	int i;
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
    // free all completed commands stored in the circular buffer
    for (i = 0; i < AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED; i++) {
        kfree(aesd_device.buffer.entry[i].buffptr);
    }

    // free any incomplete write command
    kfree(aesd_device.pending_write);

    // destroy the mutex
    mutex_destroy(&aesd_device.lock);

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
