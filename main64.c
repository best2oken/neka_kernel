#include <stdint.h>
#include "idt.h"
#include "kernel64.h"
#include "heap.h"

extern void terminal64_init(void);
extern void kprint(const char *s);
extern void kprint_hex64(uint64_t val);
extern void apic_check_supported(void);
extern void disable_legacy_pic(void);
extern void lapic_enable(void);
extern void apic_timer_init(uint8_t vector, uint32_t hz);
extern void ioapic_set_redirect(uint8_t irq, uint8_t vector);
extern void fpu_init(void);
extern void neka_panic(const char *category, uint64_t code);

// Коды паники для категории "SCHED" — аналог кодов в User::Panic()
#define PANIC_THREAD_CREATE_FAILED 1

// Самопроверка heap: выделяем блоки разного размера, пишем в каждый
// свой узнаваемый паттерн (чтобы поймать overlap между блоками, если
// split_block() где-то ошибся в арифметике границ), затем освобождаем
// НЕ в LIFO-порядке (специально вразнобой — это заставляет coalescing
// реально сливать блоки и вперёд, и назад, а не только в тривиальном
// стековом случае). Финальная проверка: сумма свободных байт должна
// вернуться ровно к тому, что было до всех выделений — если нет,
// значит где-то либо утечка, либо coalescing работает неверно.
static void heap_self_test(void) {
    kprint("[heap] self-test starting...\n");
    heap_print_stats();
    size_t initial_free = heap_free_bytes();

    void *p1 = kmalloc(100);
    void *p2 = kmalloc(4000);
    void *p3 = kmalloc(50);
    void *p4 = kmalloc(20000);
    void *p5 = kmalloc(16);

    // Пишем паттерны и сразу перепроверяем p1, p3, p5 (маленькие блоки,
    // которые физически соседствуют с только что выделенными большими
    // p2/p4 — если split_block() промахнулся с границей хотя бы на
    // байт, это затрёт соседний блок и проявится здесь).
    for (int i = 0; i < 100; i++) ((uint8_t*)p1)[i] = 0xAA;
    for (int i = 0; i < 50; i++)  ((uint8_t*)p3)[i] = 0xBB;
    for (int i = 0; i < 16; i++)  ((uint8_t*)p5)[i] = 0xCC;

    int corrupted = 0;
    for (int i = 0; i < 100; i++) if (((uint8_t*)p1)[i] != 0xAA) corrupted = 1;
    for (int i = 0; i < 50; i++)  if (((uint8_t*)p3)[i] != 0xBB) corrupted = 1;
    for (int i = 0; i < 16; i++)  if (((uint8_t*)p5)[i] != 0xCC) corrupted = 1;

    kprint(corrupted ? "[heap] OVERLAP DETECTED after alloc!\n" : "[heap] no overlap after alloc, OK\n");
    heap_print_stats();

    // Освобождаем вразнобой: сначала средний (p3), потом первый (p1) —
    // это должно слить p1+p3 в один блок (backward+forward coalesce
    // одновременно с точки зрения p1), затем p4, затем p2 (сольётся с
    // уже освобождённым p1+p3 через промежуточные границы), последним p5.
    kfree(p3);
    kfree(p1);
    kfree(p4);
    kfree(p2);
    kfree(p5);

    size_t final_free = heap_free_bytes();
    kprint("[heap] after freeing everything: ");
    heap_print_stats();

    if (final_free == initial_free) {
        kprint("[heap] self-test PASSED (free bytes fully recovered)\n\n");
    } else {
        kprint("[heap] self-test FAILED: free bytes mismatch (leak or bad coalesce)\n\n");
    }
}

static volatile uint32_t counter_a = 0;
static volatile uint32_t counter_b = 0;
static volatile uint32_t counter_c = 0;

// Каждый поток пишет свою "подпись" по ОДНОМУ И ТОМУ ЖЕ виртуальному
// адресу (DEMO_VA), затем читает обратно. Раз у каждого потока своя
// приватная физическая страница за этим VA (см. paging64.c), поток
// должен всегда видеть СВОЮ подпись, даже если другой поток тоже
// писал по тому же адресу между двумя его запусками — это и есть
// доказательство изоляции адресных пространств через CR3.
static void write_signature(char c) {
    volatile char *p = (volatile char *)DEMO_VA;
    for (int i = 0; i < 4; i++) p[i] = c;
}
static int verify_signature(char expected) {
    volatile char *p = (volatile char *)DEMO_VA;
    for (int i = 0; i < 4; i++) if (p[i] != expected) return 0;
    return 1;
}

// Никаких sched_yield() здесь больше нет — потоки просто работают, а
// таймер (100 Гц, IRQ0) вытесняет их сам через sched64_timer_tick().
// Это и есть разница с кооперативной 32-битной версией: настоящий
// preemptive multitasking, как в реальном EKA2.
static void thread_a(void) {
    for (;;) {
        counter_a++;
        write_signature('A');
        for (volatile int i = 0; i < 2000000; i++); // тут таймер может вытеснить нас
        int ok = verify_signature('A'); // если изоляция сломана, тут будет 0
        kprint("[A cpu"); kprint_hex64((uint64_t)lapic_get_id()); kprint("] tick ");
        kprint_hex64(counter_a);
        kprint(ok ? " AS=OK\n" : " AS=CORRUPTED!\n");

        // Каждый 5-й тик — демонстрация NKern::Lock(): держим критическую
        // секцию заведомо ДОЛЬШЕ одного timeslice (timeslice = 5 тиков
        // таймера по 10мс = 50мс; цикл ниже занимает намного больше).
        // Если механизм реально работает — за это время НИ ОДНОГО тика
        // потока B в логе не появится, несмотря на то что таймер продолжает
        // тикать и honestly пытается вытеснить нас (reschedule откладывается,
        // а не выполняется). Сразу после NKern_Unlock() — либо отложенный
        // reschedule сработает немедленно, либо поток доработает остаток
        // своего текущего timeslice, в обоих случаях B не мог вклиниться РАНЬШЕ.
        if (counter_a % 5 == 0) {
            kprint("[A] --- entering critical section (NKern_Lock) ---\n");
            NKern_Lock();
            for (volatile long i = 0; i < 20000000; i++);
            kprint("[A] --- still holding lock, about to unlock ---\n");
            NKern_Unlock();
            kprint("[A] --- critical section released (NKern_Unlock) ---\n");
        }

        for (volatile int i = 0; i < 3000000; i++);
    }
}

static void thread_b(void) {
    for (;;) {
        counter_b++;
        write_signature('B');
        for (volatile int i = 0; i < 2000000; i++);
        int ok = verify_signature('B');
        kprint("  [B cpu"); kprint_hex64((uint64_t)lapic_get_id()); kprint("] tick ");
        kprint_hex64(counter_b);
        kprint(ok ? " AS=OK\n" : " AS=CORRUPTED!\n");
        for (volatile int i = 0; i < 3000000; i++);
    }
}

static void thread_c_low_priority(void) {
    for (;;) {
        counter_c++;
        kprint("    [C low-prio] tick "); kprint_hex64(counter_c); kprint("\n");
        for (volatile int i = 0; i < 5000000; i++);
    }
}

// Этот "поток" в обычном смысле не возвращается: enter_usermode() делает
// iretq в ring3 и больше не отдаёт управление сюда — модель похожа на
// exec() в Unix, где текущий процесс заменяет свой образ и никогда не
// возвращается в вызвавший его код (кроме как через явный syscall exit,
// которого мы пока не реализовали).
static void thread_ring3_launcher(void) {
    kprint("[launcher] jumping into ring3...\n");
    enter_usermode(USER_CODE_VA, USER_STACK_TOP);
    // Недостижимо после iretq.
}

extern uint64_t kernel_end; // из linker64.ld / elf_x86_64_efi_neka.lds

void kernel_main64(uint64_t mbi_addr) {
    // fpu_init() ОБЯЗАН быть первым — это уже второй раз, когда эта же
    // ошибка всплывает (см. README): GCC на -O2 вектализирует через SSE
    // почти любой цикл обнуления/копирования памяти, включая тот, что
    // теперь появился внутри paging_init_kernel_map() (заинлайненный
    // alloc_frame(), обнуляющий фрейм). Без FPU/SSE state это #UD/#GP
    // ещё до того, как IDT вообще установлен — мгновенный triple fault.
    fpu_init();

    // Строим СВОИ page tables и переключаем CR3 сразу после — это делает
    // kernel_main64() независимым от того, кто нас позвал (GRUB через
    // multiboot со своим identity map для перехода в long mode, или
    // UEFI, где прошивка уже сама была в long mode и дала СВОЙ flat map).
    paging_init_kernel_map();

    terminal64_init();

    kprint("[diag] kernel_main64 runtime address = ");
    kprint_hex64((uint64_t)&kernel_main64);
    kprint("\n");


    kprint("========================================\n");
    kprint(" NEKA64 -- preemptive scheduler online\n");
    kprint("========================================\n");

    kprint("FPU/SSE initialized (CR0.EM=0, CR4.OSFXSR=1)\n");

    gdt_init();
    kprint("GDT rebuilt: kernel+user segments, TSS loaded (ring3 ready)\n");
    kprint("[diag] TSS.RSP0 (cpu0) = "); kprint_hex64(gdt_get_tss_rsp0(0)); kprint("\n");

    idt_init();
    kprint("IDT installed (32 exceptions + IRQ0/IRQ1)\n");

    // На multiboot-пути RSDP ещё не найден (UEFI-путь уже сделал это
    // через Configuration Table в uefi_main.c до вызова kernel_main64() —
    // guard внутри acpi_find_rsdp_bios() не даст перезаписать). Делаем
    // это ДО инициализации APIC, чтобы apic_apply_acpi_bases() ниже
    // реально могло найти MADT, а не работать на пустом месте.
    acpi_find_rsdp_bios();

    apic_check_supported();
    apic_apply_acpi_bases(); // читаем РЕАЛЬНЫЕ адреса LAPIC/IOAPIC из MADT вместо хардкода
    disable_legacy_pic();
    lapic_enable();
    ioapic_set_redirect(1, 33); // ISA IRQ1 (клавиатура) -> вектор 33, тот же что раньше через PIC
    apic_timer_init(32, 100);   // вектор 32, 100 Гц — те же вектор/частота, что были у PIT
    kprint("Legacy PIC disabled, LAPIC+IOAPIC online\n");

    // mbi_addr!=0 -> GRUB/multiboot путь, разбираем прямо здесь.
    // mbi_addr==0 -> либо UEFI (там uefi_main.c уже вызвал
    // memmap_parse_uefi() ДО этого вызова kernel_main64(), регионы уже
    // в списке), либо memory map просто недоступна — activate() в этом
    // случае активирует пустой список, и alloc_frame() тут же панике́т
    // при первой попытке взять фрейм сверх bootstrap-пула (лучше явная
    // остановка, чем тихая порча памяти).
    memmap_parse_multiboot(mbi_addr);
    memmap_activate(0x100000, (uint64_t)&kernel_end);

    heap_init();
    heap_self_test();

    sched64_init();

    // БАГ, найденный при аудите: sched64_create_thread() возвращает 0
    // при переполнении MAX_THREADS64 или неверном приоритете, но
    // раньше это никак не проверялось — поток просто "молча" не
    // создавался, и первый sched64_reschedule() ушёл бы в бесконечный
    // hlt-цикл без единого сообщения о причине. Аналог непроверенного
    // Leave-кода в Symbian — TRAPD без проверки результата.
    if (!sched64_create_thread(thread_a, 5, "ThreadA"))
        neka_panic("SCHED", PANIC_THREAD_CREATE_FAILED);
    if (!sched64_create_thread(thread_b, 5, "ThreadB"))
        neka_panic("SCHED", PANIC_THREAD_CREATE_FAILED);
    if (!sched64_create_thread(thread_c_low_priority, 2, "ThreadC-low"))
        neka_panic("SCHED", PANIC_THREAD_CREATE_FAILED);

    thread64_t *ring3_thread = sched64_create_thread(thread_ring3_launcher, 5, "Ring3Launcher");
    if (!ring3_thread)
        neka_panic("SCHED", PANIC_THREAD_CREATE_FAILED);
    // У этого потока — не обычное демо-адресное пространство, а
    // настоящий процесс с замапленными user code/stack страницами.
    ring3_thread->cr3 = create_user_process();

    kprint("4 threads created (A,B,Ring3Launcher prio=5, C prio=2)\n");

    int aps_started = smp_start_aps();
    kprint("[smp] Application Processors started: ");
    kprint_hex64((uint64_t)aps_started);
    kprint("\n");

    kprint("Enabling interrupts -- preemption starts now.\n\n");
    __asm__ volatile ("sti");

    // Первый ручной reschedule запускает самый первый поток; дальше
    // таймер (100 Гц) вытесняет потоки сам, без единого sched_yield().
    for (;;) {
        sched64_reschedule();
        __asm__ volatile ("hlt");
    }
}
