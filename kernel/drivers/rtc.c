/* AresOS - RTC (CMOS 0x70/0x71). Протокол: выбираем регистр через 0x70,
 * читаем байт из 0x71. Перед чтением ждём конца цикла обновления (UIP,
 * регистр 0x0A бит 7), иначе можно поймать "половину" нового времени.
 * Формат байт: BCD или двоичный - смотрим регистр 0x0B (bit2 = binary,
 * bit1 = 24-часовой режим). VirtualBox и qemu отдают BCD/24h. */
#include "rtc.h"
#include "io.h"
#include "kprintf.h"

#define CMOS_ADDR 0x70
#define CMOS_DATA 0x71

static uint8_t cmos(uint8_t reg) {
    outb(CMOS_ADDR, (uint8_t)(reg | 0x80));   /* bit7 = NMI disable (не трогаем чужое) */
    return inb(CMOS_DATA);
}

static uint8_t from_bcd(uint8_t v, int binary) {
    return binary ? v : (uint8_t)((v >> 4) * 10 + (v & 0x0F));
}

/* день недели по дате (алгоритм Томохико Сакамото): 0 = воскресенье */
static int calc_wday(int y, int m, int d) {
    static const int t[12] = { 0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4 };
    if (m < 3) y -= 1;
    return (y + y / 4 - y / 100 + y / 400 + t[m - 1] + d) % 7;
}

void rtc_read(rtc_time_t *t) {
    /* ждём конца обновления (UIP=0 держится >= 244 мкс - чтение успеет) */
    for (int i = 0; i < 100000; i++) {
        if (!(cmos(0x0A) & 0x80)) break;
    }
    uint8_t rb = cmos(0x0B);
    int binary = (rb & 0x04) != 0;
    int h24    = (rb & 0x02) != 0;

    t->sec  = from_bcd(cmos(0x00), binary);
    t->min  = from_bcd(cmos(0x02), binary);
    uint8_t hr = cmos(0x04);
    int pm = 0;
    if (!h24) { pm = (hr & 0x80) != 0; hr &= 0x7F; }
    t->hour = from_bcd(hr, binary);
    if (!h24) {                       /* 12-часовой -> 24-часовой */
        if (t->hour == 12) t->hour = 0;
        if (pm) t->hour = (uint8_t)(t->hour + 12);
    }
    t->day  = from_bcd(cmos(0x07), binary);
    t->mon  = from_bcd(cmos(0x08), binary);
    t->year = (uint16_t)(2000 + from_bcd(cmos(0x09), binary));
    /* столетие (регистр 0x32) у VBox/qemu обычно есть, но не обязателен;
     * 2000 + год - практичное допущение для нашей эпохи */
    if (t->mon < 1 || t->mon > 12) t->mon = 1;
    if (t->day < 1 || t->day > 31) t->day = 1;
    t->wday = (uint8_t)calc_wday(t->year, t->mon, t->day);
}

void rtc_log_boot(void) {
    rtc_time_t t;
    rtc_read(&t);
    kprintf("[rtc] часы CMOS: %04u-%02u-%02u %02u:%02u:%02u (д.нед. %u)\n",
            (uint64_t)t.year, (uint64_t)t.mon, (uint64_t)t.day,
            (uint64_t)t.hour, (uint64_t)t.min, (uint64_t)t.sec,
            (uint64_t)t.wday);
}
