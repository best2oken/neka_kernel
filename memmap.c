#include <stdint.h>
#include <stddef.h>

extern void kprint(const char *s);
extern void kprint_hex64(uint64_t val);
extern void neka_panic(const char *category, uint64_t code);

#define PANIC_MEMMAP_OOM 1

// Единый, не привязанный к конкретному загрузчику формат "свободный
// регион физической памяти". multiboot и UEFI описывают это совершенно
// по-разному (multiboot_mmap_entry vs EFI_MEMORY_DESCRIPTOR) — вся
// разница остаётся внутри memmap_parse_multiboot()/memmap_parse_uefi(),
// а остальной код ядра (paging64.c) вообще не знает, кто нас грузил.
typedef struct {
    uint64_t base;
    uint64_t length;
} region_t;

#define MAX_REGIONS 64
static region_t regions[MAX_REGIONS];
static int region_count = 0;

static void add_region(uint64_t base, uint64_t length) {
    if (region_count >= MAX_REGIONS) return; // молча игнорируем лишние — 64 региона с большим запасом
    regions[region_count].base = base;
    regions[region_count].length = length;
    region_count++;
}

// --- Multiboot1 memory map ---
// Структуры ниже — реализация задокументированного бинарного ABI
// multiboot1 spec, а не чей-то код; layout стандартный и стабильный.
typedef struct __attribute__((packed)) {
    uint32_t flags;
    uint32_t mem_lower, mem_upper;
    uint32_t boot_device;
    uint32_t cmdline;
    uint32_t mods_count, mods_addr;
    uint32_t syms[4];
    uint32_t mmap_length, mmap_addr;
} multiboot_info_t;

typedef struct __attribute__((packed)) {
    uint32_t size; // размер записи БЕЗ учёта этого самого поля
    uint64_t addr;
    uint64_t len;
    uint32_t type; // 1 = доступная RAM
} multiboot_mmap_entry_t;

#define MULTIBOOT_FLAG_MMAP (1u << 6)
#define MULTIBOOT_MEM_AVAILABLE 1

void memmap_parse_multiboot(uint64_t mbi_addr) {
    if (mbi_addr == 0) return; // UEFI-путь передаёт 0 — карта уже разобрана иначе

    multiboot_info_t *info = (multiboot_info_t *)mbi_addr;
    if (!(info->flags & MULTIBOOT_FLAG_MMAP)) {
        // GRUB не предоставил карту (не должно случаться — мы явно
        // запросили её в заголовке, см. boot64.s), но лучше явно
        // остановиться, чем молча работать с пустым списком регионов.
        kprint("[memmap] WARNING: GRUB did not provide memory map\n");
        return;
    }

    uint64_t ptr = info->mmap_addr;
    uint64_t end = info->mmap_addr + info->mmap_length;
    while (ptr < end) {
        multiboot_mmap_entry_t *e = (multiboot_mmap_entry_t *)ptr;
        if (e->type == MULTIBOOT_MEM_AVAILABLE) {
            add_region(e->addr, e->len);
        }
        ptr += e->size + sizeof(e->size); // size не включает само поле size
    }
}

// --- UEFI memory map ---
// Стандартный layout EFI_MEMORY_DESCRIPTOR — тоже задокументированный
// UEFI spec ABI, не чей-то авторский код.
typedef struct __attribute__((packed)) {
    uint32_t type;
    uint32_t pad;
    uint64_t phys_start;
    uint64_t virt_start;
    uint64_t num_pages;
    uint64_t attribute;
} efi_mem_descriptor_t;

#define EFI_CONVENTIONAL_MEMORY 7

void memmap_parse_uefi(void *map, uint64_t map_size, uint64_t descriptor_size) {
    for (uint64_t offset = 0; offset < map_size; offset += descriptor_size) {
        efi_mem_descriptor_t *d = (efi_mem_descriptor_t *)((uint8_t *)map + offset);
        // Только EfiConventionalMemory — все остальные типы (LoaderCode/
        // LoaderData, куда как раз попадает НАШ СОБСТВЕННЫЙ образ, ACPI
        // reclaim/NVS, MMIO и т.д.) намеренно пропускаем. Раз наш образ
        // отмечен прошивкой как LoaderData/LoaderCode, а не Conventional —
        // эта фильтрация автоматически защищает нас от самоперезаписи,
        // без необходимости знать свои собственные границы вручную (в
        // отличие от multiboot-пути, где такой автоматики нет).
        if (d->type == EFI_CONVENTIONAL_MEMORY) {
            add_region(d->phys_start, d->num_pages * 4096);
        }
    }
}

// БАГ, найденный на практике (краш на UEFI+SMP уже ПОСЛЕ фикса per-CPU
// TSS): AP-трамплин копируется на ЖЁСТКО ЗАДАННЫЙ физический адрес
// 0x8000 — это стандартная BIOS-эра конвенция ("низкая память ниже
// 1MB свободна после boot"), но под UEFI образ ядра грузится по
// адресу, который выбирает САМА прошивка (наш линкер-скрипт использует
// ImageBase=0, релоцируется загрузчиком) — 0x8000 НЕ гарантированно
// свободен. Эта функция проверяет, покрыт ли данный диапазон хотя бы
// ОДНИМ уже разобранным regions[] (то есть реально помечен прошивкой
// как EfiConventionalMemory) — вызывается ПЕРЕД записью trampoline,
// а не после того как что-то уже тихо испортилось.
int memmap_region_is_safe(uint64_t addr, uint64_t length) {
    for (int i = 0; i < region_count; i++) {
        if (addr >= regions[i].base && addr + length <= regions[i].base + regions[i].length) {
            return 1;
        }
    }
    return 0;
}

// --- Активация: переключение аллокатора фреймов на разобранную карту ---
static int map_active = 0;
static uint64_t reserve_start = 0, reserve_end = 0; // диапазон "не трогать" (свой образ)
static int cur_region = 0;
static uint64_t cur_offset = 0;

void memmap_activate(uint64_t reserved_lo, uint64_t reserved_hi) {
    reserve_start = reserved_lo;
    reserve_end = reserved_hi;
    cur_region = 0;
    cur_offset = 0;
    map_active = 1;

    uint64_t total = 0;
    for (int i = 0; i < region_count; i++) total += regions[i].length;
    kprint("[memmap] activated: ");
    kprint_hex64((uint64_t)region_count);
    kprint(" regions, ");
    kprint_hex64(total);
    kprint(" bytes total usable RAM reported\n");
}

int memmap_is_active(void) { return map_active; }

// Простой bump-allocator ПОВЕРХ разобранной карты: идём по регионам
// последовательно, пропуская всё ниже 1MB (легаси BIOS/EBDA/video —
// стандартная практика их не трогать) и весь диапазон [reserve_start,
// reserve_end) — наш собственный загруженный образ на multiboot-пути
// (на UEFI-пути этот диапазон намеренно (0,0) — там уже не нужен,
// см. комментарий в memmap_parse_uefi).
void *memmap_alloc_frame(void) {
    while (cur_region < region_count) {
        region_t *r = &regions[cur_region];
        // cur_offset везде ниже отсчитывается от base_aligned — единая
        // точка отсчёта, без путаницы с невыровненной r->base.
        uint64_t base_aligned = (r->base + 0xFFF) & ~0xFFFULL;
        uint64_t addr = base_aligned + cur_offset;
        uint64_t region_end = r->base + r->length;

        if (addr >= region_end) {
            cur_region++;
            cur_offset = 0;
            continue;
        }
        if (addr < 0x100000) { // пропускаем всё ниже 1MB целиком (легаси BIOS/EBDA/video)
            cur_offset = (0x100000 > base_aligned) ? (0x100000 - base_aligned) : 0;
            continue;
        }
        if (addr < reserve_end && addr + 4096 > reserve_start) {
            // Попадание на собственный образ — перепрыгиваем целиком за него
            cur_offset = (reserve_end > base_aligned) ? (reserve_end - base_aligned) : cur_offset + 4096;
            continue;
        }

        cur_offset += 4096;
        return (void *)addr;
    }
    neka_panic("MEMMAP", PANIC_MEMMAP_OOM);
    return NULL; // недостижимо
}
