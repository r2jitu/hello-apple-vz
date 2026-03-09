#!/bin/bash
# run.sh — Build the kernel, compile the runner, sign it, and start the VM.
#
# The kernel polls tx_used.idx then calls PSCI SYSTEM_OFF, which triggers
# guestDidStop in the runner so it exits cleanly.
set -e

echo "▶ Building kernel..."
bash build.sh

echo "▶ Compiling runner..."
swiftc -framework Virtualization runner.swift -o runner
codesign --entitlements entitlements.plist --force -s - runner

echo "▶ Starting VM..."
./runner
echo "▶ Done."
