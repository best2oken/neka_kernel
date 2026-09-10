#ifndef KERNEL_H
#define KERNEL_H

#include <stdint.h>

#define MAX_THREADS   16
#define NUM_PRIORITIES 8      // как KNumPriorities в EKA2
#define STACK_SIZE    4096

typedef enum {
    THREAD_UNUSED = 0,
    THREAD_READY,
    THREAD_RUNNING,
    THREAD_SUSPENDED
} thread_state_t;

// Аналог NThreadBase из EKA2 (сильно упрощённый)
typedef struct thread {
    uint32_t   esp;                 // сохранённый stack pointer (как iSavedSP)
    uint8_t    priority;            // приоритет потока (0..NUM_PRIORITIES-1)
    thread_state_t state;
    uint32_t   time_slice;          // остаток таймслайса (как iTime)
    uint32_t   stack[STACK_SIZE / 4];
    const char *name;
    struct thread *next;            // аналог SDblQueLink — кольцевой список потоков одного приоритета
} thread_t;

// Аналог TScheduler из EKA2: битовая карта готовых приоритетов + очереди
typedef struct {
    uint32_t ready_bitmap;                    // бит N = есть готовый поток приоритета N
    thread_t *ready_queue[NUM_PRIORITIES];    // по одному потоку на приоритет (упрощение)
    thread_t *current;
} scheduler_t;

void kernel_main(uint32_t magic, uint32_t mbi_addr);

void sched_init(void);
thread_t *sched_create_thread(void (*entry)(void), uint8_t priority, const char *name);
void sched_reschedule(void);          // аналог TScheduler::Reschedule()
void sched_yield(void);               // аналог TScheduler::YieldTo()

void terminal_init(void);
void terminal_writestring(const char *str);
void terminal_write_hex(uint32_t val);

void timer_init(uint32_t freq);
void pic_remap(void);

extern void context_switch(uint32_t *old_esp, uint32_t new_esp);

#endif
