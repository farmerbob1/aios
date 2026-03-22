/* AIOS — CPK (Chaos Package) Archive Reader
 * Opens .cpk archives from ChaosFS, reads TOC, extracts files. */

#include "cpk.h"
#include "lz4.h"
#include "../chaos/chaos.h"
#include "../heap.h"
#include "../../include/string.h"
#include "../../drivers/serial.h"
#include "../../include/kaos/export.h"

struct cpk_handle {
    bool in_use;
    int fd;
    struct cpk_header header;
    struct cpk_entry *toc;
};

static struct cpk_handle handles[CPK_MAX_HANDLES];

static bool valid_handle(int h) {
    return h >= 0 && h < CPK_MAX_HANDLES && handles[h].in_use;
}

int cpk_open(const char *path) {
    /* Find free slot */
    int slot = -1;
    for (int i = 0; i < CPK_MAX_HANDLES; i++) {
        if (!handles[i].in_use) { slot = i; break; }
    }
    if (slot < 0) return -1;

    int fd = chaos_open(path, CHAOS_O_RDONLY);
    if (fd < 0) return -1;

    /* Read header */
    struct cpk_header hdr;
    int n = chaos_read(fd, &hdr, sizeof(hdr));
    if (n != sizeof(hdr) || hdr.magic != CPK_MAGIC || hdr.version != CPK_VERSION) {
        chaos_close(fd);
        return -1;
    }

    if (hdr.file_count == 0) {
        /* Empty archive — valid but no TOC */
        handles[slot].in_use = true;
        handles[slot].fd = fd;
        handles[slot].header = hdr;
        handles[slot].toc = NULL;
        return slot;
    }

    /* Read TOC */
    uint32_t toc_size = hdr.file_count * sizeof(struct cpk_entry);
    struct cpk_entry *toc = kmalloc(toc_size);
    if (!toc) {
        chaos_close(fd);
        return -1;
    }

    chaos_seek(fd, hdr.toc_offset, 0 /* SEEK_SET */);
    n = chaos_read(fd, toc, toc_size);
    if ((uint32_t)n != toc_size) {
        kfree(toc);
        chaos_close(fd);
        return -1;
    }

    handles[slot].in_use = true;
    handles[slot].fd = fd;
    handles[slot].header = hdr;
    handles[slot].toc = toc;
    return slot;
}

int cpk_file_count(int handle) {
    if (!valid_handle(handle)) return 0;
    return (int)handles[handle].header.file_count;
}

int cpk_get_entry(int handle, int index, struct cpk_entry *out) {
    if (!valid_handle(handle)) return -1;
    if (index < 0 || (uint32_t)index >= handles[handle].header.file_count) return -1;
    if (!out) return -1;
    *out = handles[handle].toc[index];
    return 0;
}

int cpk_find(int handle, const char *path) {
    if (!valid_handle(handle) || !path) return -1;
    uint32_t count = handles[handle].header.file_count;
    struct cpk_entry *toc = handles[handle].toc;
    for (uint32_t i = 0; i < count; i++) {
        if (strcmp(toc[i].path, path) == 0)
            return (int)i;
    }
    return -1;
}

int cpk_extract(int handle, int index, void *buf, int buf_size) {
    if (!valid_handle(handle)) return -1;
    if (index < 0 || (uint32_t)index >= handles[handle].header.file_count) return -1;

    struct cpk_entry *e = &handles[handle].toc[index];
    int fd = handles[handle].fd;
    bool lz4 = (handles[handle].header.flags & CPK_FLAG_LZ4) != 0;

    if ((uint32_t)buf_size < e->size_original) return -1;

    /* Seek to file data */
    chaos_seek(fd, e->offset, 0 /* SEEK_SET */);

    if (!lz4 || e->size_compressed == e->size_original) {
        /* Uncompressed — read directly into output buffer */
        int n = chaos_read(fd, buf, e->size_original);
        if ((uint32_t)n != e->size_original) return -1;
    } else {
        /* LZ4 compressed — read into temp buffer, then decompress */
        void *tmp = kmalloc(e->size_compressed);
        if (!tmp) return -1;

        int n = chaos_read(fd, tmp, e->size_compressed);
        if ((uint32_t)n != e->size_compressed) {
            kfree(tmp);
            return -1;
        }

        int dec = chaos_lz4_decompress(tmp, e->size_compressed, buf, buf_size);
        kfree(tmp);
        if (dec < 0 || (uint32_t)dec != e->size_original) return -1;
    }

    /* Verify CRC-32 */
    uint32_t crc = chaos_crc32(buf, e->size_original);
    if (crc != e->checksum) {
        serial_printf("cpk: CRC mismatch for '%s' (got %x, expected %x)\n",
                      e->path, crc, e->checksum);
        return -1;
    }

    return (int)e->size_original;
}

void cpk_close(int handle) {
    if (!valid_handle(handle)) return;
    if (handles[handle].toc) {
        kfree(handles[handle].toc);
    }
    chaos_close(handles[handle].fd);
    handles[handle].in_use = false;
    handles[handle].toc = NULL;
}

KAOS_EXPORT(cpk_open)
KAOS_EXPORT(cpk_file_count)
KAOS_EXPORT(cpk_get_entry)
KAOS_EXPORT(cpk_find)
KAOS_EXPORT(cpk_extract)
KAOS_EXPORT(cpk_close)
