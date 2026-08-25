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

    /* v0.2.5: канарейка под дном стека (стек растёт ВНИЗ к _stack_guard).
       Если exception_handler обнаружит канарейки испорченными — стек
       переполнен и ушёл в BSS (типичный источник «ret в стек»). */
    mov rax, 0xA9E5C0FFEE15DA7A
    lea rdi, [rip + _stack_guard]
    mov ecx, 8
.Lcanary:
    mov [rdi], rax
    add rdi, 8
    dec ecx
    jnz .Lcanary

    mov rdi, r12                    # первый аргумент kmain — bootinfo*
    and rsp, -16                    # SysV: rsp ≡ 0 (mod 16) в точке call
    call kmain

.Lhalt:                             # kmain не возвращается, но на всякий случай
    cli
    hlt
    jmp .Lhalt

.section .bss
.align 16
.global _stack_guard, _stack_bottom, _stack_top
_stack_guard:                       # 64 байта магии под дном стека (v0.2.5)
    .skip 64
_stack_bottom:
    .skip 262144                    # 256 КиБ стека ядра (было 64 — поднято в v0.2.5)
_stack_top:

.section .note.GNU-stack,"",@progbits
