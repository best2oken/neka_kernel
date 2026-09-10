ENTRY(_start)

SECTIONS
{
    . = 1M;

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.multiboot)
        *(.text)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(COMMON)
        *(.bss)
    }

    . = ALIGN(4K);
    kernel_end = .; /* конец нашего загруженного образа — граница, ниже
                        которой memory-map аллокатор не должен раздавать
                        физические фреймы (иначе затрёт сам себя же) */
}
