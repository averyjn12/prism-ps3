#!/bin/bash
# Host packages PSL1GHT and its tools need (run as root in WSL PSBBN). Owner OK 2026-11-22:
# "download anything you need".
set -e
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq autoconf automake libtool bison flex texinfo libelf-dev libssl-dev libgmp-dev libmpfr-dev libmpc-dev python3-setuptools > /dev/null
echo "apt done"
