#!/bin/bash
# Script outline to install and build kernel.
# Author: Siddhant Jajoo.

set -e
set -u

OUTDIR=/tmp/aeld
KERNEL_REPO=git://git.kernel.org/pub/scm/linux/kernel/git/stable/linux-stable.git
KERNEL_VERSION=v5.15.163
BUSYBOX_VERSION=1_33_1
FINDER_APP_DIR=$(realpath $(dirname $0))
ARCH=arm64
CROSS_COMPILE=aarch64-none-linux-gnu-
JOBS=1

if [ $# -lt 1 ]
then
	echo "Using default directory ${OUTDIR} for output"
else
	OUTDIR=$1
	echo "Using passed directory ${OUTDIR} for output"
fi

mkdir -p ${OUTDIR}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/linux-stable" ]; then
    #Clone only if the repository does not exist.
	echo "CLONING GIT LINUX STABLE VERSION ${KERNEL_VERSION} IN ${OUTDIR}"
	git clone ${KERNEL_REPO} --depth 1 --single-branch --branch ${KERNEL_VERSION}
fi
if [ ! -e ${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image ]; then
    cd linux-stable
    echo "Checking out version ${KERNEL_VERSION}"
    git checkout ${KERNEL_VERSION}

    # TODO: Add your kernel build steps here
    make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" mrproper
    make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" defconfig
    make -j"${JOBS}" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" Image
fi

echo "Adding the Image in outdir"
cp "${OUTDIR}/linux-stable/arch/${ARCH}/boot/Image" "${OUTDIR}/Image"

echo "Creating the staging directory for the root filesystem"
cd "$OUTDIR"
if [ -d "${OUTDIR}/rootfs" ]
then
	echo "Deleting rootfs directory at ${OUTDIR}/rootfs and starting over"
    sudo rm  -rf ${OUTDIR}/rootfs
fi

# TODO: Create necessary base directories
mkdir -p ${OUTDIR}/rootfs/{bin,dev,etc,home,lib,lib64,proc,sbin,sys,tmp,usr/{bin,lib,lib64,sbin},var/{log,tmp}}

cd "$OUTDIR"
if [ ! -d "${OUTDIR}/busybox" ]
then
git clone git://busybox.net/busybox.git
    cd busybox
    git checkout "${BUSYBOX_VERSION}"
    # TODO:  Configure busybox
else
    cd "${OUTDIR}/busybox"
fi

make distclean
make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" defconfig

# TODO: Make and install busybox
make -j"${JOBS}" ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}"
make ARCH="${ARCH}" CROSS_COMPILE="${CROSS_COMPILE}" CONFIG_PREFIX="${OUTDIR}/rootfs" install

echo "Library dependencies"
${CROSS_COMPILE}readelf -a busybox | grep "program interpreter"
${CROSS_COMPILE}readelf -a busybox | grep "Shared library"

# TODO: Add library dependencies to rootfs
SYSROOT=$(${CROSS_COMPILE}gcc -print-sysroot)
cp -aL "${SYSROOT}/lib/." "${OUTDIR}/rootfs/lib/"

if [ -d "${SYSROOT}/lib64" ]; then
	cp -aL "${SYSROOT}/lib64/." "${OUTDIR}/rootfs/lib64/"
fi

# TODO: Make device nodes
sudo mknod -m 666 "${OUTDIR}/rootfs/dev/null" c 1 3
sudo mknod -m 600 "${OUTDIR}/rootfs/dev/console" c 5 1

# TODO: Clean and build the writer utility
make -C "${FINDER_APP_DIR}" clean
make -C "${FINDER_APP_DIR}" CROSS_COMPILE="${CROSS_COMPILE}"

# TODO: Copy the finder related scripts and executables to the /home directory
# on the target rootfs
cp "${FINDER_APP_DIR}/writer" "${OUTDIR}/rootfs/home/"
cp "${FINDER_APP_DIR}/finder.sh" "${OUTDIR}/rootfs/home/"
cp "${FINDER_APP_DIR}/finder-test.sh" "${OUTDIR}/rootfs/home/"
cp "${FINDER_APP_DIR}/autorun-qemu.sh" "${OUTDIR}/rootfs/home/"

mkdir -p "${OUTDIR}/rootfs/home/conf"

cp "${FINDER_APP_DIR}/../conf/username.txt" "${OUTDIR}/rootfs/home/conf/"
cp "${FINDER_APP_DIR}/../conf/assignment.txt" "${OUTDIR}/rootfs/home/conf/"

# TODO: Chown the root directory
sudo chown -R root:root "${OUTDIR}/rootfs"

# TODO: Create initramfs.cpio.gz
echo "Creating initramfs.cpio.gz"
cd "${OUTDIR}/rootfs"
find . -print | cpio -ov -H newc | gzip -9 > "${OUTDIR}/initramfs.cpio.gz"
