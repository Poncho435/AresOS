/* AresOS - планировщик потоков ядра (M5).
 * Переключение - ТОЛЬКО в тике таймера: все кадры прерываний имеют одну
 * форму, поток возобновляется ровно там, где был вытеснен.
 * Поток живёт в ring-0 со своим 16-КиБ стеком. */
#ifndef ARES_PROC_H
#define ARES_PROC_H

#include <stdint.h>

typedef void (*proc_fn)(void *arg);

enum { PROC_READY = 0, PROC_RUNNING = 1, PROC_SLEEP = 2, PROC_DEAD = 3 };
#define PROC_F_BACKGROUND 1

typedef struct {
    int      id;
    char     name[16];
    int      state;
    int      flags;         /* PROC_F_BACKGROUND */
    uint64_t ticks;         /* сколько тиков CPU накопил */
} proc_info_t;

int      proc_init(void);                  /* текущий поток становится "idle" (id 0) */
int      proc_spawn(proc_fn fn, void *arg, const char *name, int flags); /* id или -1 */
void     sched_tick(void);                 /* из обработчика таймера */
uint64_t sched_ticks(void);
void     proc_sleep(uint32_t ms);          /* тики 10 мс */
void     proc_yield(void);
int      proc_list(proc_info_t *out, int max);
const char *proc_state_name(int s);
int      proc_kill_slot(int slot);     /* v0.6.0: остановить процесс по слоту (0..3 - системные, нельзя) */
int      proc_state_at(int slot);

#endif
