#include <stdint.h>
#include "kernel64.h"
#include "heap.h"
#include "idt.h"

extern void kprint(const char *s);
extern void kprint_hex64(uint64_t val);

// Символы из ap_trampoline_blob.o — создаются `ld -r -b binary` при
// сборке (build64.sh/build_uefi.sh), имя автоматически произведено
// линкером от имени входного файла ap_trampoline.bin.
extern uint8_t _binary_ap_trampoline_bin_start[];
extern uint8_t _binary_ap_trampoline_bin_end[];

// ДОЛЖНО совпадать с `org 0x8000` в ap_trampoline.s — AP стартует по
// физическому адресу vector<<12, значит адрес обязан быть кратен 4KB.
#define TRAMPOLINE_ADDR 0x8000ULL
#define MBOX_OFFSET     0x100 // синхронизировано вручную с ap_trampoline.s

#define AP_STACK_SIZE (16 * 1024)
#define MAX_AP_CPUS 8

static volatile int ap_ready[MAX_AP_CPUS];
extern void ap_entry64(void); // определена в smp.c ниже, адрес берём через &

static inline void outb_smp(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

#define LAPIC_REG(offset) (*(volatile uint32_t *)(0xFEE00000ULL + (offset)))
#define LAPIC_ICR_LOW  0x300
#define LAPIC_ICR_HIGH 0x310

// Грубая задержка busy-loop — Intel MP spec задаёт МИНИМАЛЬНЫЕ паузы
// между INIT/SIPI/SIPI (10мс, затем ~200мкс между двумя SIPI); быть
// МЕДЛЕННЕЕ спеки не страшно, страшно быть быстрее. Настоящий кернел
// использовал бы откалиброванный delay (мы уже умеем это — см.
// apic_timer_init's calibration), здесь — с большим запасом простой
// busy-loop, отмечено как сознательное упрощение.
static void crude_delay(uint64_t iterations) {
    for (volatile uint64_t i = 0; i < iterations; i++);
}

static void send_init_sipi_sipi(uint8_t apic_id) {
    uint32_t vector = (uint32_t)(TRAMPOLINE_ADDR >> 12);

    // INIT (assert)
    LAPIC_REG(LAPIC_ICR_HIGH) = ((uint32_t)apic_id) << 24;
    LAPIC_REG(LAPIC_ICR_LOW)  = 0x4500; // INIT, level=assert
    crude_delay(50000000);              // ~10мс с большим запасом

    // SIPI #1
    LAPIC_REG(LAPIC_ICR_HIGH) = ((uint32_t)apic_id) << 24;
    LAPIC_REG(LAPIC_ICR_LOW)  = 0x4600 | vector; // Startup IPI
    crude_delay(5000000);                          // ~200мкс с запасом

    // SIPI #2 — по спеке требуется дважды, некоторые CPU игнорируют первый
    LAPIC_REG(LAPIC_ICR_HIGH) = ((uint32_t)apic_id) << 24;
    LAPIC_REG(LAPIC_ICR_LOW)  = 0x4600 | vector;
    crude_delay(5000000);
}

// Точка входа AP на чистом C — сюда прыгает ap_trampoline.s после
// перехода в long mode. Каждое ядро обязано само включить СВОЙ Local
// APIC (MSR — состояние per-core, инициализация BSP на других ядрах
// не распространяется) и откалибровать СВОЙ таймер.
static volatile int g_ap_index_starting = -1; // какой индекс AP сейчас поднимается (для ap_ready[])

void ap_entry64(void) {
    extern void lapic_enable(void);
    extern void apic_timer_init(uint8_t vector, uint32_t hz);
    extern void sched64_reschedule(void);
    extern void fpu_init(void);

    // БАГ, найденный на практике (третий раз одна и та же природа —
    // см. README): CR0.EM/CR4.OSFXSR — состояние ПРОЦЕССОРНОГО ЯДРА,
    // не глобальное. fpu_init(), который BSP вызвал в kernel_main64(),
    // никак не влияет на AP — у него СВОИ регистры CR0/CR4. Первая же
    // SSE-инструкция, которую GCC на -O2 вставил в kprint_hex64()
    // (movdqa при инициализации локального буфера), дала #UD/#GP на
    // AP, а IDT там ещё тоже не установлен — мгновенный triple fault
    // без единого printf. ОБЯЗАН быть первым вызовом на КАЖДОМ ядре.
    fpu_init();

    idt_load_current_cpu(); // IDTR per-core — без этого свой же таймер triple fault'ит AP, см. idt.c

    // БАГ, найденный на практике (реальный краш UEFI+SMP, задокументирован
    // подробно в gdt64.c): раньше AP сознательно НЕ получал свой TSS,
    // в расчёте на то что ring3-код туда не попадёт. Расчёт был неверным
    // — scheduler не привязывает потоки к ядрам, ring3 launcher мог
    // мигрировать на AP в любой момент. Теперь у каждого ядра свой TSS.
    gdt_setup_cpu_tss(get_cpu_index());
    kprint("[smp] AP TSS configured\n");

    kprint("[smp] ap_entry64 reached\n");

    lapic_enable();
    kprint("[smp] lapic_enable done\n");

    uint8_t id = lapic_get_id();
    kprint("[smp] got id\n");
    kprint_hex64((uint64_t)id);
    kprint("\n");

    // NB: сознательно НЕ грузим GDT/LTR заново и НЕ строим отдельный
    // per-CPU TSS в этой версии — AP работает только в ring0 (ни один
    // ring3-процесс сюда не планируется), поэтому TSS.RSP0 ему не нужен.
    // Свой TSS на CPU понадобится, когда захотим ring3-код на AP —
    // отмечено как известное ограничение в README.

    apic_timer_init(32, 100); // тот же вектор/частота, что у BSP — свой, локальный таймер этого ядра
    kprint("[smp] apic_timer_init done\n");

    if (g_ap_index_starting >= 0) ap_ready[g_ap_index_starting] = 1;
    kprint("[smp] ap_ready set\n");

    __asm__ volatile ("sti");

    for (;;) {
        sched64_reschedule();
        __asm__ volatile ("hlt");
    }
}

// Запускается BSP-ом один раз, уже ПОСЛЕ полной инициализации своего
// собственного APIC/scheduler/heap. Возвращает количество реально
// поднятых AP (может быть 0, если ACPI не нашла других CPU — тогда
// система просто продолжает работать в однопроцессорном режиме,
// без паники: SMP — это ускорение, а не обязательное требование).
int smp_start_aps(void) {
    uint8_t apic_ids[MAX_AP_CPUS];
    int cpu_count = acpi_find_cpus(apic_ids, MAX_AP_CPUS);
    if (cpu_count <= 1) {
        kprint("[smp] single-CPU system (or ACPI unavailable) — skipping AP bringup\n");
        return 0;
    }

    uint8_t bsp_id = lapic_get_id();
    uint64_t kernel_cr3 = paging_get_kernel_cr3();
    uint64_t gdt_ptr_addr = gdt_get_ptr_address();

    int started = 0;
    for (int i = 0; i < cpu_count && started < MAX_AP_CPUS; i++) {
        if (apic_ids[i] == bsp_id) continue; // сами себя не поднимаем

        // БАГ, найденный на практике (краш на UEFI+SMP уже ПОСЛЕ фикса
        // per-CPU TSS): под UEFI образ ядра грузится прошивкой по
        // адресу, который выбирает ОНА, а не мы (наш линкер-скрипт для
        // UEFI использует ImageBase=0, релоцируется загрузчиком) —
        // фиксированный физический адрес 0x8000, безопасный по BIOS-эра
        // конвенции, НЕ гарантированно свободен под UEFI. Проверяем по
        // РЕАЛЬНО разобранной карте памяти перед тем как туда писать.
        uint64_t blob_size_check = (uint64_t)(_binary_ap_trampoline_bin_end - _binary_ap_trampoline_bin_start);
        if (!memmap_region_is_safe(TRAMPOLINE_ADDR, blob_size_check)) {
            kprint("[smp] WARNING: trampoline address 0x8000 not confirmed safe by memory map!\n");
        }

        // Копируем трамплин на фиксированный физический адрес — он
        // identity-mapped (первый 1GB), обычный memcpy работает.
        uint8_t *dst = (uint8_t *)TRAMPOLINE_ADDR;
        uint8_t *src = _binary_ap_trampoline_bin_start;
        uint64_t blob_size = (uint64_t)(_binary_ap_trampoline_bin_end - _binary_ap_trampoline_bin_start);
        for (uint64_t b = 0; b < blob_size; b++) dst[b] = src[b];

        // Заполняем почтовый ящик — фиксированное смещение 0x100,
        // синхронизировано вручную с ap_trampoline.s.
        uint64_t *mbox = (uint64_t *)(TRAMPOLINE_ADDR + MBOX_OFFSET);
        void *ap_stack = kmalloc(AP_STACK_SIZE);
        mbox[0] = kernel_cr3;
        mbox[1] = gdt_ptr_addr;
        // ВАЖНО про выравнивание (наступили на это на практике — #GP на
        // movaps внутри kprint_hex64, вызванного из ap_entry64): трамплин
        // делает НАСТОЯЩИЙ "call rax" в 64-битном режиме, значит перед
        // ним RSP обязан быть % 16 == 0 (тогда call столкнёт 8 байт, и
        // ap_entry64 увидит стандартное RSP % 16 == 8 на входе — то, что
        // ожидает System V ABI и на что рассчитывает GCC при выборе
        // выровненных SSE-инструкций). kmalloc() НЕ гарантирует 16-байтовое
        // выравнивание возврата — границы блоков heap сдвигаются на
        // align16(size)+4 (не кратно 16) по мере разбиения, поэтому
        // маскируем явно, а не полагаемся на удачу.
        uint64_t stack_top = ((uint64_t)ap_stack + AP_STACK_SIZE) & ~0xFULL;
        mbox[2] = stack_top;
        mbox[3] = (uint64_t)ap_entry64;

        kprint("[smp diag] mbox[0] kernel_cr3   = "); kprint_hex64(mbox[0]); kprint("\n");
        kprint("[smp diag] mbox[1] gdt_ptr_addr = "); kprint_hex64(mbox[1]); kprint("\n");
        kprint("[smp diag] mbox[2] stack_top    = "); kprint_hex64(mbox[2]); kprint("\n");
        kprint("[smp diag] mbox[3] ap_entry64   = "); kprint_hex64(mbox[3]); kprint("\n");
        kprint("[smp diag] ap_stack (raw kmalloc) = "); kprint_hex64((uint64_t)ap_stack); kprint("\n");

        ap_ready[started] = 0;
        g_ap_index_starting = started;

        kprint("[smp] starting AP, APIC ID=");
        kprint_hex64((uint64_t)apic_ids[i]);
        kprint("...\n");

        send_init_sipi_sipi(apic_ids[i]);

        // Ждём подтверждения с разумным таймаутом — если AP не поднялся
        // (баг в трамплине, несовместимое железо), не виснем навечно,
        // а честно сообщаем и идём дальше однопроцессорными силами.
        int timeout = 200000000;
        while (!ap_ready[started] && timeout-- > 0);

        if (ap_ready[started]) {
            kprint("[smp] AP confirmed alive.\n");
            started++;
        } else {
            kprint("[smp] WARNING: AP did not respond within timeout.\n");
        }
    }

    return started;
}
