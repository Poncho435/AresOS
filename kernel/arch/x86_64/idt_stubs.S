/* AresOS — стабы обработчиков исключений CPU (векторы 0..31).
 * Единый формат кадра: [GPR×15][vector][error][rip,cs,rflags,rsp,ss]
 * — его же видит структура regs_t в exceptions.c. */
.intel_syntax noprefix
.extern exception_handler

.macro ISR_NOERR n
.global isr\n
isr\n:
    push 0              # фиктивный error code — кадр единого размера
    push \n
    jmp isr_common
.endm

.macro ISR_ERR n
.global isr\n
isr\n:
    push \n             # error code CPU положил сам
    jmp isr_common
.endm

isr_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp        # arg0 = regs_t*
    mov rbx, rsp        # сохранить указатель (старое rbx — на стеке)
    and rsp, -16        # выравнивание под SysV перед call
    cld
    call exception_handler
    mov rsp, rbx

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16         # снять vector + error
    iretq

/* исключения БЕЗ error code */
ISR_NOERR 0     # DE — деление на ноль
ISR_NOERR 1     # DB — debug
ISR_NOERR 2     # NMI
ISR_NOERR 3     # BP — breakpoint
ISR_NOERR 4     # OF — overflow
ISR_NOERR 5     # BR — bound range
ISR_NOERR 6     # UD — недопустимая инструкция
ISR_NOERR 7     # NM — FPU недоступен
ISR_ERR   8     # DF — double fault
ISR_NOERR 9     # — (legacy)
ISR_ERR   10    # TS
ISR_ERR   11    # NP
ISR_ERR   12    # SS
ISR_ERR   13    # GP — general protection
ISR_ERR   14    # PF — page fault
ISR_NOERR 15    # — reserved
ISR_NOERR 16    # MF — x87 FP
ISR_ERR   17    # AC — alignment check
ISR_NOERR 18    # MC — machine check
ISR_NOERR 19    # XM — SIMD FP
ISR_NOERR 20    # VE — virtualization
ISR_ERR   21    # CP — control protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29    # VC
ISR_ERR   30    # SX
ISR_NOERR 31

/* ================= аппаратные прерывания (вектора 32..47) ================= */
.extern irq_dispatch

irq_common:
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    mov rdi, rsp
    mov rbx, rsp
    and rsp, -16
    cld
    call irq_dispatch
    mov rsp, rbx

    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    iretq

.macro IRQSTUB n
.global irq\n
irq\n:
    push 0
    push \n
    jmp irq_common
.endm

IRQSTUB 32    # IRQ0  PIT (пока замаскирован)
IRQSTUB 33    # IRQ1  клавиатура (M4)
IRQSTUB 34    # IRQ2  cascade
IRQSTUB 35    # IRQ3  COM2
IRQSTUB 36    # IRQ4  COM1
IRQSTUB 37    # IRQ5
IRQSTUB 38    # IRQ6  FDC
IRQSTUB 39    # IRQ7  LPT
IRQSTUB 40    # IRQ8  RTC
IRQSTUB 41    # IRQ9
IRQSTUB 42    # IRQ10
IRQSTUB 43    # IRQ11
IRQSTUB 44    # IRQ12 мышь PS/2
IRQSTUB 45    # IRQ13 FPU
IRQSTUB 46    # IRQ14 ATA primary
IRQSTUB 47    # IRQ15 ATA secondary

.section .note.GNU-stack,"",@progbits
