#!/bin/bash
# sets some temporary env variables


arch=$(uname -i)
if  [[ $arch == arm* ]] || [[ $arch = aarch64 ]]; then
	echo "ARM architecture detected, using picoquic_arm precompiled library"
        export PICOQUIC_DIR=$PWD/picoquic_arm
	export PICOTLS_DIR=$PWD/picotls_arm
else
	echo "x86 architecture detected, using picoquic precompiled library"
	export PICOQUIC_DIR=$PWD/picoquic
	export PICOTLS_DIR=$PWD/picotls
fi

export FEMTO_QUIC_DIR=$PWD/femto_quic
export AUTOSYNDESIS_DIR=$PWD/autosyndesis
export ATSSS_PROJECT_DIR=$PWD/atsss_project


rm -rf ${FEMTO_QUIC_DIR}/build
mkdir ${FEMTO_QUIC_DIR}/build
cd femto_quic/build
if  [[ $arch == arm* ]] || [[ $arch = aarch64 ]]; then
        echo "ARM architecture detected, using picoquic_arm precompiled library"
        cmake -DCMAKE_BUILD_TYPE=Debug -DARM_ARCH=ON -DENABLE_DEBUG=$ENABLE_PRINT ..
else
        echo "x86 architecture detected, using picoquic precompiled library"
        cmake -DCMAKE_BUILD_TYPE=Debug -DARM_ARCH=OFF -DENABLE_DEBUG=$ENABLE_PRINT ..
fi
make

rm -rf ${AUTOSYNDESIS_DIR}/build
mkdir ${AUTOSYNDESIS_DIR}/build
cd ../../autosyndesis/build
if  [[ $arch == arm* ]] || [[ $arch = aarch64 ]]; then
        echo "ARM architecture detected, using picoquic_arm precompiled library"
        cmake .. -DARM_ARCH=ON -DENABLE_DEBUG=$ENABLE_PRINT
else
        echo "x86 architecture detected, using picoquic precompiled library"
        cmake .. -DARM_ARCH=OFF -DENABLE_DEBUG=$ENABLE_PRINT
fi
make

rm -rf ${ATSSS_PROJECT_DIR}/build
mkdir ${ATSSS_PROJECT_DIR}/build
cd ../../atsss_project/build
if  [[ $arch == arm* ]] || [[ $arch = aarch64 ]]; then
        echo "ARM architecture detected, using picoquic_arm precompiled library"
        cmake -DCMAKE_BUILD_TYPE=Debug -DARM_ARCH=ON -DENABLE_DEBUG=$ENABLE_PRINT ..
else
        echo "x86 architecture detected, using picoquic precompiled library"
        cmake -DCMAKE_BUILD_TYPE=Debug -DARM_ARCH=OFF -DENABLE_DEBUG=$ENABLE_PRINT ..
fi
make

openssl genrsa -out server_key.pem 2048
openssl req -x509 -new -nodes -key server_key.pem -sha256 -days 365 -out server_cert.pem -subj "/C=DE/ST=NRW/L=Duisburg/O=UDE"

mkfifo -m 666 /tmp/fifo_input
mkfifo -m 666 /tmp/fifo_output


