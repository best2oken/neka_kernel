#include "kernel.h"

extern thread_t *sched_current(void);

static volatile uint32_t counter_a = 0;
static volatile uint32_t counter_b = 0;
static volatile uint32_t counter_c = 0;

static void thread_a(void) {
    for (;;) {
        counter_a++;
        terminal_writestring("[A] tick ");
        terminal_write_hex(counter_a);
        terminal_writestring("\n");
        for (volatile int i = 0; i < 3000000; i++); // имитация работы
        sched_yield(); // аналог кооперативного Reschedule
    }
}

static void thread_b(void) {
    for (;;) {
        counter_b++;
        terminal_writestring("  [B] tick ");
        terminal_write_hex(counter_b);
        terminal_writestring("\n");
        for (volatile int i = 0; i < 3000000; i++);
        sched_yield();
    }
}

static void thread_c_low_priority(void) {
    for (;;) {
        counter_c++;
        terminal_writestring("    [C low-prio] tick ");
        terminal_write_hex(counter_c);
        terminal_writestring("\n");
        for (volatile int i = 0; i < 3000000; i++);
        sched_yield();
    }
}

void kernel_main(uint32_t magic, uint32_t mbi_addr) {
    (void)mbi_addr;
    terminal_init();
    terminal_writestring("========================================\n");
    terminal_writestring("   NEKA — New EKA (bitmap scheduler)   \n");
    terminal_writestring("========================================\n");

    if (magic != 0x2BADB002) {
        terminal_writestring("WARNING: not booted by multiboot loader!\n");
    }

    pic_remap();
    timer_init(100); // 100 Hz, как справочная частота тика в EKA2-подобных системах

    sched_init();

    // Приоритеты как в EKA2: выше число = выше приоритет (bsr выбирает старший бит)
    sched_create_thread(thread_a, 5, "ThreadA");
    sched_create_thread(thread_b, 5, "ThreadB");
    sched_create_thread(thread_c_low_priority, 2, "ThreadC-low");

    terminal_writestring("Starting scheduler...\n\n");

    // Кооперативный запуск: первый reschedule выберет поток с наивысшим
    // приоритетом (bsr по ready_bitmap), дальше потоки сами зовут sched_yield()
    for (;;) {
        sched_reschedule();
        __asm__ volatile ("hlt"); // idle, если вообще нет готовых потоков
    }
}
