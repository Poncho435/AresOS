# AresOS — Установка инструментария (Windows 10/11 + WSL2)

Это пошаговая инструкция: что скачать и какие команды выполнить, чтобы собрать
окружение для разработки ОС. Выполняется **один раз**.

---

## Шаг 0. Установить WSL2 (PowerShell от администратора)

```powershell
wsl --install
```

Перезагрузи ПК. После перезагрузки откроется Ubuntu — придумай логин/пароль.
Проверка, что у тебя именно WSL**2**:

```powershell
wsl -l -v
```

> На **Windows 11** графические окна Linux (включая окно QEMU) работают «из коробки»
> через WSLg. На **Windows 10** — либо ставь X-сервер (VcXsrv), либо запускай
> QEMU для Windows (см. раздел «Производительность» внизу).

Дальше **все команды выполняются внутри терминала Ubuntu (WSL)**.

---

## Шаг 1. Базовые пакеты

```bash
sudo apt update && sudo apt upgrade -y

sudo apt install -y \
  build-essential bison flex \
  libgmp3-dev libmpc-dev libmpfr-dev libisl-dev texinfo \
  nasm make git curl \
  qemu-system-x86 ovmf mtools xorriso \
  gdb gdb-multiarch \
  x86_64-w64-mingw32-gcc
```

Что зачем:

| Пакет | Зачем |
|---|---|
| `build-essential`, `make`, `git` | базовая сборка |
| `bison`, `flex`, `libgmp/mpfr/mpc/isl-dev`, `texinfo` | зависимости для сборки кросс-GCC |
| `nasm` | ассемблер ( входная точка ядра, context switch ) |
| `qemu-system-x86` | эмулятор ПК — там будет крутиться AresOS |
| `ovmf` | прошивка UEFI для QEMU (наш «BIOS») |
| `mtools`, `xorriso` | создание FAT32-образа диска и ISO без root-прав |
| `gdb-multiarch` | отладчик для подключения к QEMU |
| `x86_64-w64-mingw32-gcc` | компилятор **Windows PE .exe** — им мы будем собирать тестовые программы, которые AresOS будет запускать |

---

## Шаг 2. Кросс-компилятор x86_64-elf (выбери ОДИН вариант)

### Вариант A (рекомендуемый): собрать кросс-GCC самому

Обычный gcc из Ubuntu «зашивает» зависимости от Linux. Кросс-компилятор
`x86_64-elf-gcc` собирает чистый freestanding-код без ОС. Сборка занимает
20–60 минут, делается один раз.

```bash
export PREFIX="$HOME/opt/cross"
export TARGET=x86_64-elf
export PATH="$PREFIX/bin:$PATH"
mkdir -p ~/src && cd ~/src

# Возьми последние стабильные версии с ftp.gnu.org, если этих уже нет:
BINUTILS_VER=2.43
GCC_VER=14.2.0

# --- Binutils (ассемблер+линкер) ---
curl -LO https://ftp.gnu.org/gnu/binutils/binutils-$BINUTILS_VER.tar.xz
tar xf binutils-$BINUTILS_VER.tar.xz
mkdir build-binutils && cd build-binutils
../binutils-$BINUTILS_VER/configure --target=$TARGET --prefix="$PREFIX" \
    --with-sysroot --disable-nls --disable-werror
make -j$(nproc)
make install
cd ~/src

# --- GCC ---
curl -LO https://ftp.gnu.org/gnu/gcc/gcc-$GCC_VER/gcc-$GCC_VER.tar.xz
tar xf gcc-$GCC_VER.tar.xz
mkdir build-gcc && cd build-gcc
../gcc-$GCC_VER/configure --target=$TARGET --prefix="$PREFIX" \
    --disable-nls --enable-languages=c --without-headers
make -j$(nproc) all-gcc
make -j$(nproc) all-target-libgcc
make install-gcc install-target-libgcc
```

Добавь в `~/.bashrc`, чтобы не вводить каждый раз:

```bash
echo 'export PATH="$HOME/opt/cross/bin:$PATH"' >> ~/.bashrc
source ~/.bashrc
```

### Вариант B (быстрый): Clang — он изначально кросс-компилятор

```bash
sudo apt install -y clang lld llvm
```

Clang умеет `--target=x86_64-unknown-none` без отдельной сборки. Если выберешь
вариант B — скажи мне, флаги в Makefile будут чуть другими.

---

## Шаг 3. Проверка окружения

```bash
x86_64-elf-gcc --version     # кросс-компилятор
x86_64-elf-ld --version      # кросс-линкер
nasm -v                      # ассемблер
qemu-system-x86_64 --version # эмулятор
ls /usr/share/OVMF/          # здесь лежат OVMF_CODE.fd / OVMF_VARS.fd (прошивка UEFI)
x86_64-w64-mingw32-gcc --version  # сборщик тестовых .exe
```

Если все 6 команд что-то вывели без ошибок — **M0 выполнен**.

---

## Производительность QEMU (по желанию, позже)

Внутри WSL2 нет KVM-ускорения, QEMU работает на программной эмуляции (TCG) —
для отладки ОС этого хватает, но медленно. Варианты ускорения:

1. **QEMU для Windows** (рекомендуется позже): скачать установщик с
   https://www.qemu.org/download/#windows, запускать из PowerShell с `-accel whpx`.
   Образ диска берётся из WSL через путь `\\wsl$\Ubuntu\home\<юзер>\...`.
2. Остаться на TCG, но добавлять флаг `-accel tcg,thread=multi`.

## Отладка через GDB (понадобится с M2)

```bash
# Один терминал — QEMU висит и ждёт отладчик:
qemu-system-x86_64 ... -s -S
# Второй терминал:
gdb -ex 'target remote localhost:1234' -ex 'symbol-file build/kernel.elf'
```

Позже всё это будет завёрнуто в удобные цели Makefile: `make run` и `make debug`.

---

## Что НЕ нужно качать

- ❌ Visual Studio / MSVC — нам не нужна Windows-разработка, только MinGW как «генератор exe».
- ❌ Bochs — полезен, но QEMU+GDB покрывает всё; вернёмся к нему при странных багах CPU.
- ❌ WSL1 — только версия 2.
