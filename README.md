# Linux Kernel 4.14.0 for FC3000 Handheld (Debian Wheezy)
## Description
The default serial port is UART0 (PE0, PE1) and default configuration file is arch/arm/configs/suniv-debian_defconfig.  
  
## Support LCD panel  
```console
fc3000_tft1 = T2812-M106-24C-7D  ->  FC3000 TFT v1
fc3000_tft2 = T2815-M110-24C-25  ->  FC3000 TFT v2
fc3000_ips1 = RB411-11A          ->  FC3000 IPS v1
fc3000_ips2 = WL-28H105-A1       ->  FC3000 IPS v2
```
  
## How to setup toolchain
```console
$ cd
$ wget https://github.com/steward-fu/miyoo/releases/download/v1.0/toolchain.7z
$ 7za x toolchain.7z
$ sudo mv miyoo /opt
```
  
## How to build kernel
```console
$ ARCH=arm make suniv-debian_defconfig
$ ./tools/make_suniv.sh fc3000_ips2
```
  
## How to flash into MicroSD
https://github.com/steward-fu/bootloader/blob/f1c100s_fc3000_uboot-2018.01/README.md
  
## Thanks
neotendo  
kendling
