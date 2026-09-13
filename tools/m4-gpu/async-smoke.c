// SPDX-License-Identifier: GPL-2.0-only
// Copyright The Gravity Linux Contributors
/* A focused Asahi-UAPI probe wrapped around one real Mesa workload.
 * It injects an externally delayed input fence and timestamp destinations.
 * No firmware ABI or GPU instruction encoding is used here. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <drm/asahi_drm.h>

struct sw_sync_create_fence_data { uint32_t value; char name[32]; int32_t fence; };
#define SW_CREATE _IOWR('W', 0, struct sw_sync_create_fence_data)
#define SW_INC _IOW('W', 1, uint32_t)
static int (*real_ioctl)(int,unsigned long,...);
static int gpu = -1, timeline = -1;
static uint32_t timestamp_handle;
static volatile uint64_t *words;
static uint64_t initial_time;
static atomic_int signaled;
static unsigned submits, timestamp_words, timestamp_mask=15;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static void require(int ok, const char *what) {
    if (!ok) { fprintf(stderr,"M4_UAPI_SMOKE_FAIL %s: %s\n",what,strerror(errno)); _exit(91); }
}
static int call(int fd,unsigned long request,void *arg) {
    return real_ioctl(fd,request,arg);
}
static void *release_input(void *unused) {
    (void)unused;
    struct timespec delay = { .tv_nsec=200000000 };
    nanosleep(&delay,NULL);
    atomic_store_explicit(&signaled,1,memory_order_release);
    uint32_t value=1;
    require(call(timeline,SW_INC,&value)==0,"signal delayed input");
    return NULL;
}
static void initialize(int fd) {
    if(getenv("M4_TS_MASK")) {
        timestamp_mask=strtoul(getenv("M4_TS_MASK"),NULL,0);
        require(timestamp_mask>0 && timestamp_mask<=15,"timestamp selection mask");
    }
    gpu=dup(fd); require(gpu>=0,"duplicate GPU file");
    struct drm_asahi_gem_create bo={.size=0x4000};
    require(call(gpu,DRM_IOCTL_ASAHI_GEM_CREATE,&bo)==0,"create timestamp BO");
    struct drm_asahi_gem_mmap_offset map={.handle=bo.handle};
    require(call(gpu,DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET,&map)==0,"timestamp mmap offset");
    words=mmap(NULL,0x4000,PROT_READ|PROT_WRITE,MAP_SHARED,gpu,map.offset);
    require(words!=MAP_FAILED,"map timestamp BO");
    for(unsigned i=0;i<0x4000/8;i++) words[i]=0xfeedfacefeedfaceULL;
    if(getenv("M4_TS_ZERO")) for(unsigned i=1;i<=4;i++) words[i]=0;
    struct drm_asahi_gem_bind_object bind={.op=DRM_ASAHI_BIND_OBJECT_OP_BIND,
        .flags=DRM_ASAHI_BIND_OBJECT_USAGE_TIMESTAMPS,.handle=bo.handle,.offset=0,.range=0x4000};
    require(call(gpu,DRM_IOCTL_ASAHI_GEM_BIND_OBJECT,&bind)==0,"bind timestamp object");
    timestamp_handle=bind.object_handle;
    struct drm_asahi_get_time now={0};
    require(call(gpu,DRM_IOCTL_ASAHI_GET_TIME,&now)==0,"initial GPU time");
    initial_time=now.gpu_timestamp;
}
int ioctl(int fd,unsigned long request,...) {
    va_list ap; va_start(ap,request); void *arg=va_arg(ap,void*); va_end(ap);
    if(!real_ioctl) real_ioctl=dlsym(RTLD_NEXT,"ioctl");
    if(request!=DRM_IOCTL_ASAHI_SUBMIT) return call(fd,request,arg);
    pthread_mutex_lock(&lock);
    if(gpu<0) initialize(fd);
    struct drm_asahi_submit submit=*(struct drm_asahi_submit*)arg;
    void *commands=malloc(submit.cmdbuf_size); require(commands!=NULL,"copy commands");
    memcpy(commands,(void*)(uintptr_t)submit.cmdbuf,submit.cmdbuf_size);
    submit.cmdbuf=(uintptr_t)commands;
    for(size_t offset=0;offset<submit.cmdbuf_size;) {
        struct drm_asahi_cmd_header *h=(void*)((char*)commands+offset);
        offset+=sizeof(*h);
        require(offset+h->size<=submit.cmdbuf_size,"command bounds");
        if(h->cmd_type==DRM_ASAHI_CMD_RENDER) {
            struct drm_asahi_cmd_render *r=(void*)((char*)commands+offset);
            require(h->size>=sizeof(*r),"render size");
            r->ts_vtx.start=(struct drm_asahi_timestamp){timestamp_handle,8};
            r->ts_vtx.end=(struct drm_asahi_timestamp){timestamp_handle,16};
            r->ts_frag.start=(struct drm_asahi_timestamp){timestamp_handle,24};
            r->ts_frag.end=(struct drm_asahi_timestamp){timestamp_handle,32};
            if(!(timestamp_mask&1))r->ts_vtx.start=(struct drm_asahi_timestamp){0};
            if(!(timestamp_mask&2))r->ts_vtx.end=(struct drm_asahi_timestamp){0};
            if(!(timestamp_mask&4))r->ts_frag.start=(struct drm_asahi_timestamp){0};
            if(!(timestamp_mask&8))r->ts_frag.end=(struct drm_asahi_timestamp){0};
            timestamp_words=4;
        } else if(h->cmd_type==DRM_ASAHI_CMD_COMPUTE) {
            struct drm_asahi_cmd_compute *c=(void*)((char*)commands+offset);
            require(h->size>=sizeof(*c),"compute size");
            c->ts.start=(struct drm_asahi_timestamp){timestamp_handle,8};
            c->ts.end=(struct drm_asahi_timestamp){timestamp_handle,16};
            if(!(timestamp_mask&1))c->ts.start=(struct drm_asahi_timestamp){0};
            if(!(timestamp_mask&2))c->ts.end=(struct drm_asahi_timestamp){0};
            timestamp_words=2;
        }
        offset+=h->size;
    }
    pthread_t signal_thread;
    struct drm_syncobj_create input={0};
    struct drm_asahi_sync *syncs=NULL;
    if(submits==0) {
        timeline=open("/sys/kernel/debug/sync/sw_sync",O_RDWR|O_CLOEXEC);
        require(timeline>=0,"open software timeline (mount debugfs)");
        struct sw_sync_create_fence_data pending={.value=1,.name="asahi-input",.fence=-1};
        require(call(timeline,SW_CREATE,&pending)==0,"create pending sync_file");
        require(call(fd,DRM_IOCTL_SYNCOBJ_CREATE,&input)==0,"create input syncobj");
        struct drm_syncobj_handle import={.handle=input.handle,
            .flags=DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE,.fd=pending.fence};
        require(call(fd,DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE,&import)==0,"import input fence");
        close(pending.fence);
        size_t count=submit.in_sync_count+submit.out_sync_count;
        syncs=calloc(count+1,sizeof(*syncs)); require(syncs!=NULL,"copy sync list");
        if(count) memcpy(syncs+1,(void*)(uintptr_t)submit.syncs,count*sizeof(*syncs));
        syncs[0]=(struct drm_asahi_sync){.handle=input.handle};
        submit.in_sync_count++; submit.syncs=(uintptr_t)syncs;
        require(pthread_create(&signal_thread,NULL,release_input,NULL)==0,"start fence signaler");
    }
    int result=call(fd,request,&submit), saved=errno;
    if(submits==0) {
        require(result==0,"submit with pending input");
        require(!atomic_load_explicit(&signaled,memory_order_acquire),"SUBMIT waited for input fence");
        for(unsigned i=1;i<=4;i++) require(words[i]==(getenv("M4_TS_ZERO") ? 0 : 0xfeedfacefeedfaceULL),"GPU ran before input dependency");
        require(submit.out_sync_count>0,"Mesa supplied output syncobjs");
        for(unsigned i=0;i<submit.out_sync_count;i++) {
            struct drm_asahi_sync *s=&syncs[submit.in_sync_count+i];
            int status;
            if(s->sync_type==DRM_ASAHI_SYNC_TIMELINE_SYNCOBJ) {
                struct drm_syncobj_timeline_wait wait={.handles=(uintptr_t)&s->handle,
                    .points=(uintptr_t)&s->timeline_value,.count_handles=1};
                status=call(fd,DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT,&wait);
            } else {
                struct drm_syncobj_wait wait={.handles=(uintptr_t)&s->handle,.count_handles=1};
                status=call(fd,DRM_IOCTL_SYNCOBJ_WAIT,&wait);
            }
            require(status==-1 && errno==ETIME,"output fence signaled before input dependency");
        }
        require(!atomic_load_explicit(&signaled,memory_order_acquire),"input released during pending-output check");
        fprintf(stderr,"M4_UAPI_ASYNC_PASS submit returned with input and output fences pending; timestamp writes deferred\n");
        pthread_join(signal_thread,NULL);
        struct drm_syncobj_destroy destroy={.handle=input.handle};
        require(call(fd,DRM_IOCTL_SYNCOBJ_DESTROY,&destroy)==0,"destroy input syncobj");
        close(timeline);
    }
    submits++; free(syncs); free(commands); pthread_mutex_unlock(&lock); errno=saved;
    return result;
}
__attribute__((destructor)) static void finish(void) {
    if(gpu<0) return;
    struct drm_asahi_get_time now={0};
    require(call(gpu,DRM_IOCTL_ASAHI_GET_TIME,&now)==0,"final GPU time");
    require(words[0]==0xfeedfacefeedfaceULL && words[5]==0xfeedfacefeedfaceULL,"timestamp guards");
    for(unsigned i=1;i<=timestamp_words;i++) {
        fprintf(stderr,"M4_UAPI_TIMESTAMP word=%u value=%llu interval=%llu..%llu\n",i,
            (unsigned long long)words[i],(unsigned long long)initial_time,(unsigned long long)now.gpu_timestamp);

    }
    if(getenv("M4_TS_PAUSE")) { fprintf(stderr,"M4_UAPI_TIMESTAMP_PAUSE\n"); sleep(30); }
    for(unsigned i=1;i<=timestamp_words;i++) {
        if(timestamp_mask&(1u<<(i-1)))require(words[i]>=initial_time && words[i]<=now.gpu_timestamp,"timestamp units/range");
        else require(words[i]==(getenv("M4_TS_ZERO")?0:0xfeedfacefeedfaceULL),"disabled timestamp guard");
    }
    if((timestamp_mask&3)==3)require(words[1]<=words[2],"timestamp ordering");
    if(timestamp_words==4 && (timestamp_mask&12)==12)require(words[3]<=words[4],"fragment timestamp ordering");
    fprintf(stderr,"M4_UAPI_TIMESTAMP_PASS submissions=%u words=%u selected_mask=%x\n",submits,timestamp_words,timestamp_mask);
    munmap((void*)words,0x4000); close(gpu);
}
