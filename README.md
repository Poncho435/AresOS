# AresOS

64-битная операционная система «голого железа» (x86-64): собственное ядро на
C + NASM, собственный UEFI-загрузчик — и конечная цель: **запускать Windows
PE-приложения (.exe)** через собственный слой совместимости WinAPI.

> A 64-bit bare-metal OS in C + assembly with a custom UEFI bootloader,
> aiming to run Windows PE executables via its own WinAPI compatibility layer.

## Статус

📍 Этап **M0 — инфраструктура**. См. [docs/ROADMAP.md](docs/ROADMAP.md).

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
