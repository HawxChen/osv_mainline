#!/bin/sh
pkill qemu
make mode=debug -j8 && scripts/build mode=debug image=golang-example -j8
