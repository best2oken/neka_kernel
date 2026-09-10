#!/bin/bash
set -e
cd "$(dirname "$0")"

mkdir -p build_uefi

EFI_INC=/usr/include/efi
EFI_LIB=/usr/lib

# Флаги, которых требует gnu-efi: -fpic (PE использует таблицу релокаций,
# а не настоящий PIC в смысле Linux, но так строятся все gnu-efi проекты),
# -DEFI_FUNCTION_WRAPPER — раз мы вызываем UEFI-функции через
# uefi_call_wrapper (совместимо с любым gcc, без завязки на MS ABI).
CFLAGS_EFI="-m64 -fpic -ffreestanding -fshort-wchar -fno-stack-protector -fno-strict-aliasing \
    -fno-merge-constants -maccumulate-outgoing-args -mno-red-zone \
    -DEFI_FUNCTION_WRAPPER -I$EFI_INC -I$EFI_INC/x86_64 -I. -Ikernel -Wall -Wextra"

nasm -f elf64 kernel/isr.s               -o build_uefi/isr.o
nasm -f elf64 kernel/context_switch64.s  -o build_uefi/context_switch64.o
nasm -f elf64 kernel/usermode.s          -o build_uefi/usermode.o

# AP-трамплин (см. build64.sh — тот же принцип: raw binary, завёрнутый
# в .o через ld -r -b binary). БАГ, найденный при сборке: этот шаг и
# компиляция acpi.c/smp.c раньше отсутствовали в UEFI-сборке вообще —
# main64.c ссылается на acpi_find_rsdp_bios()/smp_start_aps() и т.д.,
# но `ld -shared` для UEFI не жалуется на undefined-символы (в отличие
# от обычной статической линковки в build64.sh) — сборка "успешно"
# завершалась, но реально упала бы в runtime на первом же вызове через
# неразрешённый GOT/PLT. Проверяется через `nm -u` — см. README.
nasm -f bin kernel/ap_trampoline.s -o build_uefi/ap_trampoline.bin
(cd build_uefi && ld -r -b binary -o ap_trampoline_blob.o ap_trampoline.bin)

gcc $CFLAGS_EFI -c kernel/uefi_main.c  -o build_uefi/uefi_main.o
gcc $CFLAGS_EFI -c kernel/main64.c     -o build_uefi/main64.o
gcc $CFLAGS_EFI -c kernel/terminal64.c -o build_uefi/terminal64.o
gcc $CFLAGS_EFI -c kernel/timer64.c    -o build_uefi/timer64.o
gcc $CFLAGS_EFI -c kernel/idt.c        -o build_uefi/idt.o
gcc $CFLAGS_EFI -c kernel/fpu.c        -o build_uefi/fpu.o
gcc $CFLAGS_EFI -c kernel/sched64.c    -o build_uefi/sched64.o
gcc $CFLAGS_EFI -c kernel/panic.c      -o build_uefi/panic.o
gcc $CFLAGS_EFI -c kernel/paging64.c   -o build_uefi/paging64.o
gcc $CFLAGS_EFI -c kernel/gdt64.c      -o build_uefi/gdt64.o
gcc $CFLAGS_EFI -c kernel/heap.c       -o build_uefi/heap.o
gcc $CFLAGS_EFI -c kernel/keyboard.c   -o build_uefi/keyboard.o
gcc $CFLAGS_EFI -c kernel/apic.c       -o build_uefi/apic.o
gcc $CFLAGS_EFI -c kernel/memmap.c     -o build_uefi/memmap.o
gcc $CFLAGS_EFI -c kernel/nkern_lock.c -o build_uefi/nkern_lock.o
gcc $CFLAGS_EFI -c kernel/acpi.c       -o build_uefi/acpi.o
gcc $CFLAGS_EFI -c kernel/smp.c        -o build_uefi/smp.o

# Линкуем как обычный ELF (пока не PE) — crt0-efi-x86_64.o даёт настоящую
# точку входа _start, которая делает EFI-специфичный пролог и зовёт наш
# efi_main(). Используем родной linker script gnu-efi.
ld -shared -Bsymbolic -nostdlib -znocombreloc \
    -T elf_x86_64_efi_neka.lds \
    $EFI_LIB/crt0-efi-x86_64.o \
    build_uefi/uefi_main.o build_uefi/main64.o build_uefi/terminal64.o \
    build_uefi/timer64.o build_uefi/idt.o build_uefi/fpu.o build_uefi/sched64.o \
    build_uefi/panic.o build_uefi/paging64.o build_uefi/gdt64.o build_uefi/heap.o \
    build_uefi/keyboard.o build_uefi/apic.o build_uefi/memmap.o build_uefi/nkern_lock.o \
    build_uefi/acpi.o build_uefi/smp.o \
    build_uefi/isr.o build_uefi/context_switch64.o \
    build_uefi/usermode.o build_uefi/ap_trampoline_blob.o \
    -L$EFI_LIB -lefi -lgnuefi \
    -o build_uefi/neka.so

# Проверка на неразрешённые символы — ld -shared молча пропускает их,
# в отличие от обычной статической линковки, поэтому проверяем явно,
# чтобы не повторить баг, из-за которого acpi.c/smp.c когда-то выпали
# из этого скрипта незамеченными.
UNDEFINED=$(nm -u build_uefi/neka.so 2>/dev/null | grep -v " U _" | grep -vE "efi_main|__stack_chk" || true)
if [ -n "$UNDEFINED" ]; then
    echo "ERROR: undefined symbols in UEFI build:"
    echo "$UNDEFINED"
    exit 1
fi

# ELF -> PE32+ EFI application. Именно этот шаг превращает обычный
# relocatable ELF в формат, который UEFI firmware реально умеет грузить
# (PE32+ с EFI_SUBSYSTEM_APPLICATION) — без него получили бы просто .so.
objcopy -j .text -j .sdata -j .data -j .dynamic -j .dynsym -j .rel \
    -j .rela -j .reloc -j .eh_frame \
    --target=efi-app-x86_64 build_uefi/neka.so build_uefi/BOOTX64.EFI

echo "UEFI kernel built: build_uefi/BOOTX64.EFI"
file build_uefi/BOOTX64.EFI
