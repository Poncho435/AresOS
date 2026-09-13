/* AresOS - kernel heap: kmalloc/kfree (M3).
 * Арена 2 МиБ на собственной VMM (физ. страницы любые, VA непрерывны).
 * Аллокатор K&R: адресно-упорядоченный свободный список, split+coalesce. */
#ifndef ARES_HEAP_H
#define ARES_HEAP_H

#include <stddef.h>

void  heap_init(void);
void *kmalloc(size_t n);
void  kfree(void *p);

/* v0.8.1: стек потока с НЕзамапленной страницей-часовым снизу.
 * Возвращает дно пригодной области (стек растёт вниз от base+size).
 * Переполнение упирается в guard-страницу -> #PF на IST-стеке -> честная
 * паника с именем потока, а не triple fault / молчаливая порча кучи. */
void *kstack_alloc(unsigned size, unsigned long *out_guard);
void  kstack_free(void *base, unsigned size);

/* принадлежит ли адрес стеку какого-либо потока (диагностика #PF) */
int   kstack_is_guard(unsigned long addr);

/* границы стека потока, которому принадлежит addr (для backtrace). 1 - нашли */
int   kstack_bounds(unsigned long addr, unsigned long *lo, unsigned long *hi);
void  heap_stress_test(void);      /* миллион случайных alloc/free - DoD M3 */
size_t heap_free_bytes(void);

#endif
