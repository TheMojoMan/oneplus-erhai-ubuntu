#!/bin/bash
# Build Qualcomm's FastRPC userspace (libcdsprpc.so etc.) from the public
# source, staged into $STAGE so nothing is installed into the running system
# before it has been reviewed.
set -x
SRC=/media/data/develop/erhai/tmp/qnn/fastrpc-src
STAGE=/media/data/develop/erhai/tmp/qnn/fastrpc-stage

cd "$SRC" || exit 1
rm -rf "$STAGE"
./gitcompile --prefix=/usr/local || exit 1
make install DESTDIR="$STAGE" || exit 1
echo "=== staged files ==="
find "$STAGE" -type f -o -type l | sort
echo "=== staged libs ==="
ls -la "$STAGE"/usr/local/lib/ 2>/dev/null
