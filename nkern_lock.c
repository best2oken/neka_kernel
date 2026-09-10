#include <stdint.h>

extern uint8_t lapic_get_id(void);
extern void sched64_reschedule(void);

// БАГ 1 (см. README): один kern_cs_locked без atomics — не давал
// настоящего взаимного исключения между ядрами.
// БАГ 2: reschedule() из NKern_Unlock() шёл не из прерывания, context_switch64
// мог быть прерван посередине — исправлено через cli/sti с сохранением IF.
//
// БАГ 3, найденный ПОСЛЕ этого (реальный краш при более равномерном
// распределении работы между BSP/AP на UEFI+SMP): sched64_reschedule()
// отпускала лок И включала прерывания ДО фактического context_switch64()!
// Это открывало окно, где:
//   - prev->state помечался READY, и другое ядро могло попытаться
//     запустить prev, хотя РЕАЛЬНЫЙ context_switch64() (который
//     сохраняет prev->rsp) ещё не произошёл — prev->rsp оставался
//     СТАРЫМ, другое ядро прыгнуло бы на невалидный/устаревший стек.
//   - если прерывания уже были включены В ЭТОТ МОМЕНТ, таймер того же
//     ядра мог прервать ЕЩЁ ДО context_switch64() и рекурсивно вызвать
//     reschedule() снова, пока мы "всё ещё технически" переключаемся.
// Фикс: разблокировка должна происходить ПОСЛЕ того как переключение
// РЕАЛЬНО случилось — то есть либо из thread_start64 (для только что
// созданных потоков), либо сразу после возврата из context_switch64()
// внутри sched64_reschedule() (для возобновляемых). Отсюда — отдельная
// пара функций специально для планировщика, с БЕЗУСЛОВНЫМ sti (обычные
// потоки всегда хотят прерывания включёнными после переключения — в
// отличие от NKern_Lock/Unlock общего назначения, которым нужно
// восстанавливать ИСХОДНОЕ состояние IF вызывающего кода).
extern int get_cpu_index(void);
#define MAX_CPUS 16

static volatile int preempt_disable_count[MAX_CPUS];
static volatile int reschedule_pending[MAX_CPUS];
static volatile int saved_if[MAX_CPUS];
static volatile int big_kernel_lock = 0;

static inline int cpu_idx(void) {
    return get_cpu_index();
}

static inline void spin_acquire(volatile int *lock) {
    while (__sync_lock_test_and_set(lock, 1)) {
        while (*lock) { __asm__ volatile ("pause"); }
    }
}
static inline void spin_release(volatile int *lock) {
    __sync_lock_release(lock);
}

void NKern_Lock(void) {
    int c = cpu_idx();
    uint64_t flags;
    __asm__ volatile ("pushfq; cli; pop %0" : "=r"(flags));

    if (preempt_disable_count[c] == 0) {
        saved_if[c] = (int)((flags >> 9) & 1);
    }
    preempt_disable_count[c]++;
    if (preempt_disable_count[c] == 1) {
        spin_acquire(&big_kernel_lock);
    }
}

void NKern_Unlock(void) {
    int c = cpu_idx();
    if (preempt_disable_count[c] > 0) preempt_disable_count[c]--;
    if (preempt_disable_count[c] == 0) {
        spin_release(&big_kernel_lock);
        if (reschedule_pending[c]) {
            reschedule_pending[c] = 0;
            sched64_reschedule();
        }
        if (saved_if[c]) {
            __asm__ volatile ("sti");
        }
    }
}

int NKern_IsLocked(void) {
    return preempt_disable_count[cpu_idx()] > 0;
}

void NKern_RequestReschedule(void) {
    reschedule_pending[cpu_idx()] = 1;
}

// --- Специальная пара для sched64_reschedule() (см. БАГ 3 выше) ---

void NKern_LockForSwitch(void) {
    int c = cpu_idx();
    __asm__ volatile ("cli"); // безусловно — планировщику не нужна semантика "восстановить исходный IF"
    preempt_disable_count[c]++;
    if (preempt_disable_count[c] == 1) {
        spin_acquire(&big_kernel_lock);
    }
}

// Вызывается ТОЛЬКО после того как context_switch64() реально
// произошёл — либо из thread_start64 (новый поток), либо сразу после
// возврата из context_switch64() внутри sched64_reschedule()
// (возобновившийся поток). К этому моменту prev->rsp уже сохранён
// (или мы и есть только что созданный поток, которому "сохранять"
// нечего) — можно безопасно делать другой поток видимым для других
// ядер и включать прерывания.
void NKern_UnlockAfterSwitch(void) {
    int c = cpu_idx();
    if (preempt_disable_count[c] > 0) preempt_disable_count[c]--;
    if (preempt_disable_count[c] == 0) {
        spin_release(&big_kernel_lock);
        __asm__ volatile ("sti"); // безусловно — обычные потоки всегда хотят прерывания включёнными
    }
}
