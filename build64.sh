#!/bin/bash
set -e
cd "$(dirname "$0")"

mkdir -p build

# -mgeneral-regs-only больше не нужен: fpu_init() включает SSE/FPU state
# ДО любого кода, который GCC мог бы векторизовать через movdqa/movaps.
CFLAGS64="-m64 -ffreestanding -fno-pic -fno-pie -mno-red-zone -fno-stack-protector -O2 -Wall -Wextra -Ikernel"

nasm -f elf64 boot/boot64.s              -o build/boot64.o
nasm -f elf64 kernel/isr.s               -o build/isr.o
nasm -f elf64 kernel/context_switch64.s  -o build/context_switch64.o
nasm -f elf64 kernel/usermode.s          -o build/usermode.o

# AP-трамплин: отдельно собирается как raw binary (-f bin, без ELF), т.к.
# Application Processor стартует в 16-битном real mode по физическому
# адресу vector<<12 — там не может быть никакой ELF-обёртки. Заворачиваем
# получившийся блоб в обычный .o через `ld -r -b binary`, чтобы C-код мог
# сослаться на него как на массив байт (символы _binary_..._start/_end).
nasm -f bin kernel/ap_trampoline.s -o build/ap_trampoline.bin
(cd build && ld -r -b binary -o ap_trampoline_blob.o ap_trampoline.bin)

gcc $CFLAGS64 -c kernel/main64.c     -o build/main64.o
gcc $CFLAGS64 -c kernel/terminal64.c -o build/terminal64.o
gcc $CFLAGS64 -c kernel/timer64.c    -o build/timer64.o
gcc $CFLAGS64 -c kernel/idt.c        -o build/idt.o
gcc $CFLAGS64 -c kernel/fpu.c        -o build/fpu.o
gcc $CFLAGS64 -c kernel/sched64.c    -o build/sched64.o
gcc $CFLAGS64 -c kernel/panic.c      -o build/panic.o
gcc $CFLAGS64 -c kernel/paging64.c   -o build/paging64.o
gcc $CFLAGS64 -c kernel/gdt64.c      -o build/gdt64.o
gcc $CFLAGS64 -c kernel/heap.c       -o build/heap.o
gcc $CFLAGS64 -c kernel/keyboard.c   -o build/keyboard.o
gcc $CFLAGS64 -c kernel/apic.c       -o build/apic.o
gcc $CFLAGS64 -c kernel/memmap.c     -o build/memmap.o
gcc $CFLAGS64 -c kernel/nkern_lock.c -o build/nkern_lock.o
gcc $CFLAGS64 -c kernel/acpi.c       -o build/acpi.o
gcc $CFLAGS64 -c kernel/smp.c        -o build/smp.o

ld -m elf_x86_64 -T linker64.ld -nostdlib -o build/toykernel64.bin \
    build/boot64.o build/main64.o build/terminal64.o build/timer64.o \
    build/idt.o build/fpu.o build/sched64.o build/panic.o build/paging64.o \
    build/gdt64.o build/heap.o build/keyboard.o build/apic.o build/memmap.o \
    build/nkern_lock.o build/acpi.o build/smp.o \
    build/isr.o build/context_switch64.o build/usermode.o build/ap_trampoline_blob.o

echo "64-bit kernel built: build/toykernel64.bin"
file build/toykernel64.bin
