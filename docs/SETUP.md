# AresOS — Установка инструментария (Windows 10/11 + WSL2)

Выполняется **один раз**. После этого рабочий цикл: `make` → `make run` → `make debug`.

> Экономия достигнута за счёт собственных инструментов проекта: образ диска делает
> `tools/mkesp.py` (не нужны mtools/xorriso), загрузчик конвертирует `tools/elf2efi.py`
> (не нужен gnu-efi), ассемблер — GAS (встроен в gcc, не нужен nasm).

---

## Шаг 0. Установить WSL2 (PowerShell от администратора)

```powershell
wsl --install
```

Перезагрузи ПК, в открывшемся терминале Ubuntu придумай логин/пароль.
Дальше **все команды — внутри Ubuntu (WSL)**.

> **Windows 11**: окно QEMU появится само (WSLg встроен).
> **Windows 10**: графики WSL нет — ставь X-сервер (VcXsrv) ИЛИ используй QEMU для
> Windows (раздел «Производительность» ниже). Serial-лог ядра виден и без графики.

---

## Шаг 1. Клонировать репозиторий

```bash
git clone https://github.com/Poncho435/AresOS.git
cd AresOS
git checkout arena/01a02ad5-aresos   # ветка с текущей работой
```

## Шаг 2. Пакеты (минимум)

```bash
sudo apt update
sudo apt install -y build-essential make python3 qemu-system-x86 ovmf
sudo apt install -y gdb        # для make debug / make gdb
sudo apt install -y x86_64-w64-mingw32-gcc   # ПОЗЖЕ (этап M8): собирать тестовые .exe
```

| Пакет | Зачем |
|---|---|
| `build-essential`, `make` | gcc + GNU as + GNU ld + make |
| `python3` | наши инструменты: `elf2efi.py`, `mkesp.py` |
| `qemu-system-x86` | эмулятор ПК |
| `ovmf` | прошивка UEFI для QEMU (файлы `/usr/share/OVMF/*.fd`) |
| `gdb` | отладка ядра через GDB-stub QEMU |
| `x86_64-w64-mingw32-gcc` | компилятор Windows PE `.exe` — понадобится на M8 |

## Шаг 3 (необязательный). Кросс-компилятор x86_64-elf-gcc

**Не обязателен:** наш код freestanding, Makefile сам использует host `gcc`.
Если хочешь «классический» OSDev-тулчейн (чистота + привычка) — собирается так:

```bash
sudo apt install -y bison flex libgmp3-dev libmpc-dev libmpfr-dev libisl-dev texinfo
export PREFIX="$HOME/opt/cross" TARGET=x86_64-elf
mkdir -p ~/src && cd ~/src
BINUTILS_VER=2.43; GCC_VER=14.2.0
curl -LO https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.xz
tar xf binutils-$BINUTILS_VER.tar.xz && mkdir build-binutils && cd build-binutils
../binutils-$BINUTILS_VER/configure --target=$TARGET --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j$(nproc) && make install && cd ~/src
curl -LO https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz
tar xf gcc-$GCC_VER.tar.xz && mkdir build-gcc && cd build-gcc
../gcc-$GCC_VER/configure --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers
make -j$(nproc) all-gcc && make -j$(nproc) all-target-libgcc
make install-gcc install-target-libgcc
echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> ~/.bashrc && source ~/.bashrc
```

Makefile сам подхватит `x86_64-elf-gcc`, если он появился в PATH.

---

## Шаг 4. Проверка, сборка, ЗАПУСК

```bash
cd AresOS
make tools-check   # все инструменты видны? (OVMF_CODE/OVMF_VARS — не пустые!)
make               # сборка: ядро → загрузчик → PE → FAT-образ
make verify        # статическая проверка бинарников
make run           # ЗАПУСК: окно QEMU + serial-лог в этом терминале
```

Ожидаемый serial-лог (`-serial stdio`):

```
[boot] AresOS UEFI loader alive
[boot] KERNEL.ELF read, size = 0x...
[boot] PT_LOAD dest=0x200000 ...
[boot] framebuffer base = 0x...
[boot] ExitBootServices OK — bye, firmware!
  ============================================
   A r e s O S   kernel 0.1.0-m2
  ============================================
[gdt] GDT loaded ...
[idt] IDT loaded ...
[fb] framebuffer console: 1024 x 768 ...
[mem] usable RAM total: ... MiB
[pmm] ...
AresOS init complete (M2). CPU halting; next milestone: M3 VMM + heap.
```

В окне QEMU — тёмно-синяя консоль с тем же текстом (framebuffer через GOP).

## Отладка

```bash
make debug     # терминал 1: QEMU стоит на старте, ждёт GDB (:1234)
make gdb       # терминал 2: подключение, breakpoint на kmain, layout split
```

`make debug` + `Ctrl-A X` — выход из QEMU.

## Производительность QEMU (по желанию)

В WSL2 нет KVM → эмуляция TCG (медленнее). Для скорости: QEMU для Windows
(https://www.qemu.org/download/#windows), запуск из PowerShell с `-accel whpx`,
образ брать через `\\wsl$\Ubuntu\home\<юзер>\AresOS\build\aresos.img`.

## Что НЕ нужно качать

- ❌ nasm, mtools, xorriso, gnu-efi — заменены нашими инструментами/сборкой
- ❌ Visual Studio / MSVC
- ❌ Bochs — QEMU+GDB покрывает всё на этом этапе
