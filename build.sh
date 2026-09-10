#!/bin/bash
set -e
cd "$(dirname "$0")"

CFLAGS="-m32 -ffreestanding -fno-pie -fno-stack-protector -O2 -Wall -Wextra -Ikernel"
LDFLAGS="-m elf_i386 -T linker.ld -nostdlib"

mkdir -p build

nasm -f elf32 boot/boot.s -o build/boot.o
nasm -f elf32 kernel/context_switch.s -o build/context_switch.o

gcc $CFLAGS -c kernel/main.c -o build/main.o
gcc $CFLAGS -c kernel/terminal.c -o build/terminal.o
gcc $CFLAGS -c kernel/sched.c -o build/sched.o
gcc $CFLAGS -c kernel/timer.c -o build/timer.o

ld $LDFLAGS -o build/toykernel.bin \
    build/boot.o build/main.o build/terminal.o build/sched.o build/timer.o build/context_switch.o

echo "Kernel built: build/toykernel.bin"
file build/toykernel.bin
