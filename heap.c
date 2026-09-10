#include <stdint.h>
#include <stddef.h>

extern void kprint(const char *s);
extern void kprint_hex64(uint64_t val);
extern void neka_panic(const char *category, uint64_t code);
extern void NKern_Lock(void);
extern void NKern_Unlock(void);

#define PANIC_HEAP_OOM 1
#define PANIC_HEAP_CORRUPT 2

// Статическая арена под heap — 1 МБ. В настоящем ядре сюда обычно
// отдают диапазон виртуальных адресов и подгружают физические страницы
// по мере роста (как sbrk/brk в Unix); здесь для простоты — фиксированный
// заранее выделенный кусок памяти ядра.
#define HEAP_SIZE (1 * 1024 * 1024)
static uint8_t heap_arena[HEAP_SIZE] __attribute__((aligned(16)));

// Заголовок блока — общий как для занятых, так и для свободных блоков.
// free-блоки образуют двусвязный список через next/prev (explicit free
// list) — это быстрее, чем сканировать implicit list по всей памяти,
// потому что kmalloc проходит только по СВОБОДНЫМ блокам, не по занятым.
typedef struct block {
    size_t size;         // размер полезной нагрузки (без заголовка)
    int    free;
    struct block *next;  // следующий блок в памяти (для coalescing)
    struct block *prev;  // предыдущий блок в памяти (для coalescing)
} block_t;

// magic-число в начале полезной нагрузки каждого занятого блока — при
// kfree() проверяем его, чтобы поймать двойной free() или порчу памяти
// до того, как она разрушит весь heap молча (тот самый "непонятно
// откуда через 10 минут работы всё сломалось", который мы уже ловили
// в других частях NEKA через neka_panic).
#define BLOCK_MAGIC 0xDEADC0DEu

static block_t *heap_list = NULL; // список ВСЕХ блоков по памяти (не только свободных)

static size_t align16(size_t n) {
    return (n + 15) & ~((size_t)15);
}

void heap_init(void) {
    heap_list = (block_t *)heap_arena;
    heap_list->size = HEAP_SIZE - sizeof(block_t);
    heap_list->free = 1;
    heap_list->next = NULL;
    heap_list->prev = NULL;
}

// Ищет первый достаточно большой свободный блок (first-fit). Проще и
// быстрее в реализации, чем best-fit, ценой некоторой фрагментации —
// стандартный компромисс для kernel-аллокаторов такого масштаба.
static block_t *find_free_block(size_t size) {
    block_t *b = heap_list;
    while (b) {
        if (b->free && b->size >= size) return b;
        b = b->next;
    }
    return NULL;
}

// Разбивает блок на две части, если после выделения останется кусок,
// достаточно большой чтобы иметь смысл как отдельный свободный блок
// (иначе — трата на заголовок ради нескольких лишних байт).
static void split_block(block_t *b, size_t size) {
    size_t remaining = b->size - size;
    if (remaining <= sizeof(block_t) + 16) return; // не стоит того

    uint8_t *new_block_addr = (uint8_t *)(b + 1) + size;
    block_t *new_block = (block_t *)new_block_addr;
    new_block->size = remaining - sizeof(block_t);
    new_block->free = 1;
    new_block->next = b->next;
    new_block->prev = b;
    if (b->next) b->next->prev = new_block;
    b->next = new_block;
    b->size = size;
}

void *kmalloc(size_t size) {
    if (size == 0) return NULL;
    size = align16(size);

    // NKern::Lock()/Unlock() вокруг всей операции: список блоков — это
    // разделяемая структура, а find_free_block/split_block оставляют её
    // на несколько инструкций в промежуточном, ещё не консистентном
    // состоянии (например, между "нашли блок" и "разбили его на два").
    // Если таймер вытеснит поток ровно в эту секунду, а ДРУГОЙ поток
    // тоже вызовет kmalloc() — он увидит битый список. Раньше это не
    // проявлялось только потому, что heap использовался исключительно
    // до старта preemption (один поток, инициализация) — с ростом
    // проекта (SMP на подходе) полагаться на такое везение нельзя.
    NKern_Lock();

    // БАГ, найденный на практике самотестом (see heap_self_test): раньше
    // здесь искали блок размером ровно `size`, а потом ещё писали внутрь
    // него 4-байтовый magic — то есть реальный payload оказывался на
    // 4 байта МЕНЬШЕ обещанного вызывающему коду. Запись ровно в `size`
    // байт (как и положено после успешного kmalloc(size)) вылезала за
    // границу блока и затирала заголовок СЛЕДУЮЩЕГО блока — проявилось
    // как "free bytes" в heap_print_stats(), читающий уже испорченный
    // b->size, вдруг показывающий 0xCCCCCCCC (наш же тестовый паттерн).
    size_t total_needed = size + sizeof(uint32_t); // + место под magic

    block_t *b = find_free_block(total_needed);
    if (!b) {
        NKern_Unlock();
        neka_panic("HEAP", PANIC_HEAP_OOM);
        return NULL; // недостижимо после panic, но компилятор должен быть доволен
    }

    split_block(b, total_needed);
    b->free = 0;

    uint32_t *magic_slot = (uint32_t *)(b + 1);
    *magic_slot = BLOCK_MAGIC;

    NKern_Unlock();

    // Полезная нагрузка начинается ПОСЛЕ magic-числа — 4 байта уходят
    // под защиту, остальное (ровно `size`, как и обещали) отдаём вызывающему.
    return (void *)((uint8_t *)(b + 1) + sizeof(uint32_t));
}

// Сливает b со следующим блоком, если тот тоже свободен — без этого
// heap со временем превратится в россыпь мелких неиспользуемых дыр
// даже при полном освобождении всей памяти (классическая внешняя
// фрагментация).
static void coalesce_forward(block_t *b) {
    block_t *n = b->next;
    if (n && n->free) {
        b->size += sizeof(block_t) + n->size;
        b->next = n->next;
        if (n->next) n->next->prev = b;
    }
}

void kfree(void *ptr) {
    if (!ptr) return;

    NKern_Lock();

    uint32_t *magic_slot = (uint32_t *)((uint8_t *)ptr - sizeof(uint32_t));
    if (*magic_slot != BLOCK_MAGIC) {
        // Или двойной free(), или ptr не из kmalloc(), или что-то
        // затёрло память перед блоком — в любом случае лучше остановить
        // систему сейчас, чем позволить heap тихо развалиться.
        NKern_Unlock();
        neka_panic("HEAP", PANIC_HEAP_CORRUPT);
        return;
    }

    block_t *b = (block_t *)((uint8_t *)magic_slot - sizeof(block_t));
    b->free = 1;
    *magic_slot = 0; // гасим magic, чтобы повторный kfree() того же ptr тоже поймался

    coalesce_forward(b);
    if (b->prev && b->prev->free) coalesce_forward(b->prev);

    NKern_Unlock();
}

// Для отладки/самопроверки — суммарный объём свободной памяти в heap.
size_t heap_free_bytes(void) {
    size_t total = 0;
    block_t *b = heap_list;
    while (b) {
        if (b->free) total += b->size;
        b = b->next;
    }
    return total;
}

void heap_print_stats(void) {
    size_t free_bytes = heap_free_bytes();
    int block_count = 0;
    block_t *b = heap_list;
    while (b) { block_count++; b = b->next; }

    kprint("[heap] free=");
    kprint_hex64(free_bytes);
    kprint(" bytes, blocks=");
    kprint_hex64((uint64_t)block_count);
    kprint("\n");
}
