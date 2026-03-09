#!/bin/bash
# run.sh — Build the kernel, compile the runner, sign it, and start the VM.
#
# The kernel enters WFI after sending output so Apple VZ's I/O thread can
# deliver the console data to the pipe. `stdbuf -oL timeout 3` ensures:
#   - stdout is line-buffered (data appears immediately)
#   - the process is terminated 3 seconds after the VM starts
set -e

echo "▶ Building kernel..."
bash build.sh

echo "▶ Compiling runner..."
swiftc -framework Virtualization runner.swift -o runner
codesign --entitlements entitlements.plist --force -s - runner

echo "▶ Starting VM..."
stdbuf -oL timeout 3 ./runner || true
echo "▶ Done."
