#include "kernel.h"

static uint16_t *const VGA_MEMORY = (uint16_t *)0xB8000;
static const int VGA_WIDTH = 80;
static const int VGA_HEIGHT = 25;
static int term_row = 0;
static int term_col = 0;
static uint8_t term_color = 0x0A; // светло-зелёный на чёрном

static uint16_t vga_entry(char c, uint8_t color) {
    return (uint16_t)c | (uint16_t)color << 8;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}

#define COM1 0x3F8
static void serial_init(void) {
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03);
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);
    outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}
static void serial_putchar(char c) {
    while (!(inb(COM1 + 5) & 0x20));
    outb(COM1, c);
}

void terminal_init(void) {
    serial_init();
    for (int y = 0; y < VGA_HEIGHT; y++)
        for (int x = 0; x < VGA_WIDTH; x++)
            VGA_MEMORY[y * VGA_WIDTH + x] = vga_entry(' ', term_color);
    term_row = 0;
    term_col = 0;
}

static void terminal_newline(void) {
    term_col = 0;
    term_row++;
    if (term_row >= VGA_HEIGHT) term_row = 0; // просто заворачиваем, без скролла (упрощение)
}

void terminal_putchar(char c) {
    if (c == '\n') { terminal_newline(); return; }
    VGA_MEMORY[term_row * VGA_WIDTH + term_col] = vga_entry(c, term_color);
    if (++term_col >= VGA_WIDTH) terminal_newline();
}

void terminal_writestring(const char *str) {
    for (int i = 0; str[i] != '\0'; i++) {
        terminal_putchar(str[i]);
        serial_putchar(str[i]);
    }
}

void terminal_write_hex(uint32_t val) {
    char buf[11] = "0x00000000";
    const char *hex = "0123456789ABCDEF";
    for (int i = 0; i < 8; i++) {
        buf[9 - i] = hex[val & 0xF];
        val >>= 4;
    }
    terminal_writestring(buf);
}
