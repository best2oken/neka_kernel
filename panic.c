#include <stdint.h>

extern void kprint(const char *s);
extern void kprint_hex64(uint64_t val);

// Аналог User::Panic(category, code) из Symbian: неинформативному
// "зависло/сломалось непонятно почему" предпочитаем явную остановку
// с категорией и кодом — по духу того же принципа, что и наш
// interrupt_dispatch() для CPU-исключений, только для программных
// ошибок инициализации (которые CPU сам не поймает никаким #GP/#UD).
void neka_panic(const char *category, uint64_t code) {
    kprint("\n*** NEKA PANIC [");
    kprint(category);
    kprint("] code=");
    kprint_hex64(code);
    kprint("\nSystem halted.\n");
    __asm__ volatile ("cli");
    for (;;) { __asm__ volatile ("hlt"); }
}
