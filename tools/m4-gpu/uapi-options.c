// SPDX-License-Identifier: GPL-2.0-only
// Copyright The Gravity Linux Contributors
/* Focused control-plane checks using only the public Asahi/DRM UAPI. */
#define _GNU_SOURCE
#include <drm/asahi_drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
static unsigned checks;
static void check(int ok,const char *name) {
    if(!ok) { fprintf(stderr,"UAPI_OPTIONS_FAIL %s errno=%d\n",name,errno); exit(1); }
    checks++;
}
static void call(int fd,unsigned long op,void *data,const char *name) { check(ioctl(fd,op,data)==0,name); }
static uint32_t vm(int fd) {
    struct drm_asahi_vm_create v={.kernel_start=0x1080000000,.kernel_end=0x10a0000000};
    call(fd,DRM_IOCTL_ASAHI_VM_CREATE,&v,"VM create"); return v.vm_id;
}
static uint32_t bo(int fd,uint32_t vm_id,unsigned flags) {
    struct drm_asahi_gem_create b={.size=0x8000,.flags=flags,.vm_id=vm_id};
    call(fd,DRM_IOCTL_ASAHI_GEM_CREATE,&b,"BO create"); return b.handle;
}
static uint64_t *map(int fd,uint32_t handle) {
    struct drm_asahi_gem_mmap_offset m={.handle=handle};
    call(fd,DRM_IOCTL_ASAHI_GEM_MMAP_OFFSET,&m,"mmap offset");
    void *p=mmap(NULL,0x8000,PROT_READ|PROT_WRITE,MAP_SHARED,fd,m.offset);
    check(p!=MAP_FAILED,"mmap"); return p;
}
int main(void) {
    int fd=open("/dev/dri/renderD128",O_RDWR),peer=open("/dev/dri/renderD128",O_RDWR);
    check(fd>=0 && peer>=0,"open render files");
    unsigned char bytes[sizeof(struct drm_asahi_params_global)+8];
    const size_t lengths[]={0,8,sizeof(struct drm_asahi_params_global),sizeof(bytes)};
    for(unsigned i=0;i<4;i++) {
        memset(bytes,0xa5,sizeof(bytes));
        struct drm_asahi_get_params p={.pointer=(uintptr_t)bytes,.size=lengths[i]};
        call(fd,DRM_IOCTL_ASAHI_GET_PARAMS,&p,"parameter prefix");
        size_t written=lengths[i]<sizeof(struct drm_asahi_params_global)?lengths[i]:sizeof(struct drm_asahi_params_global);
        for(size_t j=written;j<sizeof(bytes);j++) check(bytes[j]==0xa5,"parameter guard");
    }
    uint32_t v=vm(fd),v2=vm(fd),private=bo(fd,v,DRM_ASAHI_GEM_VM_PRIVATE|DRM_ASAHI_GEM_WRITEBACK);
    uint32_t wb=bo(fd,0,DRM_ASAHI_GEM_WRITEBACK),wc=bo(fd,0,0);
    uint64_t *a=map(fd,wb),*b=map(fd,wc); a[0]=0x1122334455667788ULL;b[0]=0x8877665544332211ULL;
    check(a[0]==0x1122334455667788ULL && b[0]==0x8877665544332211ULL,"WB/WC CPU contents");
    struct drm_prime_handle prime={.handle=private,.flags=DRM_CLOEXEC|DRM_RDWR};
    check(ioctl(fd,DRM_IOCTL_PRIME_HANDLE_TO_FD,&prime)==-1 && errno==EINVAL,"private BO export rejected");
    prime=(struct drm_prime_handle){.handle=wb,.flags=DRM_CLOEXEC|DRM_RDWR};
    call(fd,DRM_IOCTL_PRIME_HANDLE_TO_FD,&prime,"shared BO export");
    struct drm_prime_handle imported={.fd=prime.fd};
    call(peer,DRM_IOCTL_PRIME_FD_TO_HANDLE,&imported,"shared BO import on peer file");
    uint64_t *alias=map(peer,imported.handle);check(alias[0]==a[0],"imported contents");
    alias[1]=0x12345678;check(a[1]==0x12345678,"shared imported writes");
    for(unsigned priority=0;priority<2;priority++) {
        struct drm_asahi_queue_create q={.vm_id=v,.priority=priority,.usc_exec_base=1ULL<<40};
        call(fd,DRM_IOCTL_ASAHI_QUEUE_CREATE,&q,"queue priority");
        struct drm_asahi_queue_destroy d={.queue_id=q.queue_id};
        call(fd,DRM_IOCTL_ASAHI_QUEUE_DESTROY,&d,"queue destroy");
    }
    struct extended { struct drm_asahi_gem_bind_op op; uint64_t extra; } ops[2]={
        {.op={.flags=DRM_ASAHI_BIND_READ,.handle=wb,.range=0x4000,.addr=0x1400000000}},
        {.op={.flags=DRM_ASAHI_BIND_WRITE,.handle=wc,.offset=0x4000,.range=0x4000,.addr=0x1400004000}}};
    struct drm_asahi_vm_bind bind={.vm_id=v,.num_binds=2,.stride=sizeof(ops[0]),.userptr=(uintptr_t)ops};
    call(fd,DRM_IOCTL_ASAHI_VM_BIND,&bind,"extended batched bind and offsets");
    ops[0].op.addr=0x1400010000;ops[0].extra=1;bind.num_binds=1;
    check(ioctl(fd,DRM_IOCTL_ASAHI_VM_BIND,&bind)==-1 && errno==EINVAL,"unknown nonzero bind tail rejected");
    ops[0].extra=0;ops[0].op.flags=DRM_ASAHI_BIND_READ|DRM_ASAHI_BIND_WRITE|DRM_ASAHI_BIND_SINGLE_PAGE;
    ops[0].op.offset=0x4000;ops[0].op.range=0x8000;
    call(fd,DRM_IOCTL_ASAHI_VM_BIND,&bind,"single page repeated mapping");
    ops[0].op=(struct drm_asahi_gem_bind_op){.flags=DRM_ASAHI_BIND_READ,.handle=private,.range=0x4000,.addr=0x1400020000};
    bind.vm_id=v2;check(ioctl(fd,DRM_IOCTL_ASAHI_VM_BIND,&bind)==-1 && errno==EINVAL,"private BO VM ownership");
    bind.vm_id=v;bind.stride=31;ops[0].op.handle=wb;
    call(fd,DRM_IOCTL_ASAHI_VM_BIND,&bind,"short bind zero extension");
    ops[0].op=(struct drm_asahi_gem_bind_op){.flags=DRM_ASAHI_BIND_UNBIND,.range=0x4000,.addr=0x1400014000};
    bind.stride=sizeof(ops[0]);call(fd,DRM_IOCTL_ASAHI_VM_BIND,&bind,"partial range unbind");
    struct drm_asahi_gem_bind_object object={.flags=DRM_ASAHI_BIND_OBJECT_USAGE_TIMESTAMPS,
        .handle=wc,.offset=0x4000,.range=0x4000};
    call(fd,DRM_IOCTL_ASAHI_GEM_BIND_OBJECT,&object,"timestamp offset binding");
    object=(struct drm_asahi_gem_bind_object){.op=DRM_ASAHI_BIND_OBJECT_OP_UNBIND,.object_handle=object.object_handle};
    call(fd,DRM_IOCTL_ASAHI_GEM_BIND_OBJECT,&object,"timestamp unbind");
    struct drm_asahi_gem_create bad={.size=0x4000,.pad=1};
    check(ioctl(fd,DRM_IOCTL_ASAHI_GEM_CREATE,&bad)==-1 && errno==EINVAL,"GEM reserved padding");
    struct drm_asahi_vm_destroy destroy={.vm_id=v};call(fd,DRM_IOCTL_ASAHI_VM_DESTROY,&destroy,"mapped VM destruction");
    struct drm_asahi_queue_create live={.vm_id=v2,.usc_exec_base=1ULL<<40};
    call(fd,DRM_IOCTL_ASAHI_QUEUE_CREATE,&live,"queue retaining VM");
    destroy.vm_id=v2;call(fd,DRM_IOCTL_ASAHI_VM_DESTROY,&destroy,"destroy VM handle with live queue");
    check(ioctl(fd,DRM_IOCTL_ASAHI_VM_DESTROY,&destroy)==-1 && errno==ENOENT,"VM handle removed");
    struct drm_asahi_queue_create gone={.vm_id=v2,.usc_exec_base=1ULL<<40};
    check(ioctl(fd,DRM_IOCTL_ASAHI_QUEUE_CREATE,&gone)==-1 && errno==ENOENT,"new queue cannot use removed VM");
    struct drm_asahi_queue_destroy retire={.queue_id=live.queue_id};
    call(fd,DRM_IOCTL_ASAHI_QUEUE_DESTROY,&retire,"last queue releases retained VM");
    check(a[0]==0x1122334455667788ULL,"GEM survives VM destruction");
    munmap(a,0x8000);munmap(b,0x8000);munmap(alias,0x8000);close(prime.fd);close(peer);close(fd);
    printf("UAPI_OPTIONS_PASS checks=%u\n",checks);return 0;
}
