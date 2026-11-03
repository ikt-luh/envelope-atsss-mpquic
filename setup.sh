#!/bin/bash

cd picotls
cmake .
make

cd ..
cd picoquic
cmake .
make
cd ..
