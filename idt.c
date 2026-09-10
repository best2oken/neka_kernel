#include "idt.h"

static idt_entry_t idt[256];
static idt_ptr_t   idt_ptr;

extern void isr0(void);  extern void isr1(void);  extern void isr2(void);  extern void isr3(void);
extern void isr4(void);  extern void isr5(void);  extern void isr6(void);  extern void isr7(void);
extern void isr8(void);  extern void isr9(void);  extern void isr10(void); extern void isr11(void);
extern void isr12(void); extern void isr13(void); extern void isr14(void); extern void isr15(void);
extern void isr16(void); extern void isr17(void); extern void isr18(void); extern void isr19(void);
extern void isr20(void); extern void isr21(void); extern void isr22(void); extern void isr23(void);
extern void isr24(void); extern void isr25(void); extern void isr26(void); extern void isr27(void);
extern void isr28(void); extern void isr29(void); extern void isr30(void); extern void isr31(void);
extern void isr32(void); extern void isr33(void); extern void isr128(void);

// Нужны для вывода — реализованы в terminal64.c (следующий шаг),
// пока форвард-декларации, чтобы idt.c не зависел от конкретного backend'а
extern void kprint(const char *s);
extern void kprint_hex64(uint64_t val);

void idt_set_gate(uint8_t vector, uint64_t handler, uint8_t ist, uint8_t dpl) {
    idt[vector].offset_low  = handler & 0xFFFF;
    idt[vector].selector    = 0x08;          // наш 64-битный code segment из GDT
    idt[vector].ist         = ist;
    // present(1) | DPL(2 бита) | type=interrupt gate(0xE). DPL=0 — только
    // ring0 может войти через "int"; DPL=3 нужен ИМЕННО для syscall-гейта,
    // иначе ring3-код получит #GP при попытке "int 0x80" (CPL(3) > DPL(0)
    // запрещено аппаратно — так мы и защищаем остальные 33 вектора от
    // прямого вызова из user-space).
    idt[vector].type_attr   = 0x80 | ((dpl & 3) << 5) | 0x0E;
    idt[vector].offset_mid  = (handler >> 16) & 0xFFFF;
    idt[vector].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[vector].zero        = 0;
}

void idt_init(void) {
    idt_ptr.limit = sizeof(idt) - 1;
    idt_ptr.base  = (uint64_t)&idt;

    idt_set_gate(0, (uint64_t)isr0, 0, 0);
    idt_set_gate(1, (uint64_t)isr1, 0, 0);
    idt_set_gate(2, (uint64_t)isr2, 0, 0);
    idt_set_gate(3, (uint64_t)isr3, 0, 0);
    idt_set_gate(4, (uint64_t)isr4, 0, 0);
    idt_set_gate(5, (uint64_t)isr5, 0, 0);
    idt_set_gate(6, (uint64_t)isr6, 0, 0);
    idt_set_gate(7, (uint64_t)isr7, 0, 0);
    idt_set_gate(8, (uint64_t)isr8, 0, 0);
    idt_set_gate(9, (uint64_t)isr9, 0, 0);
    idt_set_gate(10, (uint64_t)isr10, 0, 0);
    idt_set_gate(11, (uint64_t)isr11, 0, 0);
    idt_set_gate(12, (uint64_t)isr12, 0, 0);
    idt_set_gate(13, (uint64_t)isr13, 0, 0);
    idt_set_gate(14, (uint64_t)isr14, 0, 0);
    idt_set_gate(15, (uint64_t)isr15, 0, 0);
    idt_set_gate(16, (uint64_t)isr16, 0, 0);
    idt_set_gate(17, (uint64_t)isr17, 0, 0);
    idt_set_gate(18, (uint64_t)isr18, 0, 0);
    idt_set_gate(19, (uint64_t)isr19, 0, 0);
    idt_set_gate(20, (uint64_t)isr20, 0, 0);
    idt_set_gate(21, (uint64_t)isr21, 0, 0);
    idt_set_gate(22, (uint64_t)isr22, 0, 0);
    idt_set_gate(23, (uint64_t)isr23, 0, 0);
    idt_set_gate(24, (uint64_t)isr24, 0, 0);
    idt_set_gate(25, (uint64_t)isr25, 0, 0);
    idt_set_gate(26, (uint64_t)isr26, 0, 0);
    idt_set_gate(27, (uint64_t)isr27, 0, 0);
    idt_set_gate(28, (uint64_t)isr28, 0, 0);
    idt_set_gate(29, (uint64_t)isr29, 0, 0);
    idt_set_gate(30, (uint64_t)isr30, 0, 0);
    idt_set_gate(31, (uint64_t)isr31, 0, 0);
    idt_set_gate(32, (uint64_t)isr32, 0, 0); // IRQ0 (PIT timer)
    idt_set_gate(33, (uint64_t)isr33, 0, 0); // IRQ1 (keyboard)
    idt_set_gate(128, (uint64_t)isr128, 0, 3); // int 0x80 syscall — ЕДИНСТВЕННЫЙ гейт с DPL=3

    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));
}

// Для AP: IDTR — состояние КОНКРЕТНОГО ядра, не разделяется автоматически
// между процессорами просто потому что BSP один раз сделал lidt. Сама
// таблица idt[]/idt_ptr — обычные данные в памяти, общие для всех ядер
// (identity-mapped, один и тот же физический адрес видят все) — поэтому
// не нужно СТРОИТЬ её заново на каждом core, только повторно исполнить
// lidt, указав на ТУ ЖЕ таблицу. БАГ, найденный на практике: без этого
// AP успевал выполнить один тик потока, но при первом же срабатывании
// СВОЕГО локального LAPIC-таймера (прерывания на AP были включены,
// IDTR — пустой после INIT-SIPI-SIPI) мгновенно triple fault'ился.
void idt_load_current_cpu(void) {
    __asm__ volatile ("lidt %0" : : "m"(idt_ptr));
}

static const char *exception_name(uint64_t vector) {
    static const char *names[32] = {
        "Divide Error", "Debug", "NMI", "Breakpoint",
        "Overflow", "Bound Range Exceeded", "Invalid Opcode", "Device Not Available",
        "Double Fault", "Coprocessor Segment Overrun", "Invalid TSS", "Segment Not Present",
        "Stack-Segment Fault", "General Protection Fault", "Page Fault", "Reserved",
        "x87 FPU Error", "Alignment Check", "Machine Check", "SIMD FP Exception",
        "Virtualization Exception", "Control Protection", "Reserved", "Reserved",
        "Reserved", "Reserved", "Reserved", "Reserved",
        "Hypervisor Injection", "VMM Communication", "Security Exception", "Reserved"
    };
    if (vector < 32) return names[vector];
    return "Unknown";
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

// Аналог обработчика exception fault в EKA2 (там — TExcTrap/exception handler
// framework с диагностикой в crash log); здесь — минимальная версия: печатаем
// вектор, error_code, RIP и виснем осознанно вместо тихого triple fault.
void interrupt_dispatch(interrupt_frame_t *f) {
    if (f->vector < 32) {
        kprint("\n*** EXCEPTION: ");
        kprint(exception_name(f->vector));
        kprint(" (vector ");
        kprint_hex64(f->vector);
        kprint(", error_code ");
        kprint_hex64(f->error_code);
        kprint(")\nRIP=");
        kprint_hex64(f->rip);
        kprint(" RSP-frame CS=");
        kprint_hex64(f->cs);
        kprint(" RFLAGS=");
        kprint_hex64(f->rflags);
        kprint("\nHalting kernel (no recovery implemented).\n");

        // Диагностика: дампим память вокруг места сбоя, ищем откуда
        // взялось подозрительное значение (например, 0x1E8480 =
        // 2000000 — число итераций нашего busy-loop — если оно попало
        // туда, где ожидался адрес возврата, это прямая улика porчи
        // стека). Печатаем f (указатель фрейма ~ RSP на момент сбоя)
        // и 24 qword-а после него.
        kprint("[diag] frame ptr f = "); kprint_hex64((uint64_t)f); kprint("\n");
        uint64_t *stack_dump = (uint64_t *)f;
        for (int i = 0; i < 24; i++) {
            kprint("[diag] f[");
            kprint_hex64((uint64_t)i);
            kprint("] = ");
            kprint_hex64(stack_dump[i]);
            kprint("\n");
        }

        for (;;) { __asm__ volatile ("cli; hlt"); }
    }

    // IRQ0 = таймер: настоящий preemptive tick планировщика.
    // EOI шлём ДО reschedule — если этого не сделать, LAPIC решит, что
    // этот вектор всё ещё "in service", и следующий тик никогда не
    // придёт, даже после возврата на этот же поток (частая ошибка,
    // тот же принцип что был с legacy PIC, просто другой регистр EOI).
    if (f->vector == 32) {
        extern void lapic_send_eoi(void);
        lapic_send_eoi();
        extern void sched64_timer_tick(void);
        sched64_timer_tick();
    }
    // IRQ1 = клавиатура: читаем и транслируем scancode через настоящий
    // драйвер (keyboard.c), не просто гасим прерывание. Теперь приходит
    // через IOAPIC, а не legacy PIC — EOI всё равно шлём в LAPIC (EOI
    // всегда адресован ЛОКАЛЬНОМУ APIC, независимо от того, кто —
    // IOAPIC или сам LAPIC-таймер — сгенерировал прерывание).
    if (f->vector == 33) {
        extern void keyboard_handle_irq(void);
        keyboard_handle_irq();
        extern void lapic_send_eoi(void);
        lapic_send_eoi();
    }

    // int 0x80 — программный syscall от ring3. Никакого EOI не нужно
    // (это не аппаратный IRQ от PIC, а software interrupt).
    // f->cs&3 — это RPL из CS, который CPU САМ положил на стек в момент
    // прерывания; если он равен 3, значит вызывающий код действительно
    // выполнялся на CPL3 — не наше слово, а факт, зафиксированный CPU.
    if (f->vector == 128) {
        int from_ring3 = (f->cs & 3) == 3;
        if (f->rax == 1) { // syscall 1 = print char (в RDI)
            char c = (char)(f->rdi & 0xFF);
            // Ring3-цикл крутится намного быстрее busy-loop потоков A/B —
            // без throttling лог тонет в повторах одного символа. Печатаем
            // раз в N вызовов, честно считая реальное число syscall'ов.
            static uint32_t call_count = 0;
            call_count++;
            if (call_count % 300000 == 1) {
                char buf[2] = {c, 0};
                kprint("[ring3] ");
                kprint(buf);
                kprint(" (syscall #");
                kprint_hex64(call_count);
                kprint(")\n");
            }
            if (from_ring3) {
                static int shown = 0;
                if (!shown) {
                    shown = 1;
                    kprint("[syscall] confirmed genuine CPL3 caller (CS&3==3)\n");
                }
            }
        }
    }
}
