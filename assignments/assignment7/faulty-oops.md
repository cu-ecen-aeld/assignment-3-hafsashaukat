# Assignment 7 - Faulty Kernel Oops Analysis

## What Happened

I ran the following command inside QEMU:

`echo "hello_world" > /dev/faulty`

This caused a kernel oops because the faulty driver tried to access an invalid memory address. The error message showed:

`Unable to handle kernel NULL pointer dereference at virtual address 0000000000000000
pc : faulty_write+0x10/0x20 [faulty]`

## Cause of the Error

The error happened in the `faulty_write()` function in `misc-modules/faulty.c`.

On line 53, the code contains:

`*(int *)0 = 0;`

This line tries to write to memory address `0x0`, which causes a NULL pointer dereference and triggers the kernel oops.

## How I Found the Error

I looked at the call trace and saw that the error occurred in `faulty_write()`. The `+0x10` indicates the offset of the instruction within that function.

I then used:

`grep -n -A 15 "faulty_write" misc-modules/faulty.c`

This showed the function and helped me identify line 53 as the cause of the error.

## Conclusion

The kernel oops was caused by an intentional NULL pointer dereference in the faulty driver. The error message and call trace helped me find the function responsible, and checking the source code showed the exact line causing the problem.
