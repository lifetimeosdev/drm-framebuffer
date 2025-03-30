#!/bin/bash

set -e

#CC=aarch64-linux-gnu-gcc
CC=gcc
CFLAGS="-O0 -ggdb -pedantic -Wall -I /usr/include/libdrm -I."
LDFLAGS="-ldrm"

$CC $CFLAGS -c -o picture.o picture.s
$CC $CFLAGS -c -o drm_framebuffer.o drm_framebuffer.c
$CC $CFLAGS -z noexecstack -o drm_framebuffer drm_framebuffer.o picture.o $LDFLAGS

# cat 1.png | convert -extent 1920x1080 -gravity Center - bgra:- | cat >1.dat
