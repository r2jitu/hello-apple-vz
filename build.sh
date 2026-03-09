#!/bin/bash
set -e

clang --target=aarch64-none-elf -c boot.S -o boot.o
clang --target=aarch64-none-elf -ffreestanding -nostdlib -mno-red-zone -c kernel.c -o kernel.o
ld.lld -T linker.ld --Bstatic --oformat binary boot.o kernel.o -o kernel.bin

echo "Built kernel.bin ($(wc -c < kernel.bin | tr -d ' ') bytes)"
