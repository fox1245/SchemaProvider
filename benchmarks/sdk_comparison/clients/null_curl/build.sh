#!/bin/sh
# usage: [CURL_PREFIX=DIR] build.sh OUTPUT_PATH
# Builds against the system libcurl, or against the libcurl installed in CURL_PREFIX (found through its rpath).
set -eu
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$(dirname "$1")"
if [ -n "${CURL_PREFIX:-}" ]; then
  ${CC:-gcc} -O2 -std=gnu11 -Wall -Wextra -o "$1" "$here/null_client.c" -I"$CURL_PREFIX/include" -L"$CURL_PREFIX/lib" -Wl,-rpath,"$CURL_PREFIX/lib" -lcurl
else
  ${CC:-gcc} -O2 -std=gnu11 -Wall -Wextra -o "$1" "$here/null_client.c" $(curl-config --cflags) $(curl-config --libs)
fi
