/* AresOS - мышь PS/2 через контроллер 8042 (порты 0x60/0x64), IRQ12.
 * Пакеты 3 байта; после IntelliMouse-последовательности - 4 (колесо!). */
#include "mouse.h"
#include "io.h"
#include "serial.h"   /* для отладочного лога init */
#include "kprintf.h"
#include <stdint.h>

#define PS2_DATA 0x60
#define PS2_CMD  0x64
#define PS2_STATUS 0x64
#define PS2_ACK 0xFA

static volatile int32_t g_x = 100, g_y = 100;
static volatile int     g_left;
static volatile int     g_moved;
static volatile int     g_wheel;        /* накопленное колесо (+ вверх/- вниз) */
static int     g_wheel_capable;
static uint8_t  g_packet[4];
static uint8_t  g_cycle;
static volatile uint32_t g_irq_count;   /* v0.3.1: только счётчик - НЕ печатаем из IRQ! */

/* ограничители - устанавливаются десктопом после инициализации графики */
static int32_t g_max_x = 1024, g_max_y = 768;

static void ps2_wait_read(void) {
    for (int i = 0; i < 100000; i++) {
        if (inb(PS2_STATUS) & 0x01) return;
    }
}
static void ps2_wait_write(void) {
    for (int i = 0; i < 100000; i++) {
        if (!(inb(PS2_STATUS) & 0x02)) return;
    }
}

static int mouse_write(uint8_t cmd) {
    ps2_wait_write();
    outb(PS2_CMD, 0xD4);          /* следующий байт - мыши */
    ps2_wait_write();
    outb(PS2_DATA, cmd);
    ps2_wait_read();
    return inb(PS2_DATA) == PS2_ACK;
}

void mouse_init(void) {
    g_irq_count = 0;
    /* слить возможный мусор в буфере */
    while (inb(PS2_STATUS) & 0x01) inb(PS2_DATA);

    ps2_wait_write();
    outb(PS2_CMD, 0xA8);          /* включить AUX (мышь) */

    /* Controller Configuration Byte: разрешить IRQ12 (bit1) */
    ps2_wait_write(); outb(PS2_CMD, 0x20);
    ps2_wait_read();
    uint8_t ccb = inb(PS2_DATA);
    ps2_wait_write(); outb(PS2_CMD, 0x60);
    ps2_wait_write(); outb(PS2_DATA, ccb | 0x02);

    int ok = 1;
    ok &= mouse_write(0xF6);      /* set defaults */

    /* IntelliMouse: магия "200,100,80" включает 4-байтный режим с колесом */
    ok &= mouse_write(0xF3); ok &= mouse_write(200);
    ok &= mouse_write(0xF3); ok &= mouse_write(100);
    ok &= mouse_write(0xF3); ok &= mouse_write(80);
    ok &= mouse_write(0xF2);      /* Get Device ID */
    uint8_t devid = 0;
    ps2_wait_read();
    devid = inb(PS2_DATA);
    g_wheel_capable = (devid == 0x03);

    ok &= mouse_write(0xF3);      /* sample rate... */
    ok &= mouse_write(100);       /* ...100 Гц */
    ok &= mouse_write(0xF4);      /* включить поток данных */

    if (ok)
        kprintf("[mouse] PS/2 mouse online (100 Hz, IRQ12, колесо: %s)\n",
                g_wheel_capable ? "ДА" : "нет");
    else
        kprintf("[mouse] WARNING: mouse did not ACK all commands\n");
}

void mouse_set_limits(uint32_t max_x, uint32_t max_y) {
    g_max_x = (int32_t)max_x - 1;
    g_max_y = (int32_t)max_y - 1;
}

void mouse_irq_handler(void) {
    uint8_t b = inb(PS2_DATA);
    g_irq_count++;   /* диагностику выводит десктоп из главного контекста */
    switch (g_cycle) {
    case 0:
        if (!(b & 0x08)) return;          /* синхронизация пакета: bit3 всегда 1 */
        g_packet[0] = b; g_cycle = 1; break;
    case 1:
        g_packet[1] = b; g_cycle = 2; break;
    case 2:
        g_packet[2] = b;
        if (g_wheel_capable) { g_cycle = 3; break; }
        /* fallthrough: 3-байтный пакет готов */
        g_cycle = 0;
        goto packet_ready;
    case 3:
        g_packet[3] = b; g_cycle = 0;
        /* 4-й байт: колесо, знак в своём бит-7 (этот байт без флагов X/Y) */
        g_wheel += (int)(int8_t)b;
        goto packet_ready;
    }
    return;
packet_ready:;
    int32_t dx = g_packet[1];
    int32_t dy = g_packet[2];
    if (g_packet[0] & 0x10) dx |= ~0xFF;   /* знак X */
    if (g_packet[0] & 0x20) dy |= ~0xFF;   /* знак Y */
    if (g_packet[0] & 0xC0) return;        /* переполнение - пакет мусорный */
    g_x += dx;
    g_y -= dy;                             /* ось Y у мыши инвертирована */
    if (g_x < 0) g_x = 0;
    if (g_y < 0) g_y = 0;
    if (g_x > g_max_x) g_x = g_max_x;
    if (g_y > g_max_y) g_y = g_max_y;
    g_left = g_packet[0] & 1;
    if (dx || dy) g_moved = 1;
}

void mouse_nudge(int dx, int dy) {
    g_x += dx;
    g_y += dy;
    if (g_x < 0) g_x = 0;
    if (g_y < 0) g_y = 0;
    if (g_x > g_max_x) g_x = g_max_x;
    if (g_y > g_max_y) g_y = g_max_y;
    g_moved = 1;
}

int32_t mouse_x(void) { return g_x; }
int32_t mouse_y(void) { return g_y; }
int     mouse_left(void) { return g_left; }
uint32_t mouse_irq_count(void) { return g_irq_count; }
int     mouse_moved(void) {
    int m = g_moved;
    g_moved = 0;
    return m;
}

int mouse_wheel(void) {          /* забирает накопленное колесо */
    int w = g_wheel;
    g_wheel = 0;
    return w;
}
