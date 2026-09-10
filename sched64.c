#include "kernel64.h"

static scheduler64_t sched64;
static thread64_t threads64[MAX_THREADS64];
static int thread64_count = 0;

// Программный аналог BSR — то же самое, что делает EKA2 в ncsched.cia
// одной инструкцией, чтобы мгновенно найти наивысший готовый приоритет.
static int find_highest_bit64(uint32_t bitmap) {
    if (bitmap == 0) return -1;
    int pos = 31;
    while (!(bitmap & (1u << pos))) pos--;
    return pos;
}

void sched64_init(void) {
    sched64.ready_bitmap = 0;
    for (int i = 0; i < NUM_PRIORITIES64; i++) sched64.ready_queue[i] = 0;
    for (int i = 0; i < MAX_SCHED_CPUS; i++) sched64.current[i] = 0;
    thread64_count = 0;
}

thread64_t *sched64_create_thread(void (*entry)(void), uint8_t priority, const char *name) {
    if (thread64_count >= MAX_THREADS64 || priority >= NUM_PRIORITIES64) return 0;

    thread64_t *t = &threads64[thread64_count++];
    t->priority = priority;
    t->state = T64_READY;
    t->time_slice = 5;
    t->name = name;
    t->cr3 = create_address_space(); // у каждого потока — своё адресное пространство

    // Раскладка стека для НОВОГО потока. ret из context_switch64
    // приземляется на thread_start64 (см. context_switch64.s) — тот
    // делает "sti" (чинит баг с потерянным IF, если поток стартует
    // впервые изнутри прерывания) и сам прыгает на entry.
    //
    // Память (низкий адрес -> высокий): [6 reg-слотов][RFLAGS][thread_start64][entry]
    // context_switch64 теперь потребляет 6 pop'ов регистров + popfq (7
    // pop'ов) + 1 pop через ret (thread_start64) = 8 слотов = 64 байта
    // (RFLAGS добавлен позже — см. context_switch64.s, чинит гонку
    // с NKern_Unlock(), вызывающим reschedule НЕ из прерывания).
    // thread_start64 добавляет ещё 1 pop (entry) = 8 байт. Итого до
    // старта entry() уходит 72 байта.
    //
    // Выравнивание (ABI требует RSP%16==8 на "входе в функцию через call"):
    //   без паддинга: t->rsp = top-72, (0-72)%16=8 — не то, что нужно
    //   с одним паддингом: t->rsp = top-80, (0-80)%16=0 -> final=top-8, %16=8 ✓
    extern void thread_start64(void);
    uint64_t *sp = &t->stack[STACK_SIZE64 / 8];
    sp -= 1;                                   // паддинг под выравнивание
    sp -= 1; *sp = (uint64_t)entry;             // заберёт thread_start64 через pop rax
    sp -= 1; *sp = (uint64_t)thread_start64;    // заберёт context_switch64 через ret
    sp -= 1; *sp = 0x202;                       // RFLAGS для popfq: IF=1, бит1=1 (зарезервирован)
    sp -= 6;                                    // r15,r14,r13,r12,rbx,rbp
    for (int i = 0; i < 6; i++) sp[i] = 0;
    t->rsp = (uint64_t)sp;

    thread64_t *head = sched64.ready_queue[priority];
    if (!head) {
        t->next = t;
        sched64.ready_queue[priority] = t;
    } else {
        thread64_t *tail = head;
        while (tail->next != head) tail = tail->next;
        tail->next = t;
        t->next = head;
    }
    sched64.ready_bitmap |= (1u << priority);

    return t;
}

void sched64_reschedule(void) {
    int c = get_cpu_index();

    // БАГ #1 (см. README): раньше pick+update разделяемых
    // ready_bitmap/ready_queue[]/current было НИЧЕМ не защищено между
    // ядрами.
    //
    // БАГ #3, найденный ПОЗЖЕ (реальный краш при более равномерном
    // распределении работы BSP/AP): здесь раньше стоял NKern_Unlock(),
    // отпускающий лок И включающий прерывания ДО фактического
    // context_switch64()! Это делало prev->state=READY видимым другим
    // ядрам ДО того как prev->rsp реально сохранён — другое ядро могло
    // попытаться запустить prev по УСТАРЕВШЕМУ значению rsp. Фикс:
    // NKern_LockForSwitch()/NKern_UnlockAfterSwitch() — разблокировка
    // происходит ТОЛЬКО после того как переключение реально случилось
    // (см. context_switch64.s: thread_start64 для новых потоков, и
    // код ниже — для возобновляемых).
    NKern_LockForSwitch();

    int prio = find_highest_bit64(sched64.ready_bitmap);
    if (prio < 0) { NKern_UnlockAfterSwitch(); return; } // нет готовых потоков — остаёмся в idle

    thread64_t *head = sched64.ready_queue[prio];
    thread64_t *next = head;
    thread64_t *prev = sched64.current[c];

    // Round-robin внутри приоритета: крутим кольцо, если таймслайс истёк
    // и текущий поток (head) — это тот же приоритет
    if (head == prev && head->next != head) {
        next = head->next;
        sched64.ready_queue[prio] = next;
    }

    if (next == prev) { NKern_UnlockAfterSwitch(); return; }

    sched64.current[c] = next;
    next->state = T64_RUNNING;
    if (prev) prev->state = T64_READY;

    // Переключаем CR3 только если адресное пространство реально другое —
    // перезагрузка CR3 сбрасывает весь TLB (дорого), ровно та же
    // оптимизация, что делает iProcessHandler в настоящем EKA2 ncsched.cia
    // ("test byte [ebx+iSpare2],2" перед переключением page tables).
    // БАГ, найденный на практике: loaded_cr3 раньше было ОДНО глобальное
    // значение на все CPU — CR3 это per-core регистр, кэш его значения
    // обязан быть per-core тоже, иначе одно ядро может "решить", что
    // CR3 уже там где надо, основываясь на том что ДРУГОЕ ядро туда
    // переключилось, хотя ЭТОГО конкретного ядра регистр совсем другой.
    static uint64_t loaded_cr3[MAX_SCHED_CPUS];
    int need_cr3_switch = (next->cr3 != loaded_cr3[c]);
    if (need_cr3_switch) loaded_cr3[c] = next->cr3;

    if (need_cr3_switch) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(next->cr3) : "memory");
    }

    static uint64_t dummy_rsp[MAX_SCHED_CPUS];
    uint64_t *prev_rsp_ptr = prev ? &prev->rsp : &dummy_rsp[c];
    context_switch64(prev_rsp_ptr, next->rsp);

    // Сюда попадаем ТОЛЬКО когда "prev" (текущий на момент ЭТОГО
    // вызова) снова выбран планировщиком, и context_switch64() вернул
    // управление — лок/cli всё ещё в силе (захвачены ЭТИМ ЖЕ вызовом
    // до свитча) — отпускаем ИМЕННО ТЕПЕРЬ, когда prev->rsp уже точно
    // сохранён и это безопасно.
    NKern_UnlockAfterSwitch();
}

// Вызывается из IRQ0 (interrupt_dispatch, vector==32) — НАСТОЯЩИЙ
// preemptive tick: в отличие от 32-битной кооперативной версии, здесь
// поток прерывается таймером сам, без добровольного sched_yield().
// Если в этот момент держится NKern-лок (критическая секция вроде
// kmalloc/kfree) — reschedule ОТКЛАДЫВАЕТСЯ до NKern_Unlock(), а не
// выполняется прямо тут поверх незавершённой операции.
void sched64_timer_tick(void) {
    thread64_t *cur = sched64.current[get_cpu_index()];
    if (!cur) return;
    if (cur->time_slice > 0 && --cur->time_slice == 0) {
        cur->time_slice = 5;
        if (NKern_IsLocked()) {
            NKern_RequestReschedule();
        } else {
            sched64_reschedule();
        }
    }
}
