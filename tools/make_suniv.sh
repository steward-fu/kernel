#!/bin/bash
export PATH=/opt/miyoo/bin:$PATH
ARCH=arm CROSS_COMPILE=arm-linux- make all -j4
echo "task done !"
