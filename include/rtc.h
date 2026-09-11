/* AresOS - RTC: часы реального времени (CMOS, порты 0x70/0x71), v0.8.0.
 * Читает дату/время из часов материнской платы: в VirtualBox это реальное
 * время хоста. Нужен экрану блокировки и часам на панели. */
#ifndef ARES_RTC_H
#define ARES_RTC_H

#include <stdint.h>

typedef struct {
    uint16_t year;   /* полный год, напр. 2026 */
    uint8_t  mon;    /* 1..12 */
    uint8_t  day;    /* 1..31 */
    uint8_t  hour;   /* 0..23 */
    uint8_t  min;    /* 0..59 */
    uint8_t  sec;    /* 0..59 */
    uint8_t  wday;   /* 0 = воскресенье .. 6 = суббота (вычислен) */
} rtc_time_t;

void rtc_read(rtc_time_t *t);
void rtc_log_boot(void);   /* строка в журнал при старте ядра */

#endif
