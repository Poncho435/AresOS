/* AresOS - VFS/ramfs (v0.7.0): настоящая файловая система в памяти ядра!
 * Папки и файлы создаются, файлы пишутся и читаются (Проводник + Просмотр).
 * Пока всё живёт в RAM (heap) и честно умирает при выключении; драйвер
 * диска (M6) добавит постоянное хранилище - интерфейс VFS уже тут. */
#ifndef ARES_VFS_H
#define ARES_VFS_H

#include <stdint.h>

#define VFS_MAX_NODES  96     /* всего объектов (папки+файлы) */
#define VFS_NAME_MAX   24     /* байт на имя (UTF-8; кириллица = 2 байта) */
#define VFS_DATA_LIMIT (768 * 1024)   /* общий потолок данных всех файлов */

int  vfs_init(void);          /* создать корень "/" - индекс 0 */
#define VFS_ROOT 0
#define VFS_NONE (-1)

/* навигация */
int         vfs_parent(int idx);
int         vfs_first_child(int dir);
int         vfs_next_sibling(int idx);
int         vfs_find(int dir, const char *name);
const char *vfs_name(int idx);
int         vfs_is_dir(int idx);
uint32_t    vfs_size(int idx);          /* байт данных файла */
int         vfs_count(int dir);         /* сколько детей у папки */

/* создание/удаление */
int  vfs_mkdir(int dir, const char *name);        /* индекс или -1 */
int  vfs_create(int dir, const char *name);       /* файл 0 байт или -1 */
int  vfs_delete(int idx);                         /* рекурсивно для папок */

/* данные файла */
const char *vfs_read(int idx, uint32_t *out_len);   /* указатель на данные */
int  vfs_write(int idx, const char *data, uint32_t len);  /* перезаписать */
int  vfs_append(int idx, const char *data, uint32_t len); /* дописать */

uint32_t vfs_used_bytes(void);   /* сколько памяти занято данными файлов */

#endif
