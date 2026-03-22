/* ChaosGL Texture Subsystem — loads textures from ChaosFS into PMM pages
 * Supports: RAWT (.raw), PNG, JPEG, BMP, GIF via stb_image */

#include "texture.h"
#include "stb_image_decode.h"
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

    /* Open the file and get its size */
    int fd = chaos_open(path, CHAOS_O_RDONLY);
    if (fd < 0) {
        serial_printf("[texture] failed to open '%s'\n", path);
        return -1;
    }

    struct chaos_stat st;
    if (chaos_stat(path, &st) < 0 || st.size == 0) {
        serial_printf("[texture] failed to stat '%s'\n", path);
        chaos_close(fd);
        return -1;
    }

    /* Read entire file into temporary buffer */
    uint32_t file_size = st.size;
    uint8_t *file_buf = (uint8_t *)kmalloc(file_size);
    if (!file_buf) {
        serial_printf("[texture] kmalloc(%u) failed for '%s'\n", file_size, path);
        chaos_close(fd);
        return -1;
    }

    uint32_t total_read = 0;
    while (total_read < file_size) {
        int r = chaos_read(fd, file_buf + total_read, file_size - total_read);
        if (r <= 0) break;
        total_read += (uint32_t)r;
    }
    chaos_close(fd);

    if (total_read < 16) {
        serial_printf("[texture] file too small (%u bytes) '%s'\n", total_read, path);
        kfree(file_buf);
        return -1;
    }

    /* Detect format by magic bytes */
    int fmt = stbi_detect_format(file_buf, (int)total_read);

    uint32_t w = 0, h = 0;
    bool has_alpha = false;
    uint8_t *pixel_data = NULL;  /* Decoded pixel data (BGRA/BGRX), needs kfree */

    if (fmt == STBI_FMT_RAWT) {
        /* RAWT: parse 16-byte header + raw BGRX pixels */
        struct raw_tex_header *hdr = (struct raw_tex_header *)file_buf;
        if (hdr->width == 0 || hdr->height == 0 ||
            hdr->width > CHAOS_GL_MAX_TEX_SIZE || hdr->height > CHAOS_GL_MAX_TEX_SIZE) {
            serial_printf("[texture] invalid RAWT dimensions %ux%u in '%s'\n",
                          hdr->width, hdr->height, path);
            kfree(file_buf);
            return -1;
        }
        w = hdr->width;
        h = hdr->height;
        uint32_t pixel_size = w * h * 4;
        if (total_read < 16 + pixel_size) {
            serial_printf("[texture] RAWT truncated in '%s'\n", path);
            kfree(file_buf);
            return -1;
        }
        has_alpha = false;
        /* Pixels are at file_buf + 16, we'll copy directly to PMM below */
        pixel_data = NULL;  /* Signal to use file_buf + 16 */
    } else if (fmt == STBI_FMT_PNG || fmt == STBI_FMT_JPEG ||
               fmt == STBI_FMT_BMP || fmt == STBI_FMT_GIF) {
        /* Decode via stb_image */
        int iw, ih, ia;
        pixel_data = stbi_decode_from_memory_bgra(file_buf, (int)total_read,
                                                   &iw, &ih, &ia);
        if (!pixel_data) {
            serial_printf("[texture] stb_image decode failed for '%s'\n", path);
            kfree(file_buf);
            return -1;
        }
        w = (uint32_t)iw;
        h = (uint32_t)ih;
        has_alpha = (ia != 0);

        if (w > CHAOS_GL_MAX_TEX_SIZE || h > CHAOS_GL_MAX_TEX_SIZE) {
            serial_printf("[texture] image too large %ux%u in '%s'\n", w, h, path);
            kfree(pixel_data);
            kfree(file_buf);
            return -1;
        }
    } else {
        serial_printf("[texture] unknown format in '%s'\n", path);
        kfree(file_buf);
        return -1;
    }

    /* Allocate PMM pages for final pixel storage */
    uint32_t pixel_size = w * h * 4;
    uint32_t pages = (pixel_size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint32_t phys = pmm_alloc_pages(pages);
    if (phys == 0) {
        serial_printf("[texture] pmm_alloc_pages(%u) failed for '%s'\n", pages, path);
        if (pixel_data) kfree(pixel_data);
        kfree(file_buf);
        return -1;
    }

    vmm_map_range(phys, phys, pages * PAGE_SIZE, PTE_PRESENT | PTE_WRITABLE);
    heap_mark_reserved(phys, pages);

    /* Copy pixel data into PMM pages */
    if (pixel_data) {
        memcpy((void *)phys, pixel_data, pixel_size);
        kfree(pixel_data);
    } else {
        /* RAWT: pixels at file_buf + 16 */
        memcpy((void *)phys, file_buf + 16, pixel_size);
    }
    kfree(file_buf);

    /* Fill the texture struct */
    textures[handle].data      = (uint32_t *)phys;
    textures[handle].width     = (int)w;
    textures[handle].height    = (int)h;
    textures[handle].pitch     = (int)w;
    textures[handle].in_use    = true;
    textures[handle].has_alpha = has_alpha;
    textures[handle].phys_addr = phys;
    textures[handle].pages     = pages;

    serial_printf("[texture] loaded '%s' as handle %d (%ux%u, %u pages, alpha=%d)\n",
                  path, handle, w, h, pages, has_alpha);
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

bool chaos_gl_texture_has_alpha(int handle) {
    if (handle < 0 || handle >= CHAOS_GL_MAX_TEXTURES) return false;
    if (!textures[handle].in_use) return false;
    return textures[handle].has_alpha;
}
