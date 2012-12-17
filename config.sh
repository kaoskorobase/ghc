#!/bin/sh

. env.sh
CPPFLAGS="$TARGET_CPPFLAGS" CFLAGS="$TARGET_CFLAGS" LDFLAGS="$TARGET_LDFLAGS" ./configure \
    --with-local-gcc=/usr/bin/gcc \
    --target=arm-apple-darwin10 \
    --with-alien=`pwd`/alien \
    --prefix=/usr/local/ghc-iphone/
