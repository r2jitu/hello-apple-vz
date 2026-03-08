#!/bin/bash
# Build and run the bare-metal Apple VZ kernel.
# Use stdbuf -oL to ensure runner output isn't buffered when killed by timeout.
set -e

echo "=== Building kernel ==="
bash build.sh

echo "=== Compiling and signing runner ==="
swiftc -framework Virtualization runner.swift -o runner
codesign --entitlements entitlements.plist --force -s - runner

echo "=== Running VM ==="
stdbuf -oL ./runner
