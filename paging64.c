#include <stdint.h>
#include <stddef.h>

extern void kprint(const char *s);
extern void neka_panic(const char *category, uint64_t code);

#define PANIC_OUT_OF_FRAMES 1

// Виртуальный адрес демо-страницы — одинаковый для ВСЕХ адресных
// пространств. Смысл демонстрации: разные потоки пишут/читают по этому
// же VA, но физически это разная память — доказательство изоляции.
// Выбран за пределами identity-mapped первого 1GB (который расшарен
// между всеми address space через общий PDPT[0]), чтобы явно завести
// отдельную PDPT[1]-ветку с 4KB-страницами именно под приватность.
#define DEMO_VA 0x40000000ULL

// Bump-аллокатор физических фреймов — двухступенчатый:
//   1) "bootstrap pool" (этот статический массив) обслуживает САМЫЕ
//      ранние вызовы (paging_init_kernel_map, APIC MMIO mapping) —
//      они происходят ДО того, как у нас будет разобрана настоящая
//      карта памяти (см. memmap.c), которая сама может требовать уже
//      работающего paging для чтения multiboot/UEFI структур.
//   2) как только memmap_activate() вызван, ВСЕ последующие вызовы
//      прозрачно уходят в memmap_alloc_frame() — по всей доступной
//      физической RAM, а не по крошечному фиксированному пулу.
// Настоящий EKA2 (и любое реальное ядро) использует битовую карту
// свободных страниц с возможностью освобождения — здесь по-прежнему
// только выделение, без free(), но уже по реальной карте памяти.
#define POOL_FRAMES 64
static uint8_t frame_pool[POOL_FRAMES][4096] __attribute__((aligned(4096)));
static int next_frame = 0;

extern int memmap_is_active(void);
extern void *memmap_alloc_frame(void);

// ПОПЫТКА защитить локом была ОТКАЧЕНА: alloc_frame() вызывается ОЧЕНЬ
// рано — из paging_init_kernel_map(), ДО того как LAPIC вообще
// замаплен/включён. NKern_Lock() теперь читает LAPIC MMIO через
// get_cpu_index(), а на этом этапе такое чтение либо падает, либо
// читает мусор — реальная регрессия (ядро вообще переставало
// загружаться, даже на одном CPU). В ТЕКУЩЕМ коде эта гонка не
// проявляется активно: все вызовы alloc_frame() при создании
// адресных пространств происходят последовательно на BSP до старта
// AP. Если в будущем появится ДИНАМИЧЕСКОЕ создание адресных
// пространств уже ПОСЛЕ старта SMP — защиту нужно будет добавить
// иначе (например, локальным spinlock без зависимости от LAPIC).
static void *alloc_frame(void) {
    void *frame;
    if (memmap_is_active()) {
        frame = memmap_alloc_frame();
    } else {
        if (next_frame >= POOL_FRAMES) {
            neka_panic("PAGING", PANIC_OUT_OF_FRAMES);
        }
        frame = frame_pool[next_frame++];
    }
    // Обнуляем — таблицы страниц не терпят мусора в неиспользуемых entries
    // (случайный установленный present-бит на мусорных данных = мгновенный
    // #GP/#PF при первом же обращении по такому "случайному" адресу).
    uint64_t *p = (uint64_t *)frame;
    for (int i = 0; i < 512; i++) p[i] = 0;
    return frame;
}

// Раньше здесь были extern-ссылки на pml4/pdpt из boot64.s — но это
// делало paging64.c зависимым от того, что ядро обязательно грузилось
// через наш ручной 32-битный boot-стаб. Под UEFI прошивка уже в long
// mode САМА, boot64.s вообще не выполняется — этих symbol'ов не
// существовало бы, линковка падала. Строим identity map на переносимом
// C, единообразно для ОБОИХ путей загрузки (GRUB/multiboot и UEFI).
static uint64_t *kernel_pml4 = NULL;
static uint64_t *kernel_pdpt = NULL;

// Строит identity map первого 1GB через 2MB-страницы (те же флаги
// present|writable|PS=0x83, что раньше строил asm-цикл в boot64.s) и
// переключает CR3 на эти таблицы. Вызывается ОДИН раз, самым первым
// делом в kernel_main64() — независимо от того, кто нас позвал (GRUB
// через multiboot уже поставил нас в какой-то identity map для перехода
// в long mode, UEFI тоже обычно даёт identity/flat map всей памяти —
// в обоих случаях мы всё равно строим СВОЙ, чтобы kernel_main64() не
// зависел от деталей конкретного загрузчика).
void paging_init_kernel_map(void) {
    kernel_pml4 = (uint64_t *)alloc_frame();
    kernel_pdpt = (uint64_t *)alloc_frame();
    uint64_t *pd = (uint64_t *)alloc_frame();

    for (int i = 0; i < 512; i++) {
        pd[i] = ((uint64_t)i << 21) | 0x83; // present | writable | PS(2MB)
    }
    kernel_pdpt[0] = (uint64_t)pd | 0x03;
    kernel_pml4[0] = (uint64_t)kernel_pdpt | 0x03;

    __asm__ volatile ("mov %0, %%cr3" : : "r"((uint64_t)kernel_pml4) : "memory");
}

// Нужен для SMP: AP-трамплин (ap_trampoline.s) грузит CR3 сам, ещё до
// того как у него будет доступ к каким-либо C-функциям — BSP кладёт
// это значение в "почтовый ящик" трамплина заранее (см. smp.c).
uint64_t paging_get_kernel_cr3(void) {
    return (uint64_t)kernel_pml4;
}

// Identity-мапит один 2MB-выровненный физический адрес как MMIO
// (present|writable|PS|PCD — PCD=cache-disable ОБЯЗАТЕЛЕН для MMIO:
// без него CPU может закэшировать чтение регистра APIC и больше никогда
// не увидеть его реальное обновлённое значение через шину). LAPIC
// (0xFEE00000 по умолчанию) и IOAPIC (0xFEC00000) лежат в третьем GB
// (0xC0000000-0xFFFFFFFF), которого наш обычный identity map (первый
// 1GB, PDPT[0]) не покрывает — нужен отдельный PDPT[3] со своим PD.
void paging_map_mmio_2mb(uint64_t pa) {
    int pdpt_idx = (pa >> 30) & 0x1FF;
    int pd_idx   = (pa >> 21) & 0x1FF;

    if (!(kernel_pdpt[pdpt_idx] & 1)) {
        uint64_t *new_pd = (uint64_t *)alloc_frame();
        kernel_pdpt[pdpt_idx] = (uint64_t)new_pd | 0x03;
    }
    uint64_t *pd = (uint64_t *)(kernel_pdpt[pdpt_idx] & ~0xFFFULL);
    pd[pd_idx] = pa | 0x93; // present | writable | PS | PCD(0x10)
}

#define USER_CODE_VA       0x50000000ULL
#define USER_STACK_PAGE_VA 0x50001000ULL // сама замапленная страница стека
#define USER_STACK_TOP     0x50002000ULL // RSP при входе — на 1 страницу выше

// Ищет/создаёт запись PDPT[идx] для произвольного va в ДАННОМ адресном
// пространстве (в отличие от create_address_space, которая жёстко
// заводила именно PDPT[1] под демо-страницу) — нужен более общий
// маппер, раз теперь адресов несколько (демо-страница + код/стек ring3).
static uint64_t *get_or_create_pdpt_entry_table(uint64_t *pml4_table, int pml4_idx) {
    if (!(pml4_table[pml4_idx] & 1)) {
        uint64_t *new_table = (uint64_t *)alloc_frame();
        pml4_table[pml4_idx] = (uint64_t)new_table | 0x03;
    }
    return (uint64_t *)(pml4_table[pml4_idx] & ~0xFFFULL);
}
static uint64_t *get_or_create_next_level(uint64_t *table, int idx, uint64_t flags) {
    if (!(table[idx] & 1)) {
        uint64_t *new_table = (uint64_t *)alloc_frame();
        table[idx] = (uint64_t)new_table | flags;
    }
    return (uint64_t *)(table[idx] & ~0xFFFULL);
}

// Маппит одну 4KB-страницу va -> frame В ДАННОМ адресном пространстве
// (задаётся своим PML4) с флагом USER (бит2=0x04) НА ВСЕХ уровнях —
// если хоть один уровень пути (PML4/PDPT/PD/PT) остался supervisor-only,
// intel трактует ВЕСЬ путь как недоступный для ring3 (AND по всем
// уровням), поэтому недостаточно выставить USER только на PT.
static void map_user_page(uint64_t *pml4_table, uint64_t va, void *frame) {
    int pml4_idx = (va >> 39) & 0x1FF;
    int pdpt_idx = (va >> 30) & 0x1FF;
    int pd_idx   = (va >> 21) & 0x1FF;
    int pt_idx   = (va >> 12) & 0x1FF;

    uint64_t user_flags = 0x07; // present | writable | USER
    uint64_t *pdpt_t = get_or_create_pdpt_entry_table(pml4_table, pml4_idx);
    uint64_t *pd_t   = get_or_create_next_level(pdpt_t, pdpt_idx, user_flags);
    uint64_t *pt_t   = get_or_create_next_level(pd_t, pd_idx, user_flags);
    pt_t[pt_idx] = (uint64_t)frame | user_flags;
}

// Крошечная ring3-"программа", закодированная вручную машинным кодом
// (нет линкера/компилятора для user-space — этот проект его не строит,
// поэтому просто кладём готовые байты x86-64 в память):
//   mov eax, 1         ; номер syscall (1 = print-char)
//   mov edi, 'X'        ; аргумент — символ для печати
// .loop:
//   int 0x80            ; передаём управление ядру (DPL=3 gate)
//   jmp .loop
static const uint8_t user_program[] = {
    0xB8, 0x01, 0x00, 0x00, 0x00,       // mov eax, 1
    0xBF, 0x58, 0x00, 0x00, 0x00,       // mov edi, 0x58 ('X')
    0xCD, 0x80,                          // int 0x80      <- loop:
    0xEB, 0xFC                           // jmp loop (rel8 = -4)
};

// Создаёт полноценный ring3-процесс: своё адресное пространство
// (наследует общий kernel identity map, как и остальные), плюс
// приватная страница кода (с нашей программой) и страница стека,
// обе с USER-битом. Возвращает CR3 для нового процесса.
// БАГ, найденный на практике (#PF при первом же lapic_send_eoi() из
// адресного пространства потока A): раньше здесь копировался только
// kernel_pdpt[0] (identity map первого 1GB) — но LAPIC/IOAPIC MMIO
// (paging_map_mmio_2mb) добавляет запись в kernel_pdpt[3], про которую
// каждое НОВОЕ адресное пространство просто не знало. Как только
// scheduler переключал CR3 на "приватный" PML4 потока, LAPIC EOI
// register переставал быть виден — не по конкретному индексу, а
// вообще любая MMIO-область, добавленная ПОСЛЕ создания потока.
// Фикс: копируем ВСЕ уже заполненные записи kernel_pdpt целиком, а не
// один захардкоженный индекс — тогда и будущие MMIO-регионы (если
// появятся) унаследуются автоматически, без похода в этот код снова.
static void inherit_kernel_pdpt(uint64_t *new_pdpt) {
    for (int i = 0; i < 512; i++) {
        if (kernel_pdpt[i] & 1) new_pdpt[i] = kernel_pdpt[i];
    }
}

uint64_t create_user_process(void) {
    uint64_t *new_pml4 = (uint64_t *)alloc_frame();
    uint64_t *new_pdpt = (uint64_t *)alloc_frame();
    inherit_kernel_pdpt(new_pdpt);
    // БАГ, найденный на практике (#PF при первой же instruction fetch
    // ring3-кода): здесь стоял флаг 0x03 (без USER-бита). Правило само
    // же описано в map_user_page() ниже — раз ЛЮБОЙ уровень path'а
    // remains supervisor-only, ВЕСЬ путь недоступен для ring3, — но тут
    // забыли его применить к самому верхнему уровню (PML4). Кернельная
    // identity-mapped область при этом остаётся защищена: её ЛИСТОВЫЕ
    // 2MB-записи (см. boot64.s, флаги 0x83) по-прежнему без USER-бита,
    // так что доступность верхних уровней тут ничего не открывает.
    new_pml4[0] = (uint64_t)new_pdpt | 0x07;

    void *code_frame  = alloc_frame();
    void *stack_frame = alloc_frame();

    // Копируем машинный код в физический фрейм. Мы обращаемся к нему
    // через его identity-mapped адрес (первый 1GB замаплен 1:1 для ВСЕХ
    // адресных пространств, включая то, из которого мы сейчас пишем эту
    // функцию, поэтому обычный memcpy-по-указателю работает без смены CR3).
    uint8_t *code_dst = (uint8_t *)code_frame;
    for (unsigned i = 0; i < sizeof(user_program); i++) code_dst[i] = user_program[i];

    map_user_page(new_pml4, USER_CODE_VA, code_frame);
    map_user_page(new_pml4, USER_STACK_PAGE_VA, stack_frame);

    return (uint64_t)new_pml4;
}


// Строит новое адресное пространство:
//   PML4[0] -> новый PDPT
//     PDPT[0] = pdpt[0] (унаследовано)  -> общий kernel PD, 2MB-страницы,
//               идентичный для всех — здесь живёт код ядра, стеки потоков,
//               VGA-буфер, наши page-tables. Без этого поток не смог бы
//               выполнить ни одной инструкции сразу после смены CR3.
//     PDPT[1] = новый приватный PD -> новый PT -> один приватный фрейм
//               по адресу DEMO_VA — вот эта часть у каждого потока своя.
uint64_t create_address_space(void) {
    uint64_t *new_pml4 = (uint64_t *)alloc_frame();
    uint64_t *new_pdpt = (uint64_t *)alloc_frame();
    uint64_t *priv_pd  = (uint64_t *)alloc_frame();
    uint64_t *priv_pt  = (uint64_t *)alloc_frame();
    void     *priv_frame = alloc_frame();

    inherit_kernel_pdpt(new_pdpt);
    priv_pt[0]  = (uint64_t)priv_frame | 0x03;          // present | writable
    priv_pd[0]  = (uint64_t)priv_pt    | 0x03;
    new_pdpt[1] = (uint64_t)priv_pd    | 0x03;
    new_pml4[0] = (uint64_t)new_pdpt   | 0x03;

    return (uint64_t)new_pml4; // это и есть будущее значение CR3 для потока
}
