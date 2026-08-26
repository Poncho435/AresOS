/* AresOS - logbuf: кольцо последних LOG_LINES строк журнала.
 * Пишется из emit() kprintf'а: работает и из IRQ-контекста (это просто запись
 * в память), без локов - однопроцессорная модель, как и весь журнал.
 *
 * Инвариант: g_head - слот "пишущейся" строки; когда буфер полон, этот же
 * слот - физически самый старый, поэтому закрытых видно LOG_LINES-1,
 * а "свежая" незакрытая занимает последнюю видимую позицию. */
#include "logbuf.h"

static char g_lines[LOG_LINES][LOG_COLS];
static int  g_head;    /* индекс ТЕКУЩЕЙ (незакрытой) строки */
static int  g_count;   /* сколько строк финализировано "\n" (0..LOG_LINES) */
static int  g_col;     /* позиция в текущей строке */

static void line_break(void) {
    g_lines[g_head][g_col] = 0;
    g_head = (g_head + 1) % LOG_LINES;
    if (g_count < LOG_LINES) g_count++;
    g_col = 0;
}

void logbuf_putc(char c) {
    if (c == '\r') return;
    if (c == '\n') { line_break(); return; }
    g_lines[g_head][g_col++] = c;
    if (g_col >= LOG_COLS - 1) line_break();   /* слишком длинная - рвём */
}

int logbuf_count(void) {
    if (g_count < LOG_LINES)
        return g_col > 0 ? g_count + 1 : g_count;
    return LOG_LINES;   /* LOG_LINES-1 закрытых + пишущаяся */
}

const char *logbuf_line(int i) {
    int n = logbuf_count();
    if (i < 0 || i >= n) return "";

    if (g_count == LOG_LINES) {
        if (g_col == 0) {
            /* head - валидная ЗАКРЫТАЯ строка (и самая старая из них) */
            return g_lines[(g_head + i) % LOG_LINES];
        }
        /* head сейчас перезаписывается: 191 новейших закрытых + partial */
        if (i == n - 1) {
            g_lines[g_head][g_col] = 0;
            return g_lines[g_head];
        }
        return g_lines[(g_head + 1 + i) % LOG_LINES];
    }
    if (g_col > 0 && i == n - 1) {
        g_lines[g_head][g_col] = 0;           /* частичная строка - "как есть" */
        return g_lines[g_head];
    }
    int idx = (g_head - g_count + i + LOG_LINES * 2) % LOG_LINES;
    return g_lines[idx];
}
