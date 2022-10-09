#!/bin/bash

function print_help() {
    echo
    echo "Usage:"
    echo "    make_suniv.sh [pocketgo | trimui | fc3000_tft1 | fc3000_tft2 | fc3000_ips1 | fc3000_ips2]"
    echo 
    echo "Notes:"
    echo "    fc3000_tft1 = T2812-M106-24C-7D  ->  FC3000 TFT v1"
    echo "    fc3000_tft2 = T2815-M110-24C-25  ->  FC3000 TFT v2"
    echo "    fc3000_ips1 = RB411-11A          ->  FC3000 IPS v1"
    echo "    fc3000_ips2 = WL-28H105-A1       ->  FC3000 IPS v2"
    echo
    exit
}

if [ "$#" -ne 1 ]; then
    print_help
fi

export PATH=/opt/miyoo/bin:$PATH
sed -i -e 's/CONFIG_CMDLINE=.*/CONFIG_CMDLINE="console=tty0 console=ttyS0,115200 rootwait\ root=\/dev\/mmcblk0p1\ rw\ fstype=ext4\ init=\/sbin\/init\ --\ '$1'"/g' .config
ARCH=arm CROSS_COMPILE=arm-linux- make zImage modules dtbs -j4
echo "task done !"
