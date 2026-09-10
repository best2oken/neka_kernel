#include <stdint.h>

// Без этого любая SSE-инструкция (которые GCC свободно генерирует на
// -O2 даже для простых циклов копирования/обнуления памяти) даёт #UD.
// Мы уже наступали на эти грабли в 32-битной сборке (движение VGA-буфера
// компилятор автовекторизировал в movdqa) — тогда обошлись флагом
// -mgeneral-regs-only. Теперь чиним по-настоящему: инициализируем FPU/SSE
// state, как это делает любой настоящий kernel (Linux, EKA2 под ARM
// с VFP — тот же принцип, просто другие регистры) до первого их использования.
void fpu_init(void) {
    uint64_t cr0, cr4;

    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 &= ~(1ULL << 2);   // EM=0 — не эмулировать FPU программно, звать настоящий
    cr0 |=  (1ULL << 1);   // MP=1 — монитор co-processor (нужно для FWAIT/WAIT корректности)
    __asm__ volatile ("mov %0, %%cr0" : : "r"(cr0));

    __asm__ volatile ("mov %%cr4, %0" : "=r"(cr4));
    cr4 |= (1ULL << 9)  |  // OSFXSR — разрешить FXSAVE/FXRSTOR и SSE-инструкции
           (1ULL << 10);   // OSXMMEXCPT — разрешить обычную обработку SIMD FP exceptions (#XM)
    __asm__ volatile ("mov %0, %%cr4" : : "r"(cr4));

    __asm__ volatile ("fninit"); // сброс x87 FPU state в известное состояние
}
