#!/bin/sh

make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- -C ../../linux-nctusde/ M=$PWD modules
