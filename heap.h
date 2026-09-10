#ifndef HEAP_H
#define HEAP_H

#include <stddef.h>

void   heap_init(void);
void  *kmalloc(size_t size);
void   kfree(void *ptr);
size_t heap_free_bytes(void);
void   heap_print_stats(void);

#endif
