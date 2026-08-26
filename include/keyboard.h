/* AresOS - клавиатура PS/2 (M4): IRQ1, scancode set 1 -> ASCII, F1..F12. */
#ifndef ARES_KEYBOARD_H
#define ARES_KEYBOARD_H

void keyboard_init(void);
void keyboard_irq_handler(void);   /* читает байт из 0x60 */
int  keyboard_getch(void);         /* -1 = буфер пуст; ASCII или KEY_* */

#define KEY_F1  0x201
#define KEY_F2  0x202
#define KEY_F3  0x203
#define KEY_F4  0x204
#define KEY_F5  0x205
#define KEY_F6  0x206
#define KEY_F7  0x207
#define KEY_F8  0x208
#define KEY_F9  0x209
#define KEY_F10 0x20A
#define KEY_UP    0x220
#define KEY_DOWN  0x221
#define KEY_LEFT  0x222
#define KEY_RIGHT 0x223
#define KEY_PGUP  0x224
#define KEY_PGDN  0x225
/* v0.5.0: Ctrl+стрелки = ручное движение курсора (спасательный круг для VM) */
#define KEY_MLEFT  0x230
#define KEY_MRIGHT 0x231
#define KEY_MUP    0x232
#define KEY_MDOWN  0x233

#endif
