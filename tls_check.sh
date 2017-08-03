#!/bin/sh
pkill qemu
./tls_build_check.sh
make check
