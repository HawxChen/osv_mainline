#!/bin/sh
scripts/build mode=debug image=golang-example clean
scripts/build image=golang-example clean
