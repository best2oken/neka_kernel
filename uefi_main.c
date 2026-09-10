#include <efi.h>
#include <efilib.h>

// Наше собственное ядро (тот же kernel_main64, что грузился раньше через
// GRUB/multiboot) — под UEFI мы вызываем его напрямую, без multiboot.
extern void kernel_main64(uint64_t mbi_addr);
extern void memmap_parse_uefi(void *map, uint64_t map_size, uint64_t descriptor_size);

// UEFI application entry point — сигнатура заданная спецификацией UEFI,
// а не нами: (EFI_HANDLE, EFI_SYSTEM_TABLE*) -> EFI_STATUS.
EFI_STATUS
EFIAPI
efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable)
{
    InitializeLib(ImageHandle, SystemTable);

    // Печатаем через настоящий UEFI Simple Text Output Protocol — если
    // это видно, значит мы реально запущены прошивкой UEFI (не BIOS/GRUB),
    // и firmware уже перевела нас в 64-битный long mode сама, без нашего
    // ручного boot64.s (см. README — вот в чём была вся экономия усилий).
    Print(L"NEKA UEFI bootloader starting...\r\n");
    Print(L"Firmware already in long mode -- no manual boot64.s dance needed.\r\n");

    // Получаем карту памяти и вызываем ExitBootServices — единственный
    // способ по-настоящему "забрать" машину у прошивки. После этого
    // ЛЮБЫЕ UEFI Boot Services (включая тот же Print через ConOut) уже
    // недействительны — только Runtime Services (которых мы не используем).
    UINTN MapKey, MapSize = 0, DescriptorSize;
    UINT32 DescriptorVersion;
    EFI_MEMORY_DESCRIPTOR *MemoryMap = NULL;

    // Первый вызов с MapSize=0 намеренно возвращает EFI_BUFFER_TOO_SMALL,
    // сообщая реальный нужный размер — стандартная двухфазная процедура UEFI.
    uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
        &MapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);

    // Берём с запасом — между этим вызовом и следующим GetMemoryMap САМА
    // аллокация может изменить карту памяти (добавить дескриптор), это
    // тоже стандартная практика UEFI-загрузчиков, а не наша прихоть.
    MapSize += DescriptorSize * 8;
    uefi_call_wrapper(SystemTable->BootServices->AllocatePool, 3,
        EfiLoaderData, MapSize, (void **)&MemoryMap);

    uefi_call_wrapper(SystemTable->BootServices->GetMemoryMap, 5,
        &MapSize, MemoryMap, &MapKey, &DescriptorSize, &DescriptorVersion);

    Print(L"Exiting UEFI boot services...\r\n");
    uefi_call_wrapper(SystemTable->BootServices->ExitBootServices, 2,
        ImageHandle, MapKey);

    // Память самого буфера MemoryMap остаётся валидной (ExitBootServices
    // не трогает содержимое, только делает недействительным ФУНКЦИОНАЛ
    // Boot Services) — разбираем его в наш общий формат ПРЯМО СЕЙЧАС,
    // пока указатель ещё жив и осмыслен. type==EfiConventionalMemory
    // фильтрация внутри memmap_parse_uefi() автоматически исключает наш
    // же образ (он размечен прошивкой как LoaderCode/LoaderData, не
    // Conventional) — в отличие от multiboot-пути, отдельно вычислять
    // свои собственные границы здесь не нужно.
    memmap_parse_uefi(MemoryMap, MapSize, DescriptorSize);

    // С этой точки мы одни на машине — ровно то состояние, в котором
    // раньше нас оставлял GRUB после multiboot. kernel_main64() ничего
    // не знает и не должен знать о том, кто его вызвал: своя GDT/IDT/
    // page tables оно строит с нуля само (gdt_init/idt_init/paging64.c),
    // не полагаясь на то, что уже настроила прошивка. 0 вместо mbi_addr
    // — сигнал "multiboot info нет, но карта памяти уже разобрана извне".
    kernel_main64(0);

    // Недостижимо — kernel_main64() не возвращается.
    return EFI_SUCCESS;
}
