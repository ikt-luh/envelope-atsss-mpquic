#!/bin/bash

cd picotls
cmake .
make

cd ..
cd picoquic
cmake .
make
cd ..

mkfifo /tmp/fifo_input
mkfifo /tmp/fifo_output
