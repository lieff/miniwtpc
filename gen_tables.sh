#!/bin/bash
# Regenerate Huffman tables from image dataset
# Usage: bash gen_tables.sh [images_dir]
# Output: prints C array to stdout, logs to stderr
set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DIR="${1:-$SCRIPT_DIR/images}"
cd "$SCRIPT_DIR"
bash build.sh
./wtpc -g "$DIR"
