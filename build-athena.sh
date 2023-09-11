#!/bin/bash

export ARCH=arm64
export CROSS_COMPILE=/home/granpc/android/prebuilts/gcc/linux-x86/aarch64/aarch64-linux-android-4.9/bin/aarch64-linux-android-

make CONFIG_BBRY=1 O=out CROSS_COMPILE=$CROSS_COMPILE ARCH=$ARCH mrproper

# TODO: perf?
make CONFIG_BBRY=1 O=out CROSS_COMPILE=$CROSS_COMPILE ARCH=$ARCH athena-perf_defconfig
# use config extracted from device for now
cp config-from-athena out/.config
make CONFIG_BBRY=1 O=out CROSS_COMPILE=$CROSS_COMPILE ARCH=$ARCH -j32
