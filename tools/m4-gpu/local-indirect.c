// SPDX-License-Identifier: GPL-2.0-only
// Copyright The Gravity Linux Contributors
/* Test-only CDM substitution for the host's g16g_local_indirect.c workload.
 * Mesa emits global-indirect for GLES. Select the producer's six-word
 * geometry instead, without changing shaders, geometry or kernel behavior.
 * M4_LOCAL_INDIRECT=global leaves the packet intact; =zero selects the
 * caller's deliberately wrong, valid zero-work geometry. Default: local.
 */
#define _GNU_SOURCE
#include <drm/asahi_drm.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

enum { LIMIT = 4096 };
struct binding {
    int fd;
    uint32_t vm, handle, flags;
    uint64_t start, end, offset;
};
struct queue { int fd; uint32_t id, vm; };
static struct binding bindings[LIMIT];
static struct queue queues[LIMIT];
static unsigned nb, nq;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int (*next_ioctl)(int, unsigned long, ...);

static void fail(const char *message)
{
    fprintf(stderr, "KERNEL_LOCAL_INDIRECT_FAIL %s errno=%d\n", message, errno);
    _exit(92);
}

static void init(void)
{
    *(void **)(&next_ioctl) = dlsym(RTLD_NEXT, "ioctl");
    if (!next_ioctl) fail("ioctl lookup");
}

static void add(struct binding b)
{
    if (nb == LIMIT) fail("binding capacity");
    bindings[nb++] = b;
}

static void unbind(int fd, uint32_t vm, uint64_t start, uint64_t end)
{
    for (unsigned i = 0; i < nb;) {
        struct binding b = bindings[i];
        if (b.fd != fd || b.vm != vm || b.end <= start || b.start >= end) {
            i++;
            continue;
        }
        if (b.start < start) {
            bindings[i++].end = start;
            if (b.end > end) {
                b.offset += end - b.start;
                b.start = end;
                add(b);
            }
        } else if (b.end > end) {
            bindings[i].offset += end - b.start;
            bindings[i++].start = end;
        } else {
            bindings[i] = bindings[--nb];
        }
    }
}

static unsigned rewrite(int fd, uint32_t vm,
                        const struct drm_asahi_cmd_compute *command,
                        const char *mode)
{
    uint64_t base = command->cdm_ctrl_stream_base;
    uint64_t end = command->cdm_ctrl_stream_end;
    if (end <= base || end - base > 0x100000 || (base | end) & 3)
        fail("CDM bounds");
    struct binding *b = NULL;
    for (unsigned i = 0; i < nb; i++)
        if (bindings[i].fd == fd && bindings[i].vm == vm &&
            bindings[i].start <= base && bindings[i].end >= end) {
            b = &bindings[i];
            break;
        }
    if (!b || b->flags & DRM_ASAHI_BIND_SINGLE_PAGE)
        fail("probe requires a single ordinary CDM binding");
    uint64_t offset = b->offset + base - b->start;
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    size_t length = (offset + end - base + page - 1) & ~(page - 1);
    struct drm_asahi_gem_mmap_offset m = {.handle = b->handle};
    if (next_ioctl(fd, DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET, &m))
        fail("CDM mmap offset");
    unsigned char *map = mmap(NULL, length, PROT_READ | PROT_WRITE,
                              MAP_SHARED, fd, m.offset);
    if (map == MAP_FAILED) fail("CDM mmap");
    uint32_t *words = (void *)(map + offset);
    size_t count = (end - base) / 4;
    unsigned launches = 0;
    bool terminated = false;
    for (size_t pos = 0; pos < count;) {
        uint32_t header = words[pos], kind = header >> 29;
        if (kind == 0) {
            unsigned launch_mode = (header >> 27) & 3;
            unsigned size = launch_mode == 0 ? 10 : launch_mode == 1 ? 9 : 6;
            if (launch_mode == 3 || size > count - pos) fail("launch bounds");
            if (launch_mode == 1) {
                if (strcmp(mode, "global")) {
                    uint64_t geometry = ((uint64_t)words[pos+4] << 32) | words[pos+5];
                    geometry += !strcmp(mode, "zero") ? 64 : 16;
                    words[pos] = (header & ~(3u << 27)) | (2u << 27);
                    words[pos+4] = geometry >> 32;
                    words[pos+5] = geometry;
                    /* Preserve the stream terminator and all later addresses. */
                    words[pos+6] = words[pos+7] = words[pos+8] = 0x60000160;
                }
                launches++;
            }
            pos += size;
        } else if (kind == 3) {
            pos++;
        } else if (kind == 2 && pos + 1 == count) {
            terminated = true;
            break;
        } else {
            fail("probe expects a linear terminated Mesa CDM stream");
        }
    }
    if (!terminated) fail("missing terminator");
    if (munmap(map, length)) fail("CDM munmap");
    return launches;
}

int ioctl(int fd, unsigned long request, ...)
{
    va_list ap;
    va_start(ap, request);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    pthread_once(&once, init);
    pthread_mutex_lock(&lock);
    unsigned launches = 0;
    const char *mode = getenv("M4_LOCAL_INDIRECT");
    if (!mode) mode = "local";
    if (strcmp(mode, "local") && strcmp(mode, "global") && strcmp(mode, "zero"))
        fail("invalid mode");
    if (request == DRM_IOCTL_ASAHI_SUBMIT) {
        const struct drm_asahi_submit *submit = arg;
        uint32_t vm = 0;
        bool found = false;
        for (unsigned i = 0; i < nq; i++)
            if (queues[i].fd == fd && queues[i].id == submit->queue_id) {
                vm = queues[i].vm;
                found = true;
                break;
            }
        if (!found) fail("submit queue lookup");
        const unsigned char *commands = (void *)(uintptr_t)submit->cmdbuf;
        for (size_t pos = 0; pos < submit->cmdbuf_size;) {
            struct drm_asahi_cmd_header h;
            if (submit->cmdbuf_size - pos < sizeof(h)) fail("command header bounds");
            memcpy(&h, commands + pos, sizeof(h));
            pos += sizeof(h);
            if (h.size > submit->cmdbuf_size - pos) fail("command body bounds");
            if (h.cmd_type == DRM_ASAHI_CMD_COMPUTE) {
                struct drm_asahi_cmd_compute command;
                if (h.size < sizeof(command)) fail("compute command size");
                memcpy(&command, commands + pos, sizeof(command));
                launches += rewrite(fd, vm, &command, mode);
            }
            pos += h.size;
        }
    }
    int result = next_ioctl(fd, request, arg), saved = errno;
    if (launches)
        fprintf(stderr, "KERNEL_LOCAL_INDIRECT mode=%s launches=%u result=%d\n",
                mode, launches, result);
    if (!result && request == DRM_IOCTL_ASAHI_QUEUE_CREATE) {
        const struct drm_asahi_queue_create *q = arg;
        if (nq == LIMIT) fail("queue capacity");
        queues[nq++] = (struct queue){fd, q->queue_id, q->vm_id};
    } else if (!result && request == DRM_IOCTL_ASAHI_QUEUE_DESTROY) {
        const struct drm_asahi_queue_destroy *q = arg;
        for (unsigned i = 0; i < nq;)
            if (queues[i].fd == fd && queues[i].id == q->queue_id)
                queues[i] = queues[--nq];
            else i++;
    } else if (!result && request == DRM_IOCTL_ASAHI_VM_BIND) {
        const struct drm_asahi_vm_bind *v = arg;
        if (v->stride < sizeof(struct drm_asahi_gem_bind_op)) fail("bind stride");
        for (unsigned i = 0; i < v->num_binds; i++) {
            struct drm_asahi_gem_bind_op b;
            memcpy(&b, (void *)(uintptr_t)(v->userptr + (uint64_t)i * v->stride), sizeof(b));
            unbind(fd, v->vm_id, b.addr, b.addr + b.range);
            if (!(b.flags & DRM_ASAHI_BIND_UNBIND))
                add((struct binding){fd, v->vm_id, b.handle, b.flags,
                                     b.addr, b.addr + b.range, b.offset});
        }
    }
    pthread_mutex_unlock(&lock);
    errno = saved;
    return result;
}
