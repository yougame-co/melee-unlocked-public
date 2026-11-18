/* ENet backend for the app-owned ring; Win32's original socket implementation
 * remains the fallback. One ENet host owns the run's single-consumer in ring.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "enet/yg_enet_socket.h"
#include <limits.h>
#include <string.h>
#include <stdio.h>
#ifdef _WIN32
#include <windows.h>
static SRWLOCK mutex = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&mutex)
#define UNLOCK() ReleaseSRWLockExclusive(&mutex)
static uint32_t load32(const void *p) { return (uint32_t)InterlockedCompareExchange((volatile LONG *)p,0,0); }
static unsigned load8(const void *p) { uintptr_t a=(uintptr_t)p; return (load32((const void *)(a & ~(uintptr_t)3)) >> (8*(a&3))) & 255; }
static void store32(void *p,uint32_t n) { InterlockedExchange((volatile LONG *)p,(LONG)n); }
static uint64_t clock_ms(void) { return GetTickCount64(); }
#else
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&mutex)
#define UNLOCK() pthread_mutex_unlock(&mutex)
static uint32_t load32(const void *p) { return __atomic_load_n((const uint32_t *)p,__ATOMIC_ACQUIRE); }
static unsigned load8(const void *p) { return __atomic_load_n((const unsigned char *)p,__ATOMIC_ACQUIRE); }
static void store32(void *p,uint32_t n) { __atomic_store_n((uint32_t *)p,n,__ATOMIC_RELEASE); }
static uint64_t clock_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000; }
#endif
/* Equivalent monotonic source to steady_clock, independent of ENet timeout clocks.
 * Calls are under the existing ring mutex, including frequency initialization. */
static uint64_t mirror_clock_ms(void) {
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    LARGE_INTEGER ticks;
    if(!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    if(!frequency.QuadPart || !QueryPerformanceCounter(&ticks)) return clock_ms();
    return (uint64_t)(ticks.QuadPart/frequency.QuadPart)*1000 +
        (uint64_t)(ticks.QuadPart%frequency.QuadPart)*1000/(uint64_t)frequency.QuadPart;
#else
    return clock_ms();
#endif
}
static unsigned char *mem;
static uint32_t tx, rx; /* Private counters, never read our own published fields. */
static int selected, enabled, broken, shutdown_logged;
static YGDoorbells bells;
static ENetSocket current = ENET_SOCKET_NULL;
static unsigned serial;
static YGNetMirror mirror;
void yg_enet_mirror_active(int active) {
    LOCK(); yg_mirror_reset(&mirror,mirror_clock_ms(),active); UNLOCK();
}
int yg_enet_mirror_service(unsigned ping,YGNetMirror *window,uint64_t *elapsed) {
    int ready=0; uint64_t now; LOCK(); now=mirror_clock_ms();
    if(enabled && mirror.active) {
        yg_mirror_service(&mirror,now,ping);
        if(now-mirror.start_ms>=10000) {
            *window=mirror; *elapsed=now-mirror.start_ms;
            yg_mirror_reset(&mirror,now,1);
            mirror.last_service_ms=now;
            ready=1;
        }
    }
    UNLOCK(); return ready;
}
static enet_uint16 local_port;
static unsigned get16(const unsigned char *p) { return p[0] | ((unsigned)p[1]<<8); }
static void put16(unsigned char *p,unsigned n) { p[0]=(unsigned char)n; p[1]=(unsigned char)(n>>8); }
/* Called under the ring mutex on every OS. Logging never changes ring state. */
static int live(void) {
    if(!mem || broken) return 0;
    if(load32(mem+28)) {
        if(!shutdown_logged) fprintf(stderr,"yougame: direct link helper set the shutdown flag; direct link down\n");
        shutdown_logged=1;
        return 0;
    }
    return 1;
}
static int state(unsigned link) {
    unsigned i;
    if (!link || link>65534 || !live()) return 3;
    for (i=0;i<3;i++) {
        unsigned s=load8(mem+38+i); /* acquire before link id/address */
        if (s && get16(mem+32+2*i)==link) return (int)s;
    }
    return 0;
}
/* Addresses are fixed for the life of a link, including across room recovery. */
static void address_for(unsigned link,ENetAddress *a) {
    a->host=ENET_HOST_TO_NET_32(0x0aff0000u|link); a->port=7777;
}
/* Room relay backend (header). Same mutex as the ring; every relay branch is taken only while
 * relay_on, so the ring code below runs exactly as before when the relay is off. */
static int relay_on, relay_woken;
static unsigned relay_links, relay_head, relay_count, relay_drops;
static ENetSocket relay_socket = ENET_SOCKET_NULL; /* created while the relay was on */
static YGRelaySend relay_send;
static struct { unsigned short link, len; unsigned char data[YG_DATAGRAM]; } relay_q[YG_SLOTS];
#ifdef _WIN32
static CONDITION_VARIABLE relay_cv = CONDITION_VARIABLE_INIT;
static void relay_signal(void) { WakeAllConditionVariable(&relay_cv); }
static void relay_sleep(uint64_t ms) { SleepConditionVariableSRW(&relay_cv,&mutex,(DWORD)(ms>100?100:ms),0); }
#else
static pthread_cond_t relay_cv = PTHREAD_COND_INITIALIZER;
static void relay_signal(void) { pthread_cond_broadcast(&relay_cv); }
/* CLOCK_REALTIME deadline in short slices; the caller re-checks its monotonic budget. */
static void relay_sleep(uint64_t ms) {
    struct timespec t;
    if(ms>100) ms=100;
    clock_gettime(CLOCK_REALTIME,&t);
    t.tv_sec+=(time_t)(ms/1000); t.tv_nsec+=(long)(ms%1000)*1000000L;
    if(t.tv_nsec>=1000000000L) { t.tv_sec++; t.tv_nsec-=1000000000L; }
    pthread_cond_timedwait(&relay_cv,&mutex,&t);
}
#endif
static int relay_link(unsigned link) { return relay_on && link>=YG_RELAY_LINK0 && link<YG_RELAY_LINK0+relay_links; }
void yg_enet_relay_callback(YGRelaySend f) { LOCK(); relay_send=f; UNLOCK(); }
int yg_enet_relay_begin(unsigned n) {
    if(!n || n>YG_RELAY_MAX) return -1;
    LOCK();
    if(!relay_on) { relay_head=relay_count=0; relay_woken=0; relay_drops=0; }
    relay_on=1; relay_links=n;
    UNLOCK(); return 0;
}
void yg_enet_relay_end(void) {
    LOCK();
    if(relay_on) {
        relay_on=0; relay_links=0; relay_head=relay_count=0; relay_woken=1;
        /* A socket made for the relay never falls through to a ring that has no doorbells. */
        if(relay_socket!=ENET_SOCKET_NULL && relay_socket==current) current=ENET_SOCKET_NULL;
        relay_socket=ENET_SOCKET_NULL;
        relay_signal();
    }
    UNLOCK();
}
int yg_enet_relay_active(void) { int n; LOCK(); n=relay_on; UNLOCK(); return n; }
unsigned yg_enet_relay_dropped(void) { unsigned n; LOCK(); n=relay_drops; UNLOCK(); return n; }
int yg_enet_relay_in(unsigned link,const void *data,size_t len) {
    int ok=0; LOCK();
    if(data && relay_link(link) && len && len<=YG_DATAGRAM) {
        unsigned at;
        if(relay_count==YG_SLOTS) { relay_head=(relay_head+1)%YG_SLOTS; relay_count--; relay_drops++; } /* oldest goes */
        at=(relay_head+relay_count)%YG_SLOTS;
        relay_q[at].link=(unsigned short)link; relay_q[at].len=(unsigned short)len;
        memcpy(relay_q[at].data,data,len); relay_count++;
        relay_signal(); ok=1;
    }
    UNLOCK(); return ok?0:-1;
}
/* Called locked; returns unlocked. The callback runs outside the lock. */
static int relay_out(const ENetAddress *a,const ENetBuffer *buffers,size_t count,size_t total) {
    unsigned char buf[YG_DATAGRAM]; size_t i,pos=0; unsigned link=0; YGRelaySend f=relay_send;
    if(a && a->host==ENET_HOST_TO_NET_32(0x7f000001) && total==1 && count==1 &&
       *(const unsigned char *)buffers[0].data==0) { relay_woken=1; relay_signal(); UNLOCK(); return 1; }
    if(a && a->port==7777 && (ENET_NET_TO_HOST_32(a->host)&0xffff0000u)==0x0aff0000u)
        link=ENET_NET_TO_HOST_32(a->host)&65535;
    if(!enabled || !total || total>YG_DATAGRAM || !relay_link(link) || !f) { UNLOCK(); return (int)total; }
    UNLOCK();
    for(i=0;i<count;i++) { memcpy(buf+pos,buffers[i].data,buffers[i].dataLength); pos+=buffers[i].dataLength; }
    f(link,buf,total);
    return (int)total;
}
/* Called locked; returns unlocked. */
static int relay_take(ENetSocket s,ENetAddress *a,ENetBuffer *buffers,size_t count) {
    int result=0;
    if(s!=current || !enabled) { UNLOCK(); return 0; }
    while(relay_count && !result) {
        unsigned len=relay_q[relay_head].len, link=relay_q[relay_head].link; size_t i,capacity=0,pos=0;
        const unsigned char *data=relay_q[relay_head].data;
        relay_head=(relay_head+1)%YG_SLOTS; relay_count--;
        for(i=0;i<count;i++) { if(buffers[i].dataLength>YG_DATAGRAM) capacity=YG_DATAGRAM; else if(capacity<YG_DATAGRAM) capacity+=buffers[i].dataLength; }
        if(len<=capacity && relay_link(link)) {
            for(i=0;i<count && pos<len;i++) { size_t n=buffers[i].dataLength; if(n>len-pos)n=len-pos; memcpy(buffers[i].data,data+pos,n); pos+=n; }
            if(a) address_for(link,a);
            result=(int)len;
        }
    }
    UNLOCK(); return result;
}
static int attach_locked(void *memory,size_t size,YGDoorbells b) {
    unsigned char *m=(unsigned char *)memory;
    int ok=0;
    if (current==ENET_SOCKET_NULL && m && size==YG_SECTION_SIZE &&
        !((uintptr_t)m & 3) && !memcmp(m,"YGDL",4) && load32(m+4)==1 &&
        load32(m+8)==YG_SLOTS && load32(m+12)==YG_STRIDE &&
        load32(m+16)==YG_DATAGRAM && load32(m+20)==YG_OUT && load32(m+24)==YG_IN &&
        b.notify && b.wake && b.wait) {
        mem=m; bells=b; tx=rx=0; broken=0; shutdown_logged=0; enabled=0; ok=1;
    }
    return ok ? 0 : -1;
}
int yg_enet_attach(void *memory,size_t size,YGDoorbells b) {
    int rc; LOCK(); rc=attach_locked(memory,size,b); UNLOCK(); return rc;
}
void yg_enet_select(int on) { LOCK(); selected=on; UNLOCK(); }
int yg_enet_selected(void) { int n; LOCK(); n=selected; UNLOCK(); return n; }
void yg_enet_enable(int on) { LOCK(); enabled=on; UNLOCK(); }
int yg_enet_channel_ready(void) { int n; LOCK(); n=live(); UNLOCK(); return n; }
int yg_enet_enabled(void) {
    int n; LOCK(); n=enabled;
#ifndef _WIN32
    n=n && (relay_on || live());
#endif
    UNLOCK(); return n;
}
int yg_enet_link_state(unsigned link) { int n; LOCK(); n=relay_link(link)?2:state(link); UNLOCK(); return n; }
int yg_enet_link_address(unsigned link,ENetAddress *a) {
    int ok=0; unsigned i; LOCK();
    if (a && relay_link(link)) { address_for(link,a); ok=1; }
    else if (a && state(link)==2) {
        for(i=0;i<3;i++) if(get16(mem+32+2*i)==link) {
            ENetAddress expected; address_for(link,&expected);
            /* Validate P2's header metadata, rather than using arbitrary destinations. */
            if (!memcmp(mem+44+4*i,&expected.host,4) && get16(mem+56+2*i)==7777) { *a=expected; ok=1; }
            break;
        }
    }
    UNLOCK(); return ok ? 0 : -1;
}
/* Opaque negative ids on POSIX, top-of-range ids on Win64: never OS handles.
 * Retired ids remain recognizable, so a stale operation never enters Winsock. */
int yg_enet_is_socket(ENetSocket s) { return s!=ENET_SOCKET_NULL && (uintptr_t)s >= (uintptr_t)(ENetSocket)-1048576; }
unsigned yg_enet_mtu(ENetSocket s,unsigned ordinary) { return yg_enet_is_socket(s) ? (ordinary<YG_DATAGRAM?ordinary:YG_DATAGRAM) : ordinary; }
ENetSocket yg_enet_create(void) {
    ENetSocket s=ENET_SOCKET_NULL; LOCK();
    if(enabled && (relay_on || live()) && current==ENET_SOCKET_NULL && serial<1048574) {
        current=(ENetSocket)(-(intptr_t)(++serial+1)); local_port=7777; s=current;
        if(relay_on) relay_socket=s;
    }
    UNLOCK(); return s;
}
void yg_enet_destroy(ENetSocket s) { LOCK(); if(s==current) current=ENET_SOCKET_NULL; if(s==relay_socket) relay_socket=ENET_SOCKET_NULL; UNLOCK(); }
int yg_enet_bind(ENetSocket s,const ENetAddress *a) { int ok; LOCK(); ok=s==current; if(ok && a) local_port=a->port; UNLOCK(); return ok?0:-1; }
int yg_enet_get_address(ENetSocket s,ENetAddress *a) {
    int ok; LOCK(); ok=s==current && a;
    if(ok) { a->host=ENET_HOST_TO_NET_32(0x0afe0001); a->port=local_port; }
    UNLOCK(); return ok?0:-1;
}
int yg_enet_send(ENetSocket s,const ENetAddress *a,const ENetBuffer *buffers,size_t count) {
    size_t i,total=0,pos=0; unsigned link=0; unsigned char *slot;
    for(i=0;i<count;i++) { if(buffers[i].dataLength>(size_t)INT_MAX-total) return -1; total+=buffers[i].dataLength; }
    LOCK();
    if(s!=current) { UNLOCK(); return (int)total; }
    if(relay_on) return relay_out(a,buffers,count,total);
#ifndef _WIN32
    if(!live()) { UNLOCK(); return -1; }
#endif
    /* Slippi's local service wake is never put into the shared SPSC out ring. */
    if(a && a->host==ENET_HOST_TO_NET_32(0x7f000001) && total==1 && count==1 &&
       *(const unsigned char *)buffers[0].data==0) { bells.wake(bells.context); UNLOCK(); return 1; }
    if(a && a->port==7777 && (ENET_NET_TO_HOST_32(a->host)&0xffff0000u)==0x0aff0000u)
        link=ENET_NET_TO_HOST_32(a->host)&65535;
    if(!enabled || !total || total>YG_DATAGRAM || state(link)!=2) { UNLOCK(); return (int)total; }
    {
        uint32_t fill=tx-load32(mem+YG_OUT+64);
        if(fill>YG_SLOTS) broken=1;
        if(fill>=YG_SLOTS) { UNLOCK(); return (int)total; } /* UDP loss, full length even on drop */
    }
    slot=mem+YG_OUT+128+(tx%YG_SLOTS)*YG_STRIDE;
    put16(slot,(unsigned)total); put16(slot+2,link); memset(slot+4,0,4);
    for(i=0;i<count;i++) { memcpy(slot+8+pos,buffers[i].data,buffers[i].dataLength); pos+=buffers[i].dataLength; }
    if(mirror.active) {
        ++mirror.out_batch;
        if(mirror.out_batch>mirror.out_batch_max) mirror.out_batch_max=mirror.out_batch;
    }
    store32(mem+YG_OUT,++tx); bells.notify(bells.context);
#ifndef _WIN32
    if(!live()) { UNLOCK(); return -1; }
#endif
    UNLOCK(); return (int)total;
}
int yg_enet_receive(ENetSocket s,ENetAddress *a,ENetBuffer *buffers,size_t count) {
    unsigned budget; int result=0; LOCK();
    if(relay_on) return relay_take(s,a,buffers,count);
#ifndef _WIN32
    if(s==current && !live()) { UNLOCK(); return -1; }
#endif
    if(s!=current || !enabled || !live()) { UNLOCK(); return 0; }
    for(budget=0;budget<YG_SLOTS;budget++) {
        uint32_t fill=load32(mem+YG_IN)-rx;
        unsigned char hdr[8],*slot; unsigned len,link; size_t i,capacity=0,pos=0;
        if(fill>YG_SLOTS) { broken=1; break; }
        if(!fill) break;
        yg_mirror_read(&mirror,fill,rx,mirror_clock_ms());
        slot=mem+YG_IN+128+(rx%YG_SLOTS)*YG_STRIDE; memcpy(hdr,slot,8);
        len=get16(hdr); link=get16(hdr+2);
        for(i=0;i<count;i++) { if(buffers[i].dataLength>YG_DATAGRAM) capacity=YG_DATAGRAM; else if(capacity<YG_DATAGRAM) capacity+=buffers[i].dataLength; }
        if(len && len<=YG_DATAGRAM && len<=capacity && state(link)==2) {
            for(i=0;i<count && pos<len;i++) { size_t n=buffers[i].dataLength; if(n>len-pos)n=len-pos; memcpy(buffers[i].data,slot+8+pos,n); pos+=n; }
            if(a) address_for(link,a);
            result=(int)len;
        }
        store32(mem+YG_IN+64,++rx);
        if(result) break; /* malformed/unknown slots are dropped without a persistent -1 spin */
    }
    UNLOCK(); return result;
}
int yg_enet_wait(ENetSocket s,enet_uint32 *condition,enet_uint32 timeout) {
    const uint64_t until=clock_ms()+timeout;
    const enet_uint32 wanted=*condition;
    *condition=ENET_SOCKET_WAIT_NONE;
    for(;;) {
        int n; uint64_t now; YGDoorbells b;
        LOCK();
        if(s!=current) { UNLOCK(); return -1; }
        if(relay_on) {
            if((wanted&ENET_SOCKET_WAIT_RECEIVE) && enabled && relay_count) { *condition=ENET_SOCKET_WAIT_RECEIVE; UNLOCK(); return 0; }
            if(wanted&ENET_SOCKET_WAIT_SEND) { *condition=ENET_SOCKET_WAIT_SEND; UNLOCK(); return 0; }
            if(relay_woken) { relay_woken=0; UNLOCK(); return 0; } /* local wake: NONE, as on the ring */
            now=clock_ms();
            if(now>=until) { UNLOCK(); return 0; }
            relay_sleep(until-now);
            UNLOCK(); continue;
        }
#ifndef _WIN32
        if(!live()) { UNLOCK(); return -1; }
#endif
        if((wanted&ENET_SOCKET_WAIT_RECEIVE) && live() && enabled && load32(mem+YG_IN)!=rx) {
            *condition=ENET_SOCKET_WAIT_RECEIVE; UNLOCK(); return 0;
        }
        if(wanted&ENET_SOCKET_WAIT_SEND) { *condition=ENET_SOCKET_WAIT_SEND; UNLOCK(); return 0; }
        b=bells; UNLOCK(); now=clock_ms();
        n=b.wait(b.context,now>=until?0:(enet_uint32)(until-now));
        if(n==1) {
            LOCK();
            if(mirror.active && live()) {
                /* Keep the oldest observed notification until its published slots are read. */
                if(!mirror.notified || (int32_t)(mirror.notified_until-rx)<=0) {
                    mirror.notified=1; mirror.notified_ms=mirror_clock_ms();
                    mirror.notified_until=load32(mem+YG_IN);
                }
            }
            UNLOCK();
        }
        if(n==2 || n==0) return 0; /* local wake returns NONE: ENet must flush queued sends now */
        if(n<0) return -1;
        if(clock_ms()>=until) return 0;
        /* Empty doorbells and INTERRUPT re-wait with the original remaining budget. */
    }
}
#ifndef _WIN32
static struct {
    int section, wake, wait, self[2];
    void *view;
} posix = {-1,-1,-1,{-1,-1},NULL};

/* Called with the ring mutex held; callbacks notify/wake already hold it. */
static void posix_down_locked(void) {
    if(!broken) fprintf(stderr,"yougame: POSIX ring helper unavailable; direct link down\n");
    broken=1; enabled=0;
}
static int posix_down(void) {
    LOCK(); posix_down_locked(); UNLOCK(); return -1;
}
static int nonblock_cloexec(int fd) {
    int flags=fcntl(fd,F_GETFL), desc=fcntl(fd,F_GETFD);
    return flags<0 || desc<0 || fcntl(fd,F_SETFL,flags|O_NONBLOCK)<0 ||
        fcntl(fd,F_SETFD,desc|FD_CLOEXEC)<0 ? -1 : 0;
}
static void pipe_byte(int fd) {
    unsigned char byte=1; ssize_t n;
    do { n=write(fd,&byte,1); } while(n<0 && errno==EINTR);
    /* A full pipe already has a pending notification. SIGPIPE is ignored at attach. */
    if(n!=1 && !(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK))) posix_down_locked();
}
static void posix_notify(void *p) { (void)p; pipe_byte(posix.wake); }
static void posix_wake(void *p) { (void)p; pipe_byte(posix.self[1]); }
static int drain_pipe(int fd) {
    unsigned char bytes[256]; ssize_t n;
    for(;;) {
        n=read(fd,bytes,sizeof bytes);
        if(n>0 || (n<0 && errno==EINTR)) continue;
        if(n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) return 0;
        return -1; /* EOF or read error: helper is gone. */
    }
}
static int posix_wait(void *p,enet_uint32 timeout) {
    struct pollfd fds[2]={{posix.wait,POLLIN,0},{posix.self[0],POLLIN,0}};
    uint64_t until=clock_ms()+timeout, now; int n, local=0, failed=0; (void)p;
    for(;;) {
        now=clock_ms();
        n=poll(fds,2,now>=until?0:(until-now>INT_MAX?INT_MAX:(int)(until-now)));
        if(n<0 && errno==EINTR) continue; /* original deadline, even under repeated signals */
        if(n<0) return posix_down();
        if(!n) { if(clock_ms()<until) continue; return 0; }
        /* Drain both, including on HUP: a final byte does not hide helper death. */
        if(fds[0].revents && drain_pipe(posix.wait)) failed=1;
        if(fds[1].revents) {
            if(drain_pipe(posix.self[0])) failed=1;
            local=1;
        }
        if((fds[0].revents|fds[1].revents)&(POLLERR|POLLNVAL)) failed=1;
        if(failed) return posix_down();
        return local?2:1;
    }
}
int yg_enet_attach_posix(int section,int wake,int wait,size_t size) {
    struct stat st; int flags, self[2]={-1,-1}, rc=-1;
    void *view=MAP_FAILED;
    struct sigaction ignore;
    YGDoorbells b={NULL,posix_notify,posix_wake,posix_wait};
    LOCK();
    if(posix.view && posix.section==section) { rc=live()?0:-1; UNLOCK(); return rc; }
    if(current!=ENET_SOCKET_NULL || size!=YG_SECTION_SIZE ||
       section<3 || wake<3 || wait<3 || section==wake || section==wait || wake==wait) goto fail;
    if(fstat(section,&st) || !S_ISREG(st.st_mode) || st.st_size<(off_t)size) goto fail;
    if(fstat(wake,&st) || !S_ISFIFO(st.st_mode) ||
       (flags=fcntl(wake,F_GETFL))<0 || (flags&O_ACCMODE)!=O_WRONLY) goto fail;
    if(fstat(wait,&st) || !S_ISFIFO(st.st_mode) ||
       (flags=fcntl(wait,F_GETFL))<0 || (flags&O_ACCMODE)!=O_RDONLY) goto fail;
    view=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,section,0);
    if(view==MAP_FAILED || pipe(self)) goto fail;
    if(nonblock_cloexec(section) || nonblock_cloexec(wake) || nonblock_cloexec(wait) ||
       nonblock_cloexec(self[0]) || nonblock_cloexec(self[1])) goto fail;
    /* Pipes have no portable per-write NOSIGNAL flag. This run-wide disposition
     * also protects writes from ENet service threads created after attachment. */
    memset(&ignore,0,sizeof ignore); ignore.sa_handler=SIG_IGN; sigemptyset(&ignore.sa_mask);
    if(sigaction(SIGPIPE,&ignore,NULL) || attach_locked(view,size,b)) goto fail;
    if(posix.view) {
        munmap(posix.view,YG_SECTION_SIZE); close(posix.self[0]); close(posix.self[1]);
    }
    posix.section=section; posix.wake=wake; posix.wait=wait;
    posix.self[0]=self[0]; posix.self[1]=self[1]; posix.view=view;
    UNLOCK(); return 0;
fail:
    if(view!=MAP_FAILED) munmap(view,size);
    if(self[0]>=0) close(self[0]);
    if(self[1]>=0) close(self[1]);
    /* Inherited descriptors are borrowed, including on partial failure. Never
     * close an app-selected number that might belong to another subsystem. */
    fprintf(stderr,"yougame: POSIX ring attach refused (fds, size, mapping or header)\n");
    UNLOCK(); return -1;
}
#endif
/* Own UDP handoff (yg_native_park in the header). Shared by win32.c's wrapper below and unix.c. */
#ifdef _WIN32
#define YG_TLS __declspec(thread)
#else
#define YG_TLS __thread
#endif
static ENetSocket parked=ENET_SOCKET_NULL; static unsigned short parked_port=0;
static YG_TLS unsigned short adopt_port=0; static YG_TLS ENetSocket adopted=ENET_SOCKET_NULL; static YG_TLS unsigned short adopted_port=0;
ENetSocket yg_native_park(ENetSocket s,unsigned short port) {
    ENetSocket old; LOCK(); old=parked; parked=s; parked_port=s==ENET_SOCKET_NULL?0:port; UNLOCK();
    return old==s?ENET_SOCKET_NULL:old;
}
void yg_native_adopt_next(unsigned short port) { adopt_port=port; }
ENetSocket yg_native_take_parked(ENetSocketType t) {
    ENetSocket s=ENET_SOCKET_NULL; unsigned short want=adopt_port;
    adopt_port=0;
    if(!want || t!=ENET_SOCKET_TYPE_DATAGRAM) return ENET_SOCKET_NULL;
    LOCK(); if(parked!=ENET_SOCKET_NULL && parked_port==want) { s=parked; parked=ENET_SOCKET_NULL; parked_port=0; } UNLOCK();
    if(s!=ENET_SOCKET_NULL) { adopted=s; adopted_port=want; }
    return s;
}
int yg_native_adopted_bind(ENetSocket s,const ENetAddress *a) {
    int ok;
    if(adopted==ENET_SOCKET_NULL || s!=adopted) return 0;
    ok=a && a->host==ENET_HOST_ANY && a->port==adopted_port;   /* already bound there by the punch */
    adopted=ENET_SOCKET_NULL; adopted_port=0;
    return ok;
}
#ifdef _WIN32
static struct { HANDLE section,to_app,to_game,wake; void *view; } handles;
static void notify_app(void *p) { (void)p; SetEvent(handles.to_app); }
static void wake_local(void *p) { (void)p; SetEvent(handles.wake); }
static int wait_handles(void *p,enet_uint32 timeout) {
    HANDLE h[2]={handles.wake,handles.to_game}; DWORD rc; (void)p;
    rc=WaitForMultipleObjectsEx(2,h,FALSE,timeout,TRUE);
    if(rc==WAIT_OBJECT_0) return 2;
    if(rc==WAIT_OBJECT_0+1) return 1;
    if(rc==WAIT_TIMEOUT) return 0;
    if(rc==WAIT_IO_COMPLETION) return 3;
    return -1;
}
int yg_enet_attach_handles(uintptr_t section,uintptr_t toApp,uintptr_t toGame) {
    void *view; HANDLE wake; YGDoorbells b={NULL,notify_app,wake_local,wait_handles};
    int rc;
    LOCK();
    if(handles.view && handles.section==(HANDLE)section) { rc=live()?0:-1; UNLOCK(); return rc; }
    if(current!=ENET_SOCKET_NULL) { UNLOCK(); return -1; }
    view=MapViewOfFile((HANDLE)section,FILE_MAP_READ|FILE_MAP_WRITE,0,0,YG_SECTION_SIZE);
    if(!view) { UNLOCK(); return -1; }
    wake=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(!wake || attach_locked(view,YG_SECTION_SIZE,b)) { if(wake) CloseHandle(wake); UnmapViewOfFile(view); UNLOCK(); return -1; }
    if(handles.view) { UnmapViewOfFile(handles.view); CloseHandle(handles.section); CloseHandle(handles.to_app); CloseHandle(handles.to_game); CloseHandle(handles.wake); }
    handles.view=view; handles.section=(HANDLE)section; handles.to_app=(HANDLE)toApp; handles.to_game=(HANDLE)toGame; handles.wake=wake;
    UNLOCK(); return 0; /* Run lifetime; only replaced after every old ENet socket is destroyed. */
}

/* The original win32.c exports these private fallbacks. */
ENetSocket yg_native_socket_create(ENetSocketType);
void yg_native_socket_destroy(ENetSocket);
int yg_native_socket_bind(ENetSocket,const ENetAddress *);
int yg_native_socket_get_address(ENetSocket,ENetAddress *);
int yg_native_socket_set_option(ENetSocket,ENetSocketOption,int);
int yg_native_socket_get_option(ENetSocket,ENetSocketOption,int *);
int yg_native_socket_send(ENetSocket,const ENetAddress *,const ENetBuffer *,size_t);
int yg_native_socket_receive(ENetSocket,ENetAddress *,ENetBuffer *,size_t);
int yg_native_socket_wait(ENetSocket,enet_uint32 *,enet_uint32);
int yg_native_socket_listen(ENetSocket,int);
int yg_native_socket_connect(ENetSocket,const ENetAddress *);
ENetSocket yg_native_socket_accept(ENetSocket,ENetAddress *);
int yg_native_socket_shutdown(ENetSocket,ENetSocketShutdown);
int yg_native_socketset_select(ENetSocket,ENetSocketSet *,ENetSocketSet *,enet_uint32);
ENetSocket enet_socket_create(ENetSocketType t) {
    ENetSocket s;
    if(yg_enet_selected()) { yg_native_adopt_next(0); return t==ENET_SOCKET_TYPE_DATAGRAM?yg_enet_create():ENET_SOCKET_NULL; }
    s=yg_native_take_parked(t);   /* the own-UDP punch socket (yg_native_park) */
    return s!=ENET_SOCKET_NULL?s:yg_native_socket_create(t);
}
void enet_socket_destroy(ENetSocket s) { if(yg_enet_is_socket(s)) yg_enet_destroy(s); else yg_native_socket_destroy(s); }
int enet_socket_bind(ENetSocket s,const ENetAddress *a) {
    if(yg_enet_is_socket(s)) return yg_enet_bind(s,a);
    if(yg_native_adopted_bind(s,a)) return 0;
    return yg_native_socket_bind(s,a);
}
int enet_socket_get_address(ENetSocket s,ENetAddress *a) { return yg_enet_is_socket(s)?yg_enet_get_address(s,a):yg_native_socket_get_address(s,a); }
int enet_socket_set_option(ENetSocket s,ENetSocketOption o,int v) { return yg_enet_is_socket(s)?0:yg_native_socket_set_option(s,o,v); }
int enet_socket_get_option(ENetSocket s,ENetSocketOption o,int *v) { if(yg_enet_is_socket(s)) { *v=0; return 0; } return yg_native_socket_get_option(s,o,v); }
int enet_socket_send(ENetSocket s,const ENetAddress *a,const ENetBuffer *b,size_t n) { return yg_enet_is_socket(s)?yg_enet_send(s,a,b,n):yg_native_socket_send(s,a,b,n); }
int enet_socket_receive(ENetSocket s,ENetAddress *a,ENetBuffer *b,size_t n) { return yg_enet_is_socket(s)?yg_enet_receive(s,a,b,n):yg_native_socket_receive(s,a,b,n); }
int enet_socket_wait(ENetSocket s,enet_uint32 *c,enet_uint32 t) { return yg_enet_is_socket(s)?yg_enet_wait(s,c,t):yg_native_socket_wait(s,c,t); }
int enet_socket_listen(ENetSocket s,int n) { return yg_enet_is_socket(s)?-1:yg_native_socket_listen(s,n); }
int enet_socket_connect(ENetSocket s,const ENetAddress *a) { return yg_enet_is_socket(s)?-1:yg_native_socket_connect(s,a); }
ENetSocket enet_socket_accept(ENetSocket s,ENetAddress *a) { return yg_enet_is_socket(s)?ENET_SOCKET_NULL:yg_native_socket_accept(s,a); }
int enet_socket_shutdown(ENetSocket s,ENetSocketShutdown h) { return yg_enet_is_socket(s)?0:yg_native_socket_shutdown(s,h); }
int enet_socketset_select(ENetSocket m,ENetSocketSet *r,ENetSocketSet *w,enet_uint32 t) {
    unsigned i; if(r) for(i=0;i<r->fd_count;i++) if(yg_enet_is_socket(r->fd_array[i])) return -1;
    if(w) for(i=0;i<w->fd_count;i++) if(yg_enet_is_socket(w->fd_array[i])) return -1;
    return yg_native_socketset_select(m,r,w,t);
}
#endif
