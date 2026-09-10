#include "kernel.h"

static scheduler_t the_scheduler; // аналог TheScheduler в EKA2 (тоже в .bss)
static thread_t threads[MAX_THREADS];
static int thread_count = 0;

// Быстрый поиск старшего установленного бита — программный аналог
// инструкции BSR, которую EKA2 использует в ncsched.cia для мгновенного
// выбора наивысшего готового приоритета без цикла по всем уровням.
static int find_highest_bit(uint32_t bitmap) {
    if (bitmap == 0) return -1;
    int pos = 31;
    while (!(bitmap & (1u << pos))) pos--;
    return pos;
}

void sched_init(void) {
    the_scheduler.ready_bitmap = 0;
    for (int i = 0; i < NUM_PRIORITIES; i++)
        the_scheduler.ready_queue[i] = 0;
    the_scheduler.current = 0;
    thread_count = 0;
}

thread_t *sched_create_thread(void (*entry)(void), uint8_t priority, const char *name) {
    if (thread_count >= MAX_THREADS || priority >= NUM_PRIORITIES) return 0;

    thread_t *t = &threads[thread_count++];
    t->priority = priority;
    t->state = THREAD_READY;
    t->time_slice = 5; // 5 тиков таймера, как timeslice в EKA2
    t->name = name;

    // Раскладка стека должна ТОЧНО совпадать с порядком pop в context_switch.s:
    //   pop ebp; pop edi; pop esi; pop ebx; pop ebp; ret
    // То есть нужно 5 "регистровых" слотов (значения не важны при первом
    // запуске — реальных сохранённых регистров ещё не было) и затем адрес
    // возврата — сюда кладём сам entry(), ret прыгнет прямо на него.
    uint32_t *sp = &t->stack[STACK_SIZE / 4];
    sp -= 1; *sp = (uint32_t)entry;   // слот, который заберёт "ret" — прыжок на entry
    sp -= 5;                          // 5 слотов под ebp,edi,esi,ebx,ebp
    for (int i = 0; i < 5; i++) sp[i] = 0;
    t->esp = (uint32_t)sp;

    // Регистрируем поток в кольцевой очереди готовности приоритета — как
    // TScheduler::Add в EKA2 добавляет поток в iQueue[priority] (SDblQueLink)
    thread_t *head = the_scheduler.ready_queue[priority];
    if (!head) {
        t->next = t;                     // кольцо из одного элемента
        the_scheduler.ready_queue[priority] = t;
    } else {
        // вставляем перед head, т.е. в конец кольца
        thread_t *tail = head;
        while (tail->next != head) tail = tail->next;
        tail->next = t;
        t->next = head;
    }
    the_scheduler.ready_bitmap |= (1u << priority);

    return t;
}

static void thread_trampoline(void) {
    // после первого context_switch поток "как будто" вызвал entry() сам
    // (в реальном EKA2 это делает нижний уровень NThread::_ThreadStart)
    __asm__ volatile ("sti"); // разрешаем прерывания перед первым запуском
    for (;;) { __asm__ volatile ("hlt"); } // не должно доходить: entry() сам не вернётся
}

// Аналог TScheduler::Reschedule() из ncsched.cia, но на C:
// 1) находим наивысший приоритет с готовым потоком (BSR по битовой карте)
// 2) если это не текущий поток — переключаем контекст
void sched_reschedule(void) {
    int prio = find_highest_bit(the_scheduler.ready_bitmap);
    if (prio < 0) return; // нет готовых потоков — остаёмся в idle (hlt в _start)

    thread_t *head = the_scheduler.ready_queue[prio];
    thread_t *next = head;
    thread_t *prev = the_scheduler.current;

    // Если у текущего потока (равного head) кончился таймслайс и есть
    // другие потоки того же приоритета — крутим кольцо (round_robin: в EKA2)
    if (head == prev && head->next != head) {
        next = head->next;
        the_scheduler.ready_queue[prio] = next; // новый head кольца
    }

    if (next == prev) return; // как resched_not_needed в EKA2

    the_scheduler.current = next;
    next->state = THREAD_RUNNING;
    if (prev) prev->state = THREAD_READY;

    static uint32_t dummy_esp;
    uint32_t *prev_esp_ptr = prev ? &prev->esp : &dummy_esp;
    context_switch(prev_esp_ptr, next->esp);
}

void sched_yield(void) {
    // Аналог TScheduler::YieldTo(): просто просим reschedule
    sched_reschedule();
}

thread_t *sched_current(void) {
    return the_scheduler.current;
}

// Вызывается из обработчика таймера (PIT IRQ0) — аналог TimesliceTick()
void sched_timer_tick(void) {
    thread_t *c = the_scheduler.current;
    if (!c) return;
    if (c->time_slice > 0 && --c->time_slice == 0) {
        c->time_slice = 5; // новый таймслайс, как в TScheduler::Remove
        sched_reschedule();
    }
}
