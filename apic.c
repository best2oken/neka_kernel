#include <stdint.h>

extern void kprint(const char *s);
extern void kprint_hex64(uint64_t val);
extern void neka_panic(const char *category, uint64_t code);
extern void paging_map_mmio_2mb(uint64_t pa);

#define PANIC_NO_APIC 1

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t r; __asm__ volatile ("inb %1, %0" : "=a"(r) : "Nd"(port)); return r;
}
static inline void cpuid(uint32_t leaf, uint32_t *a, uint32_t *b, uint32_t *c, uint32_t *d) {
    __asm__ volatile ("cpuid" : "=a"(*a), "=b"(*b), "=c"(*c), "=d"(*d) : "a"(leaf));
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t val) {
    uint32_t lo = (uint32_t)val, hi = (uint32_t)(val >> 32);
    __asm__ volatile ("wrmsr" : : "a"(lo), "d"(hi), "c"(msr));
}

// Стандартные дефолтные физические адреса — конфигурируемы через ACPI
// MADT на настоящем железе (разные платы МОГУТ их менять), но абсолютное
// большинство x86_64 систем (и QEMU/OVMF) используют именно эти базы.
// Правильный next-step — прочитать их из MADT, а не хардкодить; отмечено
// в README как известное ограничение.
// Дефолты — верны почти всегда (совпадают с MADT на подавляющем
// большинстве систем, включая QEMU), но БАГ, найденный код-ревью:
// полагаться на них БЕЗ попытки прочитать реальные значения из ACPI
// MADT некорректно для настоящего железа. apic_apply_acpi_bases()
// ниже переопределяет эти переменные, если MADT доступен — вызывается
// из main64.c ПОСЛЕ acpi_find_rsdp_bios()/acpi_set_rsdp(), но ДО
// lapic_enable()/ioapic_set_redirect().
static uint64_t lapic_base_pa  = 0xFEE00000ULL;
static uint64_t ioapic_base_pa = 0xFEC00000ULL;

#define LAPIC_REG(offset) (*(volatile uint32_t *)(lapic_base_pa + (offset)))

#define LAPIC_ID          0x20
#define LAPIC_EOI         0xB0
#define LAPIC_SPURIOUS    0xF0
#define LAPIC_LVT_TIMER   0x320
#define LAPIC_TIMER_INIT  0x380
#define LAPIC_TIMER_CUR   0x390
#define LAPIC_TIMER_DIV   0x3E0

#define IOAPIC_IOREGSEL (*(volatile uint32_t *)(ioapic_base_pa + 0x00))
#define IOAPIC_IOWIN    (*(volatile uint32_t *)(ioapic_base_pa + 0x10))

void apic_check_supported(void) {
    uint32_t a, b, c, d;
    cpuid(1, &a, &b, &c, &d);
    if (!(d & (1u << 9))) {
        // Любой x86_64 CPU обязан иметь Local APIC (это часть базовой
        // архитектуры long mode) — если этой проверки не проходит,
        // что-то серьёзно не так с виртуализацией/эмуляцией, а не
        // просто "старое железо". Останавливаемся явно вместо того
        // чтобы городить APIC-код поверх несуществующего устройства.
        neka_panic("APIC", PANIC_NO_APIC);
    }
}

// Вызывается из main64.c ПОСЛЕ ACPI-парсинга, ДО lapic_enable(). Если
// acpi_find_apic_bases() ничего не нашла (MADT недоступен) — оставляем
// дефолты как есть, система просто продолжает работать на общепринятых
// стандартных адресах вместо честно прочитанных.
void apic_apply_acpi_bases(void) {
    extern int acpi_find_apic_bases(uint64_t *lapic_pa_out, uint64_t *ioapic_pa_out);
    uint64_t lapic_pa, ioapic_pa;
    if (acpi_find_apic_bases(&lapic_pa, &ioapic_pa)) {
        lapic_base_pa = lapic_pa;
        ioapic_base_pa = ioapic_pa;
        kprint("[apic] using ACPI-discovered LAPIC/IOAPIC addresses\n");
    } else {
        kprint("[apic] ACPI bases unavailable, using standard defaults\n");
    }
}

// Полностью маскируем legacy PIC 8259 — весь дальнейший IRQ-роутинг
// идёт через IOAPIC. Ремаппинг векторов (как раньше в pic_remap())
// больше не нужен: если PIC замаскирован полностью, ему просто нечем
// сгенерировать прерывание, конфликт векторов невозможен по построению.
void disable_legacy_pic(void) {
    outb(0xA1, 0xFF);
    outb(0x21, 0xFF);
}

uint8_t lapic_get_id(void) {
    return (uint8_t)((LAPIC_REG(LAPIC_ID) >> 24) & 0xFF);
}

// Единая точка правды "какой у меня индекс среди CPU" — используется и
// scheduler'ом (per-CPU current/loaded_cr3), и NKern-локом (per-CPU
// preempt_disable_count/reschedule_pending). APIC ID как индекс —
// осознанное упрощение (см. nkern_lock.c) — для QEMU и большинства
// реального железа ID укладывается в разумные пределы, но честный
// кернел строил бы таблицу APIC ID -> порядковый номер при перечислении
// через ACPI, а не полагался на маленький ID напрямую.
#define MAX_TRACKED_CPUS 16
int get_cpu_index(void) {
    int id = (int)lapic_get_id();
    return (id >= 0 && id < MAX_TRACKED_CPUS) ? id : 0;
}

void lapic_enable(void) {
    paging_map_mmio_2mb(lapic_base_pa);

    uint64_t base = rdmsr(0x1B); // IA32_APIC_BASE
    base |= (1ULL << 11);        // global enable
    wrmsr(0x1B, base);

    // Spurious Interrupt Vector Register: бит 8 — программное
    // включение самого LAPIC (отдельно от глобального enable в MSR
    // выше!); без него LAPIC физически не доставляет прерывания, даже
    // если MSR-бит уже включён. Вектор 0xFF — просто "мусорный", на
    // случай spurious interrupt (аппаратная гонка, не наша логическая
    // ошибка) он не должен пересекаться с реальными векторами.
    LAPIC_REG(LAPIC_SPURIOUS) = 0x1FF;
}

void lapic_send_eoi(void) {
    LAPIC_REG(LAPIC_EOI) = 0;
}

// Калибровка через PIT channel 2 + порт 0x61 (bit5 = OUT2 status) —
// классическая техника busy-wait без единого прерывания, применимая
// ДО того как IDT/IOAPIC вообще настроены. Логика: включаем gate
// channel 2, программируем его на одноразовый отсчёт заданной
// длительности, одновременно запускаем LAPIC-таймер на счёт вниз с
// максимального значения, ждём срабатывания PIT по polling'у бита в
// порту 0x61, смотрим насколько LAPIC-таймер успел уменьшиться —
// это и даёт его реальную частоту (которая иначе нигде не объявлена).
static uint32_t lapic_calibrate_ticks(uint32_t ms) {
    LAPIC_REG(LAPIC_TIMER_DIV) = 0x3; // делитель 16 — сохранится и для рабочего режима

    uint8_t x = inb(0x61);
    x = (uint8_t)((x & ~0x02) | 0x01); // speaker off, gate channel2 on
    outb(0x61, x);

    uint32_t count = (uint32_t)(1193182ULL * ms / 1000);
    outb(0x43, 0xB0); // channel 2, lobyte/hibyte, mode 0 (one-shot)
    outb(0x42, (uint8_t)(count & 0xFF));
    outb(0x42, (uint8_t)((count >> 8) & 0xFF));

    LAPIC_REG(LAPIC_TIMER_INIT) = 0xFFFFFFFF; // старт отсчёта вниз

    while (!(inb(0x61) & 0x20)); // ждём OUT2 (terminal count достигнут)

    uint32_t remaining = LAPIC_REG(LAPIC_TIMER_CUR);
    return 0xFFFFFFFFu - remaining; // сколько тиков LAPIC "съел" за ms миллисекунд
}

// Настраивает LAPIC-таймер на периодический режим с заданной частотой.
// vector — тот же вектор 32, что уже обрабатывается в interrupt_dispatch()
// как "таймер" — с точки зрения остального ядра (scheduler, IDT) ничего
// не меняется, меняется только ИСТОЧНИК прерывания (LAPIC вместо PIT/PIC).
void apic_timer_init(uint8_t vector, uint32_t hz) {
    uint32_t period_ms = 1000 / hz;
    uint32_t ticks = lapic_calibrate_ticks(period_ms);

    LAPIC_REG(LAPIC_LVT_TIMER) = vector | (1u << 17); // periodic mode, не замаскирован
    LAPIC_REG(LAPIC_TIMER_DIV) = 0x3;                  // divide by 16 (тот же делитель, что при калибровке)
    LAPIC_REG(LAPIC_TIMER_INIT) = ticks;

    kprint("[apic] LAPIC timer calibrated: ");
    kprint_hex64(ticks);
    kprint(" ticks/period\n");
}

static void ioapic_write(uint8_t reg, uint32_t val) {
    paging_map_mmio_2mb(ioapic_base_pa); // идемпотентно — безопасно звать повторно
    IOAPIC_IOREGSEL = reg;
    IOAPIC_IOWIN = val;
}

// Прописывает redirection entry: внешний ISA IRQ -> вектор IDT на
// конкретном Local APIC (physical destination mode, fixed delivery,
// edge-triggered active-high — стандартное поведение ISA-совместимых
// прерываний вроде PS/2-клавиатуры).
void ioapic_set_redirect(uint8_t irq, uint8_t vector) {
    uint32_t low  = vector; // delivery=fixed(000), dest=physical(0), не замаскирован(0)
    uint32_t high = ((uint32_t)lapic_get_id()) << 24;
    ioapic_write(0x10 + irq * 2, low);
    ioapic_write(0x10 + irq * 2 + 1, high);
}
