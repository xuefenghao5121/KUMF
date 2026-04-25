#!/bin/bash
sudo apt update >/dev/null 2>&1
sudo apt install -y python3-pip >/dev/null 2>&1
sudo apt install -y libnuma-dev >/dev/null 2>&1
sudo apt install -y vmtouch >/dev/null 2>&1
pip3 install -r requirements.txt
echo "DONE"
