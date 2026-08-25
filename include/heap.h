/* AresOS — kernel heap: kmalloc/kfree (M3).
 * Арена 2 МиБ на собственной VMM (физ. страницы любые, VA непрерывны).
 * Аллокатор K&R: адресно-упорядоченный свободный список, split+coalesce. */
#ifndef ARES_HEAP_H
#define ARES_HEAP_H

#include <stddef.h>

void  heap_init(void);
void *kmalloc(size_t n);
void  kfree(void *p);
void  heap_stress_test(void);      /* миллион случайных alloc/free — DoD M3 */
size_t heap_free_bytes(void);

#endif
