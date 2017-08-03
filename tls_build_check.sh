#!/bin/sh
pkill qemu
make -j8 && ./scripts/build image=tests -j8
