#!/bin/bash
set -e

echo "Compiling boot.S..."
clang --target=aarch64-none-elf -c boot.S -o boot.o

echo "Compiling kernel.c..."
clang --target=aarch64-none-elf -ffreestanding -nostdlib -mno-red-zone -c kernel.c -o kernel.o

echo "Linking kernel into a flat binary..."
# Using LLD directly with --oformat binary to avoid needing llvm-objcopy
ld.lld -T linker.ld --Bstatic --oformat binary boot.o kernel.o -o kernel.bin

echo "Successfully built kernel.bin."
