#!/usr/bin/env bash
set -euo pipefail

# --- Configuration (Matching your exact folder structures) ---
SDK_ARCHIVE="openwrt-sdk-25.12.2-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64.tar.zst"
SDK_DIR="openwrt-sdk-25.12.2-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64"

echo "=== Verifying Local Environment ==="

# --- 1. Ensure the SDK folder exists ---
if [ ! -d "$SDK_DIR" ]; then
    if [ -f "$SDK_ARCHIVE" ]; then
        echo "SDK directory not found, but archive exists. Extracting..."
        tar -I zstd -xf "$SDK_ARCHIVE"
    else
        echo "Error: Neither '$SDK_DIR' folder nor '$SDK_ARCHIVE' found in this directory!" >&2
        exit 1
    fi
else
    echo "Found local SDK environment directory: $SDK_DIR"
fi

# --- 2. Locate Cross-Compiler ---
echo "Locating cross-compiler toolchain..."
CROSS_GCC=$(find "$SDK_DIR/staging_dir" -type f -name '*-gcc' | head -n1)

if [ -z "$CROSS_GCC" ]; then
    echo "Error: Could not find cross-compiler within '$SDK_DIR/staging_dir'." >&2
    exit 1
fi

# Strip 'gcc' off the end to get the toolchain environment prefix path
TOOLCHAIN_PREFIX="${CROSS_GCC%gcc}"
echo "Found toolchain prefix: $TOOLCHAIN_PREFIX"

# --- 3. Compile ---
echo "=== Compiling sq-osc-bridge ==="
"${TOOLCHAIN_PREFIX}gcc" -Os -Wall -Wextra -o sq-osc-bridge src/main.c

# --- 4. Verify Output ---
echo "=== Verifying Binary ==="
if [ -f sq-osc-bridge ]; then
    file sq-osc-bridge
    echo "Success! Your cross-compiled MIPS binary is ready."
else
    echo "Error: Binary was not generated." >&2
    exit 1
fi
