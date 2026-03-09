#!/bin/bash
# run.sh — Build the kernel, compile the runner, sign it, and start the VM.
#
# Why `stdbuf -oL timeout 3`:
#   The kernel enters WFI after sending output so Apple VZ can process the
#   TX queue on its I/O thread. The guest never calls PSCI SYSTEM_OFF — VZ
#   is still running. We use `timeout 3` to exit after output is delivered.
#   `stdbuf -oL` keeps stdout line-buffered so output isn't lost on exit.
#
# If re-running immediately after a previous run, allow ~2s for VZ to release
# its hypervisor resources before launching a new VM.
set -e

echo "▶ Building kernel..."
bash build.sh

echo "▶ Compiling runner..."
swiftc -framework Virtualization runner.swift -o runner
codesign --entitlements entitlements.plist --force -s - runner

echo "▶ Starting VM..."
stdbuf -oL timeout 3 ./runner || true
echo "▶ Done."
