/* AresOS — точка входа ядра.
 * Загрузчик передаёт указатель на bootinfo_t в RDI и прыгает сюда (SysV ABI).
 * До входа сюда не было ничего нашего: ни стека, ни обнулённого BSS. */
.intel_syntax noprefix

.section .text
.global _start
.extern kmain

_start:
    cli                             # прерывания выключим до установки IDT
    cld
    mov r12, rdi                    # bootinfo* — спасаем (rdi скоро понадобится stosq)

    lea rsp, [rip + _stack_top]     # свой стек ядра
    xor ebp, ebp

    /* обнуление BSS (стандарт ELF: memsz > filesz) */
    xor eax, eax
    lea rdi, [rip + __bss_start]
    lea rcx, [rip + __bss_end]
    sub rcx, rdi
    shr rcx, 3                      # BSS выровнен по 16 байтам в linker.ld
    rep stosq

    mov rdi, r12                    # первый аргумент kmain — bootinfo*
    and rsp, -16                    # SysV: rsp ≡ 0 (mod 16) в точке call
    call kmain

.Lhalt:                             # kmain не возвращается, но на всякий случай
    cli
    hlt
    jmp .Lhalt

.section .bss
.align 16
_stack_bottom:
    .skip 65536                     # 64 КиБ стека ядра
_stack_top:

.section .note.GNU-stack,"",@progbits
