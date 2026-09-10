#include <stdint.h>

// TSS в 64-битном режиме используется CPU НЕ для переключения задач
// (как в 32-бит), а только для одной вещи, которая нам критична: когда
// прерывание/исключение случается, пока CPU был в ring3, CPU обязан
// переключиться на ДРУГОЙ стек перед тем как толкать туда что-либо —
// он берёт этот стек из TSS.RSP0. Без настоящего TSS ring3-код в
// принципе не может безопасно вызвать прерывание (клавиатура, таймер,
// syscall) — CPU просто не будет знать, на какой стек переключиться.
//
// БАГ, найденный на практике (реальный краш на UEFI+SMP): TSS — это
// per-CPU состояние (у каждого ядра свой Task Register), но раньше
// была только ОДНА структура tss на всю систему, и только BSP делал
// ltr. Наш scheduler НЕ привязывает потоки к конкретному ядру — ring3
// launcher вполне мог мигрировать на AP, у которого Task Register
// вообще никогда не загружался (`ltr` не вызывался). Первый же "int
// 0x80" из ring3, выполненный на AP, требовал TSS.RSP0 у ядра, где
// TSS попросту не существует — мгновенный #GP/#DF (CR2 в дампе
// исключения был 0xFFFFFFFFFFFFFFF8, ровно "(0-8)" — CPU пытался
// толкнуть SS на стек по нулевому RSP0 из пустого/несуществующего TSS).
typedef struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;
    uint64_t reserved1;
    uint64_t ist1, ist2, ist3, ist4, ist5, ist6, ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} tss64_t;

#define KSTACK0_SIZE 8192
#define MAX_TSS_CPUS 4

// Один kstack + один TSS НА КАЖДОЕ ядро — раньше было по одному на
// всю систему.
static uint8_t kstack0[MAX_TSS_CPUS][KSTACK0_SIZE] __attribute__((aligned(16)));
static tss64_t tss[MAX_TSS_CPUS];

// GDT: null, kcode, kdata, ucode, udata (5 слотов) + по 2 слота на TSS
// каждого ядра (16-байтный system descriptor = 2 обычных QWORD-слота).
static uint64_t gdt[5 + 2 * MAX_TSS_CPUS];

typedef struct __attribute__((packed)) {
    uint16_t limit;
    uint64_t base;
} gdt_ptr_t;
static gdt_ptr_t gdt_ptr;

// Селекторы — используются и здесь, и в usermode.s/idt.c
#define SEL_KCODE 0x08
#define SEL_KDATA 0x10
#define SEL_UCODE 0x18
#define SEL_UDATA 0x20
#define SEL_TSS_BASE 0x28 // TSS ядра N — по адресу SEL_TSS_BASE + N*0x10

// Строит TSS-дескриптор для КОНКРЕТНОГО ядра в ОБЩЕЙ (расшаренной)
// GDT и делает ltr с СЕЛЕКТОРОМ ИМЕННО ЭТОГО ядра. GDT — общая, но
// каждый TSS-дескриптор используется только своим ядром (busy-бит
// у System-дескриптора не даёт двум CPU одновременно ltr один и тот
// же слот — отсюда и необходимость отдельных дескрипторов на ядро,
// а не общего).
void gdt_setup_cpu_tss(int cpu_idx) {
    if (cpu_idx < 0 || cpu_idx >= MAX_TSS_CPUS) return;

    tss64_t *t = &tss[cpu_idx];
    for (int i = 0; i < (int)(sizeof(*t)/sizeof(uint64_t)); i++) ((uint64_t*)t)[i] = 0;
    t->rsp0 = (uint64_t)&kstack0[cpu_idx][KSTACK0_SIZE];
    t->iomap_base = sizeof(*t);

    uint64_t base  = (uint64_t)t;
    uint64_t limit = sizeof(*t) - 1;
    int slot = 5 + cpu_idx * 2;
    gdt[slot] = (limit & 0xFFFF)
              | ((base & 0xFFFFFF) << 16)
              | (0x89ULL << 40)
              | (((limit >> 16) & 0xF) << 48)
              | (((base >> 24) & 0xFF) << 56);
    gdt[slot + 1] = (base >> 32) & 0xFFFFFFFF;

    uint16_t sel = (uint16_t)(SEL_TSS_BASE + cpu_idx * 0x10);
    __asm__ volatile ("ltr %0" : : "r"(sel));
}

void gdt_init(void) {
    // Дескрипторы code/data в 64-битном режиме (L=1 для code, остальные
    // поля почти не имеют значения в long mode кроме P/DPL/S/executable —
    // тот же формат, что уже использовали в boot64.s для kernel-сегментов,
    // здесь просто пересобираем в C и добавляем user-версии с DPL=3.
    uint64_t kcode = (1ULL<<43)|(1ULL<<44)|(1ULL<<47)|(1ULL<<53);
    uint64_t kdata = (1ULL<<44)|(1ULL<<47)|(1ULL<<41);
    uint64_t ucode = kcode | (3ULL<<45); // DPL=3
    uint64_t udata = kdata | (3ULL<<45); // DPL=3

    gdt[0] = 0;
    gdt[1] = kcode;
    gdt[2] = kdata;
    gdt[3] = ucode;
    gdt[4] = udata;

    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base  = (uint64_t)&gdt;

    __asm__ volatile ("lgdt %0" : : "m"(gdt_ptr));

    // Перезагружаем сегментные регистры данных сразу
    __asm__ volatile (
        "mov %0, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        : : "i"(SEL_KDATA) : "ax"
    );

    // CS нельзя перезагрузить через mov — нужен far jump/return, чтобы
    // CPU действительно перечитал дескриптор кода из НОВОЙ GDT.
    __asm__ volatile (
        "pushq %0\n"
        "leaq 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        : : "i"(SEL_KCODE) : "rax"
    );

    gdt_setup_cpu_tss(0); // BSP — всегда индекс 0 (см. get_cpu_index())
}

// Нужен для SMP: AP-трамплин должен загрузить ТУ ЖЕ самую GDT, что уже
// построил BSP (общие дескрипторы kernel/user одинаковы для всех
// ядер), а не строить собственную копию с нуля. TSS для AP настраивает
// gdt_setup_cpu_tss() отдельно, вызывается из ap_entry64().
uint64_t gdt_get_ptr_address(void) {
    return (uint64_t)&gdt_ptr;
}

// Диагностика: проверяем, что TSS.RSP0 конкретного ядра реально
// ненулевой после настройки.
uint64_t gdt_get_tss_rsp0(int cpu_idx) {
    if (cpu_idx < 0 || cpu_idx >= MAX_TSS_CPUS) return 0;
    return tss[cpu_idx].rsp0;
}
