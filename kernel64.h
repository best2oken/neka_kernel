#ifndef KERNEL64_H
#define KERNEL64_H

#include <stdint.h>

#define MAX_THREADS64    16
#define NUM_PRIORITIES64 8
#define STACK_SIZE64     8192

// Виртуальный адрес демо-страницы, одинаковый для всех адресных
// пространств — см. paging64.c для объяснения, почему изоляция
// доказывается именно так.
#define DEMO_VA 0x40000000ULL

void paging_init_kernel_map(void);
void paging_map_mmio_2mb(uint64_t pa);
uint64_t paging_get_kernel_cr3(void);

void memmap_parse_multiboot(uint64_t mbi_addr);
void memmap_parse_uefi(void *map, uint64_t map_size, uint64_t descriptor_size);
void memmap_activate(uint64_t reserved_lo, uint64_t reserved_hi);
int  memmap_is_active(void);
int  memmap_region_is_safe(uint64_t addr, uint64_t length);

void NKern_Lock(void);
void NKern_Unlock(void);
int  NKern_IsLocked(void);
void NKern_RequestReschedule(void);
void NKern_LockForSwitch(void);
void NKern_UnlockAfterSwitch(void);

void acpi_find_rsdp_bios(void);
void acpi_set_rsdp(void *rsdp);
int  acpi_find_cpus(uint8_t *apic_ids_out, int max_cpus);
int  acpi_find_apic_bases(uint64_t *lapic_pa_out, uint64_t *ioapic_pa_out);

int smp_start_aps(void);
void *memmap_alloc_frame(void);

void apic_check_supported(void);
void apic_apply_acpi_bases(void);
void disable_legacy_pic(void);
void lapic_enable(void);
void lapic_send_eoi(void);
void apic_timer_init(uint8_t vector, uint32_t hz);
void ioapic_set_redirect(uint8_t irq, uint8_t vector);
uint8_t lapic_get_id(void);
int get_cpu_index(void);
uint64_t create_address_space(void);
uint64_t create_user_process(void);
extern void gdt_init(void);
uint64_t gdt_get_ptr_address(void);
uint64_t gdt_get_tss_rsp0(int cpu_idx);
void gdt_setup_cpu_tss(int cpu_idx);
extern void enter_usermode(uint64_t entry, uint64_t user_stack_top);

#define USER_CODE_VA       0x50000000ULL
#define USER_STACK_PAGE_VA 0x50001000ULL
#define USER_STACK_TOP     0x50002000ULL

typedef enum {
    T64_UNUSED = 0,
    T64_READY,
    T64_RUNNING
} thread64_state_t;

typedef struct thread64 {
    uint64_t   rsp;
    uint64_t   cr3;            // физ.адрес PML4 этого адресного пространства
    uint8_t    priority;
    thread64_state_t state;
    uint32_t   time_slice;
    uint64_t   stack[STACK_SIZE64 / 8] __attribute__((aligned(16)));
    const char *name;
    struct thread64 *next;    // кольцевая очередь потоков одного приоритета
} thread64_t;

#define MAX_SCHED_CPUS 16

typedef struct {
    uint32_t ready_bitmap;
    thread64_t *ready_queue[NUM_PRIORITIES64];
    // БАГ, найденный на практике (см. README): раньше здесь было ОДНО
    // поле `thread64_t *current` — работало, пока был один CPU. С
    // приходом SMP два ядра реально выполняют РАЗНЫЕ потоки одновременно,
    // и общее поле "текущий поток" для ВСЕХ ядер сразу — по определению
    // неверно (кто последний записал, тот и "истина" для обоих ядер).
    // current[] индексируется по cpu_idx() — у каждого ядра СВОЙ слот.
    thread64_t *current[MAX_SCHED_CPUS];
} scheduler64_t;

void sched64_init(void);
thread64_t *sched64_create_thread(void (*entry)(void), uint8_t priority, const char *name);
void sched64_reschedule(void);
void sched64_timer_tick(void); // вызывается из IRQ0 — настоящий preemptive tick

extern void context_switch64(uint64_t *old_rsp_ptr, uint64_t new_rsp);

#endif
