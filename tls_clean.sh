#!/bin/sh
pkill qemu
scripts/build mode=debug image=tests clean
scripts/build image=tests clean
