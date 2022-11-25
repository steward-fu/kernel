# Linux Kernel 3.11.1 for Sharp Zaurus C3x00 
## Description
The default configuration file is arch/arm/configs/c3x00_defconfig.  
  
## How to setup toolchain
```console
$ cd
$ wget https://github.com/steward-fu/sl-c700/releases/download/v1.2/toolchain_for_kernel_3.x.tar.gz
$ tar xvf toolchain_for_kernel_3.x.tar.gz
$ mv toolchain /opt/c3200
$ export PATH=$PATH:/opt/c3200/bin
```
  
## How to build kernel
```console
$ ARCH=arm CROSS_COMPILE=arm-none-linux-gnueabi- make c3x00_defconfig
$ ARCH=arm CROSS_COMPILE=arm-none-linux-gnueabi- make zImage modules -j4
$ INSTALL_MOD_PATH=out ARCH=arm CROSS_COMPILE=arm-none-linux-gnueabi- make modules_install
```
