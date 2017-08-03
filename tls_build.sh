#!/bin/sh
pkill qemu
make mode=debug -j8 && ./scripts/build mode=debug image=tests -j8
