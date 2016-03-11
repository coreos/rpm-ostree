#!/bin/sh
set -xeuo pipefail
yum -y install rpm-ostree
cd /var/tmp
mkdir -p test
cd test
mkdir repo
ostree --repo=repo init --mode=bare-user

