#!/bin/bash

set -v

BUILD_DIR=$(pwd)
WAVM_DIR=$(cd `dirname $0`/../.. && pwd)

cd $BUILD_DIR

ninja

rm -rf wast-seed-corpus
rm -rf wasm-seed-corpus

mkdir wast-seed-corpus

find $WAVM_DIR/Test/spec \
  -iname *.wast -not -iname skip-stack-guard-page.wast -not -iname br_table.wast \
  | ASAN_OPTIONS=detect_leaks=0 xargs -n1 bin/DumpTestModules --wast --output-dir ./wast-seed-corpus 

find $WAVM_DIR/Test/WAVM \
  -iname *.wast \
  | ASAN_OPTIONS=detect_leaks=0 xargs -n1 bin/DumpTestModules --wast --output-dir ./wast-seed-corpus 

mkdir wasm-seed-corpus

find $WAVM_DIR/Test/spec \
  -iname *.wast -not -iname skip-stack-guard-page.wast -not -iname br_table.wast \
  | ASAN_OPTIONS=detect_leaks=0 xargs -n1 bin/DumpTestModules --wasm --output-dir ./wasm-seed-corpus 

find $WAVM_DIR/Test/WAVM \
  -iname *.wast \
  | ASAN_OPTIONS=detect_leaks=0 xargs -n1 bin/DumpTestModules --wasm --output-dir ./wasm-seed-corpus 
