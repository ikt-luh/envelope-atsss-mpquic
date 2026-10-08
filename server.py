#!/usr/bin/env python3
import os
import signal
import subprocess
import sys
import time

SERVER_FIFO_OUTPUT = os.getenv("SERVER_FIFO_OUTPUT", "/tmp/fifo_output")
SERVER_FIFO_INPUT = os.getenv("SERVER_FIFO_INPUT", "/tmp/fifo_input")

def ensure_fifo(path):
    if not os.path.exists(path):
        os.mkfifo(path)

ensure_fifo(SERVER_FIFO_OUTPUT)
ensure_fifo(SERVER_FIFO_INPUT)

# Start atsss_server
subprocess.Popen([
    "./atsss_project/build/atsss_server",
    "4445",
    "./atsss_project/build/server_cert.pem",
    "./atsss_project/build/server_key.pem",
    SERVER_FIFO_OUTPUT,
    SERVER_FIFO_INPUT,
])


while True:
    time.sleep(0.5)
