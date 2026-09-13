// SPDX-License-Identifier: GPL-2.0-only
// Copyright The Gravity Linux Contributors
/* Public-UAPI feature evidence around real Mesa commands. No GPU ISA/ABI. */
#define _GNU_SOURCE
#include <drm/asahi_drm.h>
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <time.h>
static int (*next_ioctl)(int,unsigned long,...);
static pthread_once_t once=PTHREAD_ONCE_INIT;
static uint32_t queue_vm[1024];
static uint64_t queue_heap[1024];
static uint64_t layer_va;
static unsigned char *layer_cpu;
static unsigned layer_pixels;
static int destroyed_vm;
static int single_page_armed;
static uint32_t single_page_handle;
void m4_single_page_arm(void) { single_page_armed=1; }
int m4_single_page_ready(void) { return single_page_handle!=0; }
static void init(void) { *(void **)(&next_ioctl)=dlsym(RTLD_NEXT,"ioctl"); }
static void fail(const char *s) { fprintf(stderr,"FEATURE_PROBE_FAIL %s errno=%d\n",s,errno);_exit(92); }
int ioctl(int fd,unsigned long op,...) {
    va_list ap;va_start(ap,op);void *arg=va_arg(ap,void*);va_end(ap);
    pthread_once(&once,init);
    if(op==DRM_IOCTL_ASAHI_QUEUE_CREATE && getenv("M4_UAPI_MEDIUM"))
        ((struct drm_asahi_queue_create *)arg)->priority=DRM_ASAHI_PRIORITY_MEDIUM;
    if(op==DRM_IOCTL_ASAHI_GEM_CREATE && single_page_armed) {
        struct drm_asahi_gem_create *b=arg;int result=next_ioctl(fd,op,arg);
        if(!result && b->size==0xc000) {
            single_page_handle=b->handle;single_page_armed=0;
            fprintf(stderr,"M4_UAPI_SINGLE_PAGE_BO handle=%u\n",b->handle);
        }
        return result;
    }
    if(op==DRM_IOCTL_ASAHI_VM_BIND && single_page_handle) {
        struct drm_asahi_vm_bind bind=*(struct drm_asahi_vm_bind*)arg;
        size_t size=(size_t)bind.num_binds*bind.stride;
        unsigned char *records=malloc(size);if(!records)fail("single-page bind records");
        memcpy(records,(void*)(uintptr_t)bind.userptr,size);bind.userptr=(uintptr_t)records;
        for(unsigned i=0;i<bind.num_binds;i++) {
            struct drm_asahi_gem_bind_op *b=(void*)(records+(size_t)i*bind.stride);
            if(b->handle==single_page_handle && !(b->flags&DRM_ASAHI_BIND_UNBIND)) {
                if(b->offset || b->range!=0xc000)fail("single-page expected buffer range");
                b->flags|=DRM_ASAHI_BIND_SINGLE_PAGE;
                fprintf(stderr,"M4_UAPI_SINGLE_PAGE_GPU_ALIAS pages=3\n");
            }
        }
        int result=next_ioctl(fd,op,&bind),saved=errno;free(records);errno=saved;return result;
    }
    if(op==DRM_IOCTL_ASAHI_QUEUE_CREATE) {
        int result=next_ioctl(fd,op,arg);
        struct drm_asahi_queue_create *q=arg;
        if(!result && q->queue_id<1024) {
            queue_vm[q->queue_id]=q->vm_id;
            if(getenv("M4_UAPI_LAYER_CLEAR")) {
                struct drm_asahi_gem_create bo={.size=0x20000};
                if(next_ioctl(fd,DRM_IOCTL_ASAHI_GEM_CREATE,&bo))fail("layer clear BO");
                struct drm_asahi_gem_mmap_offset m={.handle=bo.handle};
                if(next_ioctl(fd,DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET,&m))fail("layer clear mmap offset");
                layer_cpu=mmap(NULL,0x20000,PROT_READ|PROT_WRITE,MAP_SHARED,fd,m.offset);
                if(layer_cpu==MAP_FAILED)fail("layer clear mmap");
                memset(layer_cpu,0,0x14000);memset(layer_cpu+0x14000,0xa5,0xc000);
                /* Source VDM stream with only the documented terminate packet. */
                uint32_t stop=0xc0000000;memcpy(layer_cpu,&stop,sizeof(stop));
                layer_va=0x1040000000ULL+(uint64_t)q->queue_id*0x20000;
                struct drm_asahi_gem_bind_op b={.flags=DRM_ASAHI_BIND_READ|DRM_ASAHI_BIND_WRITE,.handle=bo.handle,.range=0x20000,.addr=layer_va};
                struct drm_asahi_vm_bind bind={.vm_id=q->vm_id,.num_binds=1,.stride=sizeof(b),.userptr=(uintptr_t)&b};
                if(next_ioctl(fd,DRM_IOCTL_ASAHI_VM_BIND,&bind))fail("layer clear VM bind");
            }
            if(getenv("M4_UAPI_HEAP")) {
                struct drm_asahi_gem_create bo={.size=0x4000};
                if(next_ioctl(fd,DRM_IOCTL_ASAHI_GEM_CREATE,&bo))fail("sampler heap BO");
                struct drm_asahi_gem_mmap_offset m={.handle=bo.handle};
                if(next_ioctl(fd,DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET,&m))fail("sampler heap mmap offset");
                void *p=mmap(NULL,0x4000,PROT_READ|PROT_WRITE,MAP_SHARED,fd,m.offset);
                if(p==MAP_FAILED)fail("sampler heap mmap");
                /* Mesa genxml's eight-byte Sampler record: zero selects
                 * nearest filtering, repeat wrapping, LOD 0, anisotropy 1.
                 * Four valid but unused records exercise firmware heap setup. */
                memset(p,0,0x4000);munmap(p,0x4000);
                uint64_t va=0x1700000000ULL+(uint64_t)q->queue_id*0x4000;
                struct drm_asahi_gem_bind_op b={.flags=DRM_ASAHI_BIND_READ,.handle=bo.handle,.range=0x4000,.addr=va};
                struct drm_asahi_vm_bind bind={.vm_id=q->vm_id,.num_binds=1,.stride=sizeof(b),.userptr=(uintptr_t)&b};
                if(next_ioctl(fd,DRM_IOCTL_ASAHI_VM_BIND,&bind))fail("sampler heap VM bind");
                queue_heap[q->queue_id]=va;
            }
        }
        return result;
    }
    if(op!=DRM_IOCTL_ASAHI_SUBMIT) return next_ioctl(fd,op,arg);
    struct drm_asahi_submit submit=*(struct drm_asahi_submit *)arg;
    unsigned char *extended=NULL,*attached=NULL,*configured=NULL;size_t tail=0;
    if(getenv("M4_UAPI_RSRC_SPEC") || getenv("M4_UAPI_HEAP") || getenv("M4_UAPI_LAYERS") || getenv("M4_UAPI_LAYER_CLEAR")) {
        configured=malloc(submit.cmdbuf_size);if(!configured)fail("resource command copy");
        memcpy(configured,(void*)(uintptr_t)submit.cmdbuf,submit.cmdbuf_size);submit.cmdbuf=(uintptr_t)configured;
        for(size_t pos=0;pos<submit.cmdbuf_size;) {
            struct drm_asahi_cmd_header *h=(void*)(configured+pos);
            if(h->cmd_type==DRM_ASAHI_CMD_RENDER && h->size>=sizeof(struct drm_asahi_cmd_render)) {
                struct drm_asahi_cmd_render *c=(void*)(h+1);
                /* Public resource word: reserve 64 uniforms (Mesa genxml,
                 * Fragment Shader Word 0). Extra unused capacity is valid. */
                if(getenv("M4_UAPI_RSRC_SPEC")) {
                    c->eot.rsrc_spec|=2;c->partial_eot.rsrc_spec|=2;
                    if(c->bg.usc)c->bg.rsrc_spec|=2;
                    if(c->partial_bg.usc)c->partial_bg.rsrc_spec|=2;
                }
                if(getenv("M4_UAPI_HEAP")) {
                    if(submit.queue_id>=1024 || !queue_heap[submit.queue_id])fail("render sampler heap lookup");
                    c->sampler_heap=queue_heap[submit.queue_id];c->sampler_count=4;
                }
                if(getenv("M4_UAPI_LAYERS"))c->layers=2;
                if(getenv("M4_UAPI_LAYER_CLEAR")) {
                    if(!layer_va)fail("layer clear allocation missing");
                    c->layers=2;c->flags=getenv("M4_UAPI_NO_EMPTY")?0:DRM_ASAHI_RENDER_PROCESS_EMPTY_TILES;
                    c->vdm_ctrl_stream_base=layer_va;
                    c->depth=(struct drm_asahi_zls_buffer){.base=layer_va+0x4000,.stride=1};
                    c->stencil=(struct drm_asahi_zls_buffer){.base=layer_va+0xc000,.stride=1};
                    c->zls_ctrl=(1u<<19)|(1u<<18); /* Z/S STORE without LOAD */
                    c->isp_zls_pixels=(c->width_px-1)|((c->height_px-1)<<15);
                    c->isp_bgobjdepth=0x3e800000;c->isp_bgobjvals=0x5a;
                    layer_pixels=c->width_px*c->height_px;
                }
            } else if(h->cmd_type==DRM_ASAHI_CMD_COMPUTE && h->size>=sizeof(struct drm_asahi_cmd_compute) && getenv("M4_UAPI_HEAP")) {
                struct drm_asahi_cmd_compute *c=(void*)(h+1);
                if(submit.queue_id>=1024 || !queue_heap[submit.queue_id])fail("compute sampler heap lookup");
                c->sampler_heap=queue_heap[submit.queue_id];c->sampler_count=4;
            }
            pos+=sizeof(*h)+h->size;
        }
    }
    if(getenv("M4_UAPI_ATTACH_ALL")) {
        /* Attachment records are optional residency hints. Exercise replacing
         * and clearing every stage, using an existing writable Mesa target. */
        struct drm_asahi_attachment a={0};
        for(size_t in=0;in<submit.cmdbuf_size;) {
            struct drm_asahi_cmd_header h;memcpy(&h,(void*)(uintptr_t)(submit.cmdbuf+in),sizeof(h));
            if(h.cmd_type>=DRM_ASAHI_SET_VERTEX_ATTACHMENTS && h.size>=sizeof(a))
                memcpy(&a,(void*)(uintptr_t)(submit.cmdbuf+in+sizeof(h)),sizeof(a));
            in+=sizeof(h)+h.size;
        }
        if(a.size) {
            attached=calloc(1,submit.cmdbuf_size+512);if(!attached)fail("attachment allocation");
            size_t out=0;
            for(unsigned stage=2;stage<=4;stage++) {
                struct drm_asahi_cmd_header h={.cmd_type=stage,.size=sizeof(a),.vdm_barrier=0xffff,.cdm_barrier=0xffff};
                memcpy(attached+out,&h,sizeof(h));out+=sizeof(h);memcpy(attached+out,&a,sizeof(a));out+=sizeof(a);
                h.size=0;memcpy(attached+out,&h,sizeof(h));out+=sizeof(h);
            }
            memcpy(attached+out,(void*)(uintptr_t)submit.cmdbuf,submit.cmdbuf_size);
            submit.cmdbuf=(uintptr_t)attached;submit.cmdbuf_size+=out;
        }
    }
    if(getenv("M4_UAPI_EXTEND")) {
        extended=calloc(1,submit.cmdbuf_size+512);if(!extended)fail("allocate extended commands");
        size_t out=0;
        for(size_t in=0;in<submit.cmdbuf_size;) {
            struct drm_asahi_cmd_header h;memcpy(&h,(void*)(uintptr_t)(submit.cmdbuf+in),sizeof(h));
            size_t n=sizeof(h)+h.size;if(n>submit.cmdbuf_size-in)fail("command bounds");
            memcpy(extended+out,(void*)(uintptr_t)(submit.cmdbuf+in),n);
            if(h.cmd_type<=DRM_ASAHI_CMD_COMPUTE) {
                tail=out+n;h.size+=8;memcpy(extended+out,&h,sizeof(h));out+=8;
            }
            in+=n;out+=n;
        }
        submit.cmdbuf=(uintptr_t)extended;submit.cmdbuf_size=out;
        if(tail) {
            extended[tail]=1;
            if(next_ioctl(fd,op,&submit)!=-1 || errno!=EINVAL)fail("nonzero unknown command suffix");
            extended[tail]=0;
            fprintf(stderr,"M4_UAPI_EXTENSION_REJECT_PASS\n");
        }
    }
    if(getenv("M4_UAPI_DESTROY_VM") && !destroyed_vm) {
        if(submit.queue_id>=1024 || !queue_vm[submit.queue_id])fail("queue VM lookup");
        struct drm_asahi_vm_destroy d={.vm_id=queue_vm[submit.queue_id]};
        if(next_ioctl(fd,DRM_IOCTL_ASAHI_VM_DESTROY,&d))fail("destroy live queue VM handle");
        destroyed_vm=1;fprintf(stderr,"M4_UAPI_VM_HANDLE_DESTROYED_BEFORE_SUBMIT vm=%u\n",d.vm_id);
    }
    int result=next_ioctl(fd,op,&submit),saved=errno;
    if(result==0) {
        for(size_t pos=0;pos<submit.cmdbuf_size;) {
            struct drm_asahi_cmd_header h;memcpy(&h,(void*)(uintptr_t)(submit.cmdbuf+pos),sizeof(h));
            const void *payload=(void*)(uintptr_t)(submit.cmdbuf+pos+sizeof(h));
            if(h.cmd_type==DRM_ASAHI_CMD_RENDER) {
                struct drm_asahi_cmd_render c={0};memcpy(&c,payload,h.size<sizeof(c)?h.size:sizeof(c));
                fprintf(stderr,"M4_UAPI_RENDER flags=%x layers=%u samples=%u utile=%ux%u sample_size=%u sampler=%u depth=%u stencil=%u dcomp=%u scomp=%u dstride=%x sstride=%x dcstride=%x scstride=%x query=%u dbias=%u zls=%llx bg=%x/%x eot=%x/%x pbg=%x/%x peot=%x/%x barriers=%u/%u\n",
                    c.flags,c.layers,c.samples,c.utile_width_px,c.utile_height_px,c.sample_size_B,c.sampler_count,
                    !!c.depth.base,!!c.stencil.base,!!c.depth.comp_base,!!c.stencil.comp_base,
                    c.depth.stride,c.stencil.stride,c.depth.comp_stride,c.stencil.comp_stride,
                    !!c.isp_oclqry_base,!!c.isp_dbias_base,(unsigned long long)c.zls_ctrl,
                    c.bg.usc,c.bg.rsrc_spec,c.eot.usc,c.eot.rsrc_spec,c.partial_bg.usc,c.partial_bg.rsrc_spec,
                    c.partial_eot.usc,c.partial_eot.rsrc_spec,h.vdm_barrier,h.cdm_barrier);
            } else if(h.cmd_type==DRM_ASAHI_CMD_COMPUTE) {
                struct drm_asahi_cmd_compute c={0};memcpy(&c,payload,h.size<sizeof(c)?h.size:sizeof(c));
                fprintf(stderr,"M4_UAPI_COMPUTE sampler=%u bytes=%llu barriers=%u/%u\n",c.sampler_count,
                    (unsigned long long)(c.cdm_ctrl_stream_end-c.cdm_ctrl_stream_base),h.vdm_barrier,h.cdm_barrier);
            } else fprintf(stderr,"M4_UAPI_ATTACHMENTS stage=%u count=%u\n",h.cmd_type,h.size/(unsigned)sizeof(struct drm_asahi_attachment));
            pos+=sizeof(h)+h.size;
        }
        if(extended) fprintf(stderr,"M4_UAPI_EXTENSION_ACCEPT_PASS\n");
    }
    if(!result && getenv("M4_UAPI_LAYER_CLEAR")) {
        if(!submit.out_sync_count)fail("layer clear output fence missing");
        const struct drm_asahi_sync *syncs=(void*)(uintptr_t)submit.syncs;
        struct timespec now;clock_gettime(CLOCK_MONOTONIC,&now);
        int64_t deadline=(int64_t)now.tv_sec*1000000000+now.tv_nsec+10000000000LL;
        for(unsigned i=0;i<submit.out_sync_count;i++) {
            const struct drm_asahi_sync *f=&syncs[submit.in_sync_count+i];
            int result;
            if(f->sync_type==DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ) {
                struct drm_syncobj_timeline_wait w={.handles=(uintptr_t)&f->handle,.points=(uintptr_t)&f->timeline_value,.count_handles=1,.timeout_nsec=deadline};
                result=next_ioctl(fd,DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT,&w);
            } else {
                struct drm_syncobj_wait w={.handles=(uintptr_t)&f->handle,.count_handles=1,.timeout_nsec=deadline};
                result=next_ioctl(fd,DRM_IOCTL_SYNCOBJ_WAIT,&w);
            }
            if(result)fail("layer clear fence wait");
        }
        unsigned z=0,st=0;
        for(unsigned i=0;i<0x4000;i+=4) {
            uint32_t value;memcpy(&value,layer_cpu+0x4000+i,4);
            if(value && value!=0x3e800000)fail("layer clear depth value");
            z+=value!=0;
        }
        for(unsigned i=0;i<0x4000;i++) {
            unsigned char value=layer_cpu[0xc000+i];
            if(value && value!=0x5a)fail("layer clear stencil value");
            st+=value!=0;
        }
        fprintf(stderr,"M4_UAPI_LAYER_CLEAR_COUNTS depth=%u stencil=%u pixels=%u\n",z,st,layer_pixels);
        if(getenv("M4_UAPI_NO_EMPTY")) {
            if(z || st)fail("disabled empty tile processing wrote ZLS");
        } else if(z<layer_pixels || st<layer_pixels)fail("layer clear coverage");
        if(memcmp(layer_cpu+0x4000,layer_cpu+0x8000,0x4000) || memcmp(layer_cpu+0xc000,layer_cpu+0x10000,0x4000))fail("layer clear matching slices");
        for(unsigned i=0x14000;i<0x20000;i++)if(layer_cpu[i]!=0xa5)fail("layer clear allocation guards");
        fprintf(stderr,"M4_UAPI_LAYER_CLEAR_PASS layers=2 depth=0.25 stencil=0x5a guards=49152 empty_enabled=%u\n",!getenv("M4_UAPI_NO_EMPTY"));
        /* This probe intentionally replaces the GL draw with an empty VDM
         * stream. Its oracle is the ZLS allocation, not the GL color oracle. */
        _exit(0);
    }
    free(extended);free(attached);free(configured);errno=saved;return result;
}
