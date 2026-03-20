/* ChaosGL Texture Subsystem — loads .rawt textures from ChaosFS into PMM pages */

#include "texture.h"
#include "../kernel/pmm.h"
#include "../kernel/vmm.h"
#include "../kernel/heap.h"
#include "../kernel/chaos/chaos.h"
#include "../drivers/serial.h"
#include "../include/string.h"

static chaos_gl_texture_t* textures;

void chaos_gl_texture_init(void) {
    textures = (chaos_gl_texture_t*)kzmalloc(CHAOS_GL_MAX_TEXTURES * sizeof(chaos_gl_texture_t));
}

int chaos_gl_texture_load(const char* path) {
    /* Find a free slot */
    int handle = -1;
    for (int i = 0; i < CHAOS_GL_MAX_TEXTURES; i++) {
        if (!textures[i].in_use) {
            handle = i;
            break;
        }
    }
    if (handle < 0) {
        serial_printf("[texture] no free texture slots\n");
        return -1;
    }

    /* Open the file from ChaosFS */
    int fd = chaos_open(path, CHAOS_O_RDONLY);
    if (fd < 0) {
        serial_printf("[texture] failed to open '%s'\n", path);
        return -1;
    }

    /* Read the raw_tex_header (16 bytes) */
    struct raw_tex_header hdr;
    int bytes_read = chaos_read(fd, &hdr, sizeof(hdr));
    if (bytes_read != (int)sizeof(hdr)) {
        serial_printf("[texture] failed to read header from '%s'\n", path);
        chaos_close(fd);
        return -1;
    }

    /* Validate header */
    if (hdr.magic != RAW_TEX_MAGIC) {
        serial_printf("[texture] bad magic 0x%x in '%s'\n", hdr.magic, path);
        chaos_close(fd);
        return -1;
    }
    if (hdr.width == 0 || hdr.height == 0 ||
        hdr.width > CHAOS_GL_MAX_TEX_SIZE || hdr.height > CHAOS_GL_MAX_TEX_SIZE) {
        serial_printf("[texture] invalid dimensions %ux%u in '%s'\n",
                      hdr.width, hdr.height, path);
        chaos_close(fd);
        return -1;
    }

    /* Calculate pixel data size and pages needed */
    uint32_t w = hdr.width;
    uint32_t h = hdr.height;
    uint32_t size = w * h * 4;
    uint32_t pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;

    /* Allocate physical pages */
    uint32_t phys = pmm_alloc_pages(pages);
    if (phys == 0) {
        serial_printf("[texture] pmm_alloc_pages(%u) failed for '%s'\n", pages, path);
        chaos_close(fd);
        return -1;
    }

    /* Identity-map the pages and mark reserved in heap */
    vmm_map_range(phys, phys, pages * PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE);
    heap_mark_reserved(phys, pages);

    /* Read pixel data in a loop (ChaosFS may return partial reads) */
    uint8_t* buf = (uint8_t*)phys;
    uint32_t total_read = 0;
    while (total_read < size) {
        int r = chaos_read(fd, buf + total_read, size - total_read);
        if (r <= 0) {
            serial_printf("[texture] read error at offset %u/%u for '%s'\n",
                          total_read, size, path);
            chaos_close(fd);
            pmm_free_pages(phys, pages);
            return -1;
        }
        total_read += (uint32_t)r;
    }

    chaos_close(fd);

    /* Fill the texture struct */
    textures[handle].data      = (uint32_t*)phys;
    textures[handle].width     = (int)w;
    textures[handle].height    = (int)h;
    textures[handle].pitch     = (int)w;
    textures[handle].in_use    = true;
    textures[handle].phys_addr = phys;
    textures[handle].pages     = pages;

    serial_printf("[texture] loaded '%s' as handle %d (%ux%u, %u pages)\n",
                  path, handle, w, h, pages);
    return handle;
}

void chaos_gl_texture_free(int handle) {
    if (handle < 0 || handle >= CHAOS_GL_MAX_TEXTURES) return;
    if (!textures[handle].in_use) return;

    pmm_free_pages(textures[handle].phys_addr, textures[handle].pages);
    textures[handle].in_use = false;
    textures[handle].data   = (void*)0;
}

const chaos_gl_texture_t* chaos_gl_texture_get(int handle) {
    if (handle < 0 || handle >= CHAOS_GL_MAX_TEXTURES) return (void*)0;
    if (!textures[handle].in_use) return (void*)0;
    return &textures[handle];
}

void chaos_gl_texture_get_size(int handle, int* w, int* h) {
    if (handle < 0 || handle >= CHAOS_GL_MAX_TEXTURES ||
        !textures[handle].in_use) {
        if (w) *w = 0;
        if (h) *h = 0;
        return;
    }
    if (w) *w = textures[handle].width;
    if (h) *h = textures[handle].height;
}
