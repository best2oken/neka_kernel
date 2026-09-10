#include <stdint.h>

static uint16_t *const VGA_MEMORY = (uint16_t *)0xB8000;
static int term_row = 0, term_col = 0;

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}

#define COM1 0x3F8
static void serial_init(void) {
    outb(COM1 + 1, 0x00); outb(COM1 + 3, 0x80);
    outb(COM1 + 0, 0x03); outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03); outb(COM1 + 2, 0xC7);
    outb(COM1 + 4, 0x0B);
}
static void serial_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20));
    outb(COM1, c);
}

// БАГ, найденный на практике (реально НАБЛЮДАЕМАЯ порча вывода на
// UEFI+SMP — два вызова kprint() с разных ядер перемешались посимвольно
// в одну строку): kprint() трогает разделяемое состояние (term_row,
// term_col, сам VGA-буфер, serial-порт) БЕЗ какой-либо синхронизации.
// Один поток мог начать печатать строку, второе ядро — вклиниться
// ПОСЕРЕДИНЕ. Отдельный, специально свой (не NKern_Lock — тот тянет за
// собой семантику reschedule/cli, неуместную для простого вывода,
// и kprint() вызывается в том числе из обработчиков исключений и
// самого раннего boot-кода, где NKern-инфраструктура может быть ещё
// не готова) spinlock именно для консоли.
static volatile int print_lock = 0;
static inline void print_lock_acquire(void) {
    while (__sync_lock_test_and_set(&print_lock, 1)) {
        while (print_lock) { __asm__ volatile ("pause"); }
    }
}
static inline void print_lock_release(void) {
    __sync_lock_release(&print_lock);
}

static void putc_vga(char c) {
    if (c == '\n') {
        term_col = 0;
        term_row++;
        // БАГ, найденный попутно: term_row рос без ограничения (никогда
        // не оборачивался) — после ~25 строк вывода запись уходила ЗА
        // пределы настоящего VGA text-buffer (80x25=2000 ячеек) в
        // соседнюю память. Раньше это работало "случайно" (просто
        // портило что-то за пределами видимого экрана, molча), теперь
        // оборачиваем явно.
        if (term_row >= 25) term_row = 0;
        return;
    }
    VGA_MEMORY[term_row * 80 + term_col] = (uint16_t)c | 0x0A00;
    if (++term_col >= 80) {
        term_col = 0;
        term_row++;
        if (term_row >= 25) term_row = 0;
    }
}

void kprint(const char *s) {
    // cli вокруг лока — на случай если ИСКЛЮЧЕНИЕ (тоже зовущее kprint
    // для отчёта) случится на ЭТОМ ЖЕ ядре ровно в момент, когда оно
    // уже держит print_lock — без cli это был бы самодедлок (ядро
    // бесконечно спинит на СВОЁМ ЖЕ локе).
    uint64_t flags;
    __asm__ volatile ("pushfq; cli; pop %0" : "=r"(flags));
    print_lock_acquire();
    for (int i = 0; s[i]; i++) { putc_vga(s[i]); serial_putc(s[i]); }
    print_lock_release();
    if ((flags >> 9) & 1) { __asm__ volatile ("sti"); }
}

void kprint_hex64(uint64_t val) {
    char buf[19] = "0x0000000000000000";
    const char *hex = "0123456789ABCDEF";
    for (int i = 0; i < 16; i++) {
        buf[17 - i] = hex[val & 0xF];
        val >>= 4;
    }
    kprint(buf);
}

void terminal64_init(void) {
    serial_init();
    for (int y = 0; y < 25; y++)
        for (int x = 0; x < 80; x++)
            VGA_MEMORY[y * 80 + x] = 0x0A00 | ' ';
    term_row = 0; term_col = 0;
}
