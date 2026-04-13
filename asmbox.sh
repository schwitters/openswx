#!/usr/bin/env bash
if [ ! -d ./build/asmbox_data ]; then
  mkdir ./build/asmbox_data
fi  
TEMPLATES=$(readlink -f ./asmbox/templates)
ASMBOX_PORT=8087 \
ASMBOX_BIND_ADDR=0.0.0.0 \
ASMBOX_DATA_DIR="./build/asmbox_data" \
ASMBOX_TEMPLATE_DIR="$TEMPLATES" \
  ./build/asmbox/asmbox

