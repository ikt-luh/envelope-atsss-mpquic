#!/bin/bash

loc=pwd
arch=$(uname -i)
if  [[ $arch == arm* ]] || [[ $arch = aarch64 ]]; then
        echo "ARM architecture detected, compiling ARM pico libraries"
        cd $loc/picotls_arm
        make
        cmake . -DWITH_FUSION=OFF
        cd $loc/picoquic_arm
        make
        cmake . -DPTLS_WITH_FUSION=OFF

else
        echo "x86 architecture detected, compiling x86 pico libraries"
        cd $loc/picotls
	make
	cmake .
	cd $loc/picoquic
	make
	cmake .
fi

mkfifo /tmp/fifo_input
mkfifo /tmp/fifo_output
