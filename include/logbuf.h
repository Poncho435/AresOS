/* AresOS - logbuf: кольцевой буфер строк журнала ядра (v0.5.0).
 * kprintf пишет сюда КАЖДЫЙ символ - журнал можно показать в приложении
 * "Логи" на рабочем столе, не дёргая serial/fb-консоль. */
#ifndef ARES_LOGBUF_H
#define ARES_LOGBUF_H

#define LOG_LINES 192          /* сколько последних строк храним */
#define LOG_COLS  120          /* макс. длина строки (длиннее - рвём) */

void        logbuf_putc(char c);        /* поток символов из kprintf      */
int         logbuf_count(void);         /* сколько строк сейчас в буфере  */
const char *logbuf_line(int i);         /* i = 0 (самая старая) .. count-1 */

#endif
