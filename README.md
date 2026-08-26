# AresOS

64-битная операционная система «голого железа» (x86-64): собственное ядро на
C + NASM, собственный UEFI-загрузчик — и конечная цель: **запускать Windows
PE-приложения (.exe)** через собственный слой совместимости WinAPI.

> A 64-bit bare-metal OS in C + assembly with a custom UEFI bootloader,
> aiming to run Windows PE executables via its own WinAPI compatibility layer.

## Статус

- ✅ **M0** — инфраструктура и инструменты
- ✅ **M1** — UEFI-загрузчик (BOOTX64.EFI): ELF-парсер, GOP-графика, ExitBootServices
- ✅ **M2** — ядро: serial, kprintf, framebuffer-консоль, GDT, IDT (32 исключения)
- ✅ **M2.5 (бонус)** — **прототип рабочего стола**: обои, панель, док, окно с перетаскиванием,
  мышь PS/2 через PIC (IRQ12)
- 🚧 **M3 (частично)** — PMM на bitmap; дальше: VMM/paging, higher-half ядро, kmalloc

Полный план: [docs/ROADMAP.md](docs/ROADMAP.md)

## Сборка и запуск

```bash
make tools-check   # проверка окружения
make               # собрать build/aresos.img (GPT-диск с ESP/FAT32)
make run           # QEMU + OVMF (UEFI) — окно эмулятора + serial-лог ядра
make debug         # + GDB-stub на :1234, далее `make gdb`
```

Требования: `gcc`, `make`, `python3`, `qemu-system-x86`, `ovmf` (подробности — [docs/SETUP.md](docs/SETUP.md)).

## Запуск (v0.5.2 — запуск значками, кириллица, фиксы стабильности)

**VirtualBox — самый простой путь:**
**[dist/aresos-vm-v0.5.2.zip](https://github.com/Poncho435/AresOS/raw/arena/01a02ad5-aresos/dist/aresos-vm-v0.5.2.zip)**
→ распаковать → ВМ (Other 64-bit, 256 МБ, ✔EFI) → в «Носителях» к SATA прикрепить файл
`aresos.vmdk` → Запустить. Никакой командной строки!

**Реальное железо (флешка):**
**[dist/aresos-usb-v0.5.2.zip](https://github.com/Poncho435/AresOS/raw/arena/01a02ad5-aresos/dist/aresos-usb-v0.5.2.zip)**
→ `aresos.img` на флешку (balenaEtcher / Rufus DD) → Boot Menu → «UEFI: флешка».
Пошагово: [docs/REALHARDWARE.md](docs/REALHARDWARE.md).

**ISO для CD-привода (виртуалки):**
**[dist/aresos-iso-v0.5.2.zip](https://github.com/Poncho435/AresOS/raw/arena/01a02ad5-aresos/dist/aresos-iso-v0.5.2.zip)**
→ новая ВМ без жёсткого диска → прикрепить `aresos.iso` к CD-приводу → Запустить.

> **Управление на рабочем столе:** мышь (кликни в окно ВМ для захвата!),
> **приложения запускаются значками** — в панели сверху (About/Tasks/Logs)
> и в доке внизу; **закрытие — красная кнопка ×** в углу окна, Esc или
> повторная клавиша F1/F2/F3; ↑/↓/PgUp/PgDn — прокрутка логов;
> **Ctrl+стрелки** — движение курсора с клавиатуры (если мышь в ВМ застряла).
> Весь интерфейс и логи ядра — **по-русски** (свой кириллический шрифт).
>
> v0.5.2 — приложения запускаются **значками** (кнопки в панели + док),
> окна закрываются **красной ×**, и **полный русский интерфейс** — свой
> кириллический шрифт 8x8 (UTF-8 декодер в консоли и UI).
> Интерфейс «midnight aurora»: плавающий док, окна со z-порядком и
> перетаскиванием, скругления, часы в панели, диспетчер задач с барами
> загрузки CPU, **все логи ядра в отдельном приложении «Логи»** (цветной
> терминал, прокрутка), курсор двухцветный, клавиатурная мышь Ctrl+стрелки.
>
> Внутри (v0.4.0): этапы **M4 + M5** закрыты — ядро находит ACPI/MADT через EFI,
> маршрутизирует клавиатуру и мышь через IOAPIC (с автоматическим откатом
> на legacy PIC/PIT), системный тик 100 Гц от LAPIC-таймера (калибровка по
> PIT), клавиатура PS/2 (scancode→ASCII, F1..F10, стрелки, Ctrl, PgUp/PgDn),
> планировщик round-robin с вытеснением тиком (ядровые потоки на отдельных
> стеках), фоновые демоны heartbeat/sysmon.
> Плюс всё из v0.3.x: свои таблицы страниц + heap (стресс 1M операций),
> запуск настоящего PE32+ TESTPE.EXE, живая мышь (фикс CS после lgdt).

## Документация

- [docs/SETUP.md](docs/SETUP.md) — что скачать и установить (Windows + WSL2)
- [docs/ROADMAP.md](docs/ROADMAP.md) — дорожная карта по этапам M0→M8
- [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) — архитектура ядра и карта памяти

## Стек

| Компонент | Технология |
|---|---|
| Ядро | C (freestanding) + NASM |
| Тулчейн | кросс-компилятор `x86_64-elf-gcc`, GNU ld |
| Загрузка | собственный UEFI-загрузчик (BOOTX64.EFI) |
| Эмуляция/отладка | QEMU + OVMF, GDB |
| Тестовые .exe | x86_64-w64-mingw32-gcc |

## Лицензия

Apache License 2.0 — см. [LICENSE](LICENSE).
