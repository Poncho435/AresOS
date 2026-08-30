/* AresOS - VFS/ramfs (v0.7.0): файловая система в оперативной памяти.
 * Узлы - из статического пула (предсказуемо, без фрагментации кучи),
 * данные файлов - в heap с удвоением ёмкости и общим лимитом VFS_DATA_LIMIT
 * (чтобы опечатка в приложении не съела всю память). */
#include "vfs.h"
#include "heap.h"
#include "kprintf.h"
#include <string.h>

typedef struct {
    int      used, is_dir;
    int      parent, first_child, next_sib;
    char     name[VFS_NAME_MAX];
    char    *data;
    uint32_t size, cap;
} vnode_t;

static vnode_t  g_n[VFS_MAX_NODES];
static uint32_t g_used_bytes;

static int name_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void name_copy(char *dst, const char *src) {
    int i = 0;
    while (i < VFS_NAME_MAX - 1 && src[i]) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

int vfs_init(void) {
    memset(g_n, 0, sizeof(g_n));
    g_used_bytes = 0;
    vnode_t *r = &g_n[VFS_ROOT];
    r->used = 1; r->is_dir = 1;
    r->parent = r->first_child = r->next_sib = VFS_NONE;
    name_copy(r->name, "/");
    kprintf("[vfs] ramfs online: %d узлов, лимит данных %u КиБ\n",
            (uint64_t)VFS_MAX_NODES, (uint64_t)(VFS_DATA_LIMIT >> 10));
    return 0;
}

static int alloc_node(int dir, const char *name, int is_dir) {
    if (!g_n[dir].used || !g_n[dir].is_dir) return VFS_NONE;
    if (!name[0] || name_eq(name, ".") || name_eq(name, "..")) return VFS_NONE;
    if (vfs_find(dir, name) != VFS_NONE) return VFS_NONE;   /* имя занято */
    for (int i = 1; i < VFS_MAX_NODES; i++) {
        if (g_n[i].used) continue;
        vnode_t *v = &g_n[i];
        memset(v, 0, sizeof(*v));
        v->used = 1; v->is_dir = is_dir;
        v->parent = dir; v->first_child = v->next_sib = VFS_NONE;
        name_copy(v->name, name);
        /* в хвост списка детей (порядок создания сохраняется) */
        if (g_n[dir].first_child == VFS_NONE) g_n[dir].first_child = i;
        else {
            int c = g_n[dir].first_child;
            while (g_n[c].next_sib != VFS_NONE) c = g_n[c].next_sib;
            g_n[c].next_sib = i;
        }
        return i;
    }
    kprintf("[vfs] пул узлов исчерпан (%d)\n", (uint64_t)VFS_MAX_NODES);
    return VFS_NONE;
}

int vfs_mkdir(int dir, const char *name)  { return alloc_node(dir, name, 1); }
int vfs_create(int dir, const char *name) { return alloc_node(dir, name, 0); }

int vfs_parent(int idx)       { return g_n[idx].used ? g_n[idx].parent : VFS_NONE; }
int vfs_first_child(int dir)  { return (g_n[dir].used && g_n[dir].is_dir)
                                       ? g_n[dir].first_child : VFS_NONE; }
int vfs_next_sibling(int idx) { return g_n[idx].used ? g_n[idx].next_sib : VFS_NONE; }
const char *vfs_name(int idx) { return g_n[idx].used ? g_n[idx].name : "?"; }
int  vfs_is_dir(int idx)      { return g_n[idx].used ? g_n[idx].is_dir : 0; }
uint32_t vfs_size(int idx)    { return g_n[idx].used ? g_n[idx].size : 0; }

int vfs_find(int dir, const char *name) {
    for (int c = vfs_first_child(dir); c != VFS_NONE; c = g_n[c].next_sib)
        if (name_eq(g_n[c].name, name)) return c;
    return VFS_NONE;
}

int vfs_count(int dir) {
    int n = 0;
    for (int c = vfs_first_child(dir); c != VFS_NONE; c = g_n[c].next_sib) n++;
    return n;
}

static void unlink_node(int idx) {
    int p = g_n[idx].parent;
    if (p == VFS_NONE) return;
    if (g_n[p].first_child == idx) { g_n[p].first_child = g_n[idx].next_sib; return; }
    for (int c = g_n[p].first_child; c != VFS_NONE; c = g_n[c].next_sib)
        if (g_n[c].next_sib == idx) { g_n[c].next_sib = g_n[idx].next_sib; return; }
}

int vfs_delete(int idx) {
    if (idx <= 0 || idx >= VFS_MAX_NODES || !g_n[idx].used) return 0;
    if (g_n[idx].is_dir)
        while (g_n[idx].first_child != VFS_NONE)
            vfs_delete(g_n[idx].first_child);      /* сначала всех детей */
    if (g_n[idx].data) {
        g_used_bytes -= g_n[idx].cap;
        kfree(g_n[idx].data);
        g_n[idx].data = 0;
    }
    unlink_node(idx);
    g_n[idx].used = 0;
    return 1;
}

static int grow_to(vnode_t *v, uint32_t need) {
    if (need <= v->cap) return 1;
    uint32_t ncap = v->cap ? v->cap : 128;
    while (ncap < need) ncap *= 2;
    if (g_used_bytes - v->cap + ncap > VFS_DATA_LIMIT) {
        kprintf("[vfs] лимит данных исчерпан (%u КиБ)\n",
                (uint64_t)(VFS_DATA_LIMIT >> 10));
        return 0;
    }
    char *nd = (char *)kmalloc(ncap);
    if (!nd) return 0;
    if (v->data) { memcpy(nd, v->data, v->size); kfree(v->data); }
    g_used_bytes += ncap - v->cap;
    v->data = nd; v->cap = ncap;
    return 1;
}

int vfs_write(int idx, const char *data, uint32_t len) {
    if (idx <= 0 || idx >= VFS_MAX_NODES || !g_n[idx].used || g_n[idx].is_dir)
        return 0;
    vnode_t *v = &g_n[idx];
    if (!grow_to(v, len + 1)) return 0;
    if (len) memcpy(v->data, data, len);
    v->data[len] = 0;
    v->size = len;
    return 1;
}

int vfs_append(int idx, const char *data, uint32_t len) {
    if (idx <= 0 || idx >= VFS_MAX_NODES || !g_n[idx].used || g_n[idx].is_dir)
        return 0;
    vnode_t *v = &g_n[idx];
    uint32_t base = v->size;
    if (!grow_to(v, base + len + 1)) return 0;
    if (len) memcpy(v->data + base, data, len);
    v->data[base + len] = 0;
    v->size = base + len;
    return 1;
}

const char *vfs_read(int idx, uint32_t *out_len) {
    if (idx <= 0 || idx >= VFS_MAX_NODES || !g_n[idx].used || g_n[idx].is_dir) {
        if (out_len) *out_len = 0;
        return "";
    }
    if (out_len) *out_len = g_n[idx].size;
    return g_n[idx].data ? g_n[idx].data : "";
}

uint32_t vfs_used_bytes(void) { return g_used_bytes; }
