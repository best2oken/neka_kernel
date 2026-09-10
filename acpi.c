#include <stdint.h>
#include <stddef.h>

extern void kprint(const char *s);
extern void kprint_hex64(uint64_t val);

// --- RSDP (Root System Description Pointer) ---
// ACPI 1.0 (20 байт) и 2.0+ (36 байт, добавляет XsdtAddress) — layout
// задокументирован ACPI spec, реализация ABI, не чей-то авторский код.
typedef struct __attribute__((packed)) {
    char     signature[8]; // "RSD PTR "
    uint8_t  checksum;
    char     oem_id[6];
    uint8_t  revision;
    uint32_t rsdt_address;
    // --- ACPI 2.0+ ---
    uint32_t length;
    uint64_t xsdt_address;
    uint8_t  extended_checksum;
    uint8_t  reserved[3];
} rsdp_t;

typedef struct __attribute__((packed)) {
    char     signature[4];
    uint32_t length;
    uint8_t  revision;
    uint8_t  checksum;
    char     oem_id[6];
    char     oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} sdt_header_t;

typedef struct __attribute__((packed)) {
    sdt_header_t header;
    uint32_t local_apic_address;
    uint32_t flags;
    uint8_t  entries[]; // переменной длины, см. madt_entry_header_t
} madt_t;

typedef struct __attribute__((packed)) {
    uint8_t type;   // 0 = Processor Local APIC
    uint8_t length;
} madt_entry_header_t;

typedef struct __attribute__((packed)) {
    madt_entry_header_t header;
    uint8_t  acpi_processor_id;
    uint8_t  apic_id;
    uint32_t flags; // бит0 = enabled
} madt_local_apic_entry_t;

#define MADT_TYPE_LOCAL_APIC 0

// MADT Type 1 — I/O APIC. Реальный физический адрес IOAPIC ЗДЕСЬ, а не
// в универсальной константе 0xFEC00000 — на большинстве систем совпадает
// с дефолтом, но полагаться на это вместо реального чтения MADT было
// именно тем, на что справедливо указало код-ревью (см. README).
typedef struct __attribute__((packed)) {
    madt_entry_header_t header;
    uint8_t  io_apic_id;
    uint8_t  reserved;
    uint32_t io_apic_address;
    uint32_t global_system_interrupt_base;
} madt_ioapic_entry_t;

#define MADT_TYPE_IO_APIC 1
#define MADT_FLAG_ENABLED 1

static rsdp_t *g_rsdp = NULL;

// --- Поиск RSDP на BIOS-системах (multiboot/GRUB путь) ---
// UEFI отдаёт указатель на RSDP напрямую через Configuration Table —
// цивилизованно. На "голом" BIOS такого сервиса нет, ACPI spec
// требует буквально СКАНИРОВАТЬ память в двух местах: первый килобайт
// EBDA (Extended BIOS Data Area, её сегмент лежит по фиксированному
// адресу 0x40E) и диапазон 0xE0000-0xFFFFF (область BIOS ROM).
static int checksum_ok(const uint8_t *data, uint32_t len) {
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum += data[i];
    return sum == 0;
}

static rsdp_t *scan_for_rsdp(uint64_t start, uint64_t end) {
    for (uint64_t addr = start; addr < end; addr += 16) {
        rsdp_t *candidate = (rsdp_t *)addr;
        if (candidate->signature[0] == 'R' && candidate->signature[1] == 'S' &&
            candidate->signature[2] == 'D' && candidate->signature[3] == ' ' &&
            candidate->signature[4] == 'P' && candidate->signature[5] == 'T' &&
            candidate->signature[6] == 'R' && candidate->signature[7] == ' ') {
            if (checksum_ok((uint8_t *)candidate, 20)) { // базовая ACPI 1.0 часть всегда проверяется так
                return candidate;
            }
        }
    }
    return NULL;
}

void acpi_find_rsdp_bios(void) {
    if (g_rsdp) return; // уже установлен извне (UEFI-путь) — не перезаписываем

    // EBDA: сегмент лежит по физическому адресу 0x40E (BIOS Data Area).
    // GCC подозревает разыменование "почти нулевого" указателя как баг
    // (-Warray-bounds) — здесь это осознанное чтение настоящего
    // физического адреса в bare-metal окружении, ложное срабатывание;
    // через explicit-указатель эвристика компилятора не срабатывает.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    volatile uint16_t *ebda_seg_ptr = (volatile uint16_t *)(uintptr_t)0x40E;
    uint16_t ebda_seg = *ebda_seg_ptr;
#pragma GCC diagnostic pop
    uint64_t ebda_addr = ((uint64_t)ebda_seg) << 4;
    if (ebda_addr != 0) {
        g_rsdp = scan_for_rsdp(ebda_addr, ebda_addr + 1024);
    }
    if (!g_rsdp) {
        g_rsdp = scan_for_rsdp(0xE0000, 0x100000);
    }
    if (!g_rsdp) {
        kprint("[acpi] WARNING: RSDP not found via BIOS scan\n");
    }
}

// UEFI путь: uefi_main.c зовёт это НАПРЯМУЮ с указателем из
// SystemTable->ConfigurationTable (ACPI_20_TABLE_GUID/ACPI_TABLE_GUID),
// минуя memory scan целиком — прошивка просто ЗНАЕТ адрес.
void acpi_set_rsdp(void *rsdp) {
    g_rsdp = (rsdp_t *)rsdp;
}

// Ищет MADT ("APIC") среди таблиц RSDT (32-битные указатели) или XSDT
// (64-битные, только если RSDP revision>=2) — выбор делается по тому,
// что реально предоставил RSDP, а не по вкусу.
static madt_t *find_madt(void) {
    if (!g_rsdp) return NULL;

    if (g_rsdp->revision >= 2 && g_rsdp->xsdt_address != 0) {
        sdt_header_t *xsdt = (sdt_header_t *)g_rsdp->xsdt_address;
        uint32_t count = (xsdt->length - sizeof(sdt_header_t)) / 8;
        uint64_t *tables = (uint64_t *)((uint8_t *)xsdt + sizeof(sdt_header_t));
        for (uint32_t i = 0; i < count; i++) {
            sdt_header_t *t = (sdt_header_t *)tables[i];
            if (t->signature[0] == 'A' && t->signature[1] == 'P' &&
                t->signature[2] == 'I' && t->signature[3] == 'C') {
                return (madt_t *)t;
            }
        }
    } else {
        sdt_header_t *rsdt = (sdt_header_t *)(uint64_t)g_rsdp->rsdt_address;
        uint32_t count = (rsdt->length - sizeof(sdt_header_t)) / 4;
        uint32_t *tables = (uint32_t *)((uint8_t *)rsdt + sizeof(sdt_header_t));
        for (uint32_t i = 0; i < count; i++) {
            sdt_header_t *t = (sdt_header_t *)(uint64_t)tables[i];
            if (t->signature[0] == 'A' && t->signature[1] == 'P' &&
                t->signature[2] == 'I' && t->signature[3] == 'C') {
                return (madt_t *)t;
            }
        }
    }
    return NULL;
}

// Достаёт РЕАЛЬНЫЕ физические адреса LAPIC (поле в самом заголовке MADT)
// и IOAPIC (Type 1 entry) — вместо того чтобы полагаться на константы
// 0xFEE00000/0xFEC00000, которые лишь "почти всегда" верны. Возвращает
// 1 при успехе, 0 если MADT недоступен (вызывающий код тогда сам решает,
// использовать ли дефолты как fallback).
int acpi_find_apic_bases(uint64_t *lapic_pa_out, uint64_t *ioapic_pa_out) {
    madt_t *madt = find_madt();
    if (!madt) return 0;

    *lapic_pa_out = (uint64_t)madt->local_apic_address;
    *ioapic_pa_out = 0;

    uint8_t *ptr = madt->entries;
    uint8_t *end = (uint8_t *)madt + madt->header.length;
    while (ptr < end) {
        madt_entry_header_t *eh = (madt_entry_header_t *)ptr;
        if (eh->type == MADT_TYPE_IO_APIC) {
            madt_ioapic_entry_t *e = (madt_ioapic_entry_t *)ptr;
            *ioapic_pa_out = (uint64_t)e->io_apic_address;
            break; // берём первый IOAPIC — многие системы имеют несколько, но нам достаточно одного для демо
        }
        ptr += eh->length;
    }

    return (*ioapic_pa_out != 0);
}

// Перечисляет включённые (flags бит0=1) Local APIC ID из MADT — это и
// есть настоящий список ядер CPU, которые физически можно разбудить
// через INIT-SIPI-SIPI, вместо того чтобы гадать константой "2" или
// "4". Возвращает количество найденных, максимум max_cpus штук.
int acpi_find_cpus(uint8_t *apic_ids_out, int max_cpus) {
    madt_t *madt = find_madt();
    if (!madt) {
        kprint("[acpi] WARNING: MADT not found, assuming single-CPU\n");
        return 0;
    }

    int found = 0;
    uint8_t *ptr = madt->entries;
    uint8_t *end = (uint8_t *)madt + madt->header.length;
    while (ptr < end && found < max_cpus) {
        madt_entry_header_t *eh = (madt_entry_header_t *)ptr;
        if (eh->type == MADT_TYPE_LOCAL_APIC) {
            madt_local_apic_entry_t *e = (madt_local_apic_entry_t *)ptr;
            if (e->flags & MADT_FLAG_ENABLED) {
                apic_ids_out[found++] = e->apic_id;
            }
        }
        ptr += eh->length;
    }

    kprint("[acpi] MADT: found ");
    kprint_hex64((uint64_t)found);
    kprint(" enabled CPU(s)\n");
    return found;
}
