#!/usr/bin/env bash
set -euo pipefail

cc -O2 -Wall -Wextra -std=c99 \
  -o tools/wine-dll-file-map-launcher \
  tools/wine-dll-file-map-launcher.c
