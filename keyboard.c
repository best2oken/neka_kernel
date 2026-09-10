#include <stdint.h>

extern void kprint(const char *s);

static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}

// PS/2 scancode set 1 (стандарт для i8042 в режиме, который использует
// QEMU/большинство реального железа по умолчанию) -> ASCII, US QWERTY.
// Индекс — сам scancode (make-код, старший бит=0). Break-коды (отпускание
// клавиши, старший бит=1 — т.е. scancode+0x80) сюда не входят, их
// обрабатываем отдельно по значению самого байта.
static const char scancode_ascii[128] = {
    0,  27, '1','2','3','4','5','6','7','8','9','0','-','=','\b',
    '\t','q','w','e','r','t','y','u','i','o','p','[',']','\n',
    0, /* left ctrl */
    'a','s','d','f','g','h','j','k','l',';','\'','`',
    0, /* left shift */
    '\\','z','x','c','v','b','n','m',',','.','/',
    0, /* right shift */
    '*',
    0, /* alt */
    ' ', /* space */
    0, /* caps lock */
    0,0,0,0,0,0,0,0,0,0, /* F1-F10 */
    0, /* num lock */
    0, /* scroll lock */
    0,0,0,0,0,0,0,0,0, /* keypad, home, etc — не обрабатываем */
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

static const char scancode_ascii_shift[128] = {
    0,  27, '!','@','#','$','%','^','&','*','(',')','_','+','\b',
    '\t','Q','W','E','R','T','Y','U','I','O','P','{','}','\n',
    0,
    'A','S','D','F','G','H','J','K','L',':','"','~',
    0,
    '|','Z','X','C','V','B','N','M','<','>','?',
    0,'*',0,' ',0,
    0,0,0,0,0,0,0,0,0,0,0,0,
    0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

#define KBD_SCANCODE_LSHIFT 0x2A
#define KBD_SCANCODE_RSHIFT 0x36

static int shift_held = 0;

// Простой кольцевой буфер введённых символов — реальные драйверы обычно
// именно так развязывают "быстрый" IRQ-обработчик (только читает порт и
// кладёт в буфер) от "медленного" потребителя (кто-то читает из буфера
// когда ему удобно, не блокируя сам IRQ).
#define KBD_BUF_SIZE 256
static char kbd_buffer[KBD_BUF_SIZE];
static volatile int kbd_head = 0, kbd_tail = 0;

static void kbd_buffer_push(char c) {
    int next = (kbd_head + 1) % KBD_BUF_SIZE;
    if (next == kbd_tail) return; // буфер полон — молча теряем (как и многие реальные драйверы при переполнении)
    kbd_buffer[kbd_head] = c;
    kbd_head = next;
}

int kbd_buffer_pop(char *out) {
    if (kbd_tail == kbd_head) return 0; // пусто
    *out = kbd_buffer[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return 1;
}

// Вызывается из interrupt_dispatch() на векторе 33 (IRQ1). Это и есть
// "быстрая" часть драйвера — только чтение порта и трансляция, никакого
// тяжёлого кода (в реальном ядре сюда бы не стали класть, скажем,
// парсинг команд — ровно поэтому и нужен буфер).
void keyboard_handle_irq(void) {
    uint8_t scancode = inb(0x60);

    if (scancode == KBD_SCANCODE_LSHIFT || scancode == KBD_SCANCODE_RSHIFT) {
        shift_held = 1;
        return;
    }
    if (scancode == (KBD_SCANCODE_LSHIFT | 0x80) || scancode == (KBD_SCANCODE_RSHIFT | 0x80)) {
        shift_held = 0;
        return;
    }
    if (scancode & 0x80) return; // break-код прочих клавиш — игнорируем

    char c = shift_held ? scancode_ascii_shift[scancode] : scancode_ascii[scancode];
    if (c != 0) {
        kbd_buffer_push(c);
        // Печатаем сразу же для наглядности демо — в настоящей ОС этим
        // занимался бы terminal driver, читающий из буфера отдельно.
        char buf[2] = {c, 0};
        kprint("[kbd] key: '");
        kprint(buf);
        kprint("'\n");
    }
}
