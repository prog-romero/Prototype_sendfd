#!/usr/bin/env bash

# Usage: ./collect_repo.sh <directory> [output_file]

set -euo pipefail

INPUT_DIR="${1:-}"
OUTPUT_FILE="${2:-file.txt}"

if [[ -z "$INPUT_DIR" ]]; then
  echo "Usage: $0 <directory> [output_file]"
  exit 1
fi

if [[ ! -d "$INPUT_DIR" ]]; then
  echo "Error: '$INPUT_DIR' is not a directory"
  exit 1
fi

# Clear output file
> "$OUTPUT_FILE"

echo "Collecting selected files from: $INPUT_DIR"
echo "Output file: $OUTPUT_FILE"

# Find only desired file types
find "$INPUT_DIR" -type f \( \
    -name "*.c" -o \
    -name "*.go" -o \
    -name "*.sh" -o \
    -name "*.py" -o \
    -iname "README*" \
\) | while read -r file; do

  echo "===== FILE: $file =====" >> "$OUTPUT_FILE"
  cat "$file" >> "$OUTPUT_FILE"
  echo -e "\n\n" >> "$OUTPUT_FILE"

done

echo "Done. Filtered contents saved in $OUTPUT_FILE"