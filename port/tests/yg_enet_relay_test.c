/* Room relay backend of the virtual ENet socket: no OS sockets, a fake ring for the off case.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#include "enet/yg_enet_socket.h"
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static unsigned char memory[YG_SECTION_SIZE];
static unsigned notified, sent, sent_link, sent_len, wake_pending;
static unsigned char sent_data[YG_DATAGRAM];
static void put16(void *v,unsigned n) { unsigned char *p=v; p[0]=(unsigned char)n; p[1]=(unsigned char)(n>>8); }
static void put32(void *v,uint32_t n) { __atomic_store_n((uint32_t *)v,n,__ATOMIC_RELEASE); }
static uint32_t get32(const void *v) { return __atomic_load_n((const uint32_t *)v,__ATOMIC_ACQUIRE); }
static void notify(void *p) { (void)p; notified++; }
static void wake(void *p) { (void)p; wake_pending=1; }
static int wait_fake(void *p,enet_uint32 timeout) {
    struct timespec d={(time_t)(timeout/1000),(long)(timeout%1000)*1000000}; (void)p;
    if(wake_pending) { wake_pending=0; return 2; }
    while(nanosleep(&d,&d) && errno==EINTR) {}
    return 0;
}
static void relay_send(unsigned link,const void *data,size_t len) {
    sent++; sent_link=link; sent_len=(unsigned)len; memcpy(sent_data,data,len);
}
static uint64_t now_ms(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t);
    return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;
}
static void attach_ring(void) {
    YGDoorbells b={NULL,notify,wake,wait_fake};
    memset(memory,0,sizeof memory); memcpy(memory,"YGDL",4); put32(memory+4,1);
    put32(memory+8,YG_SLOTS); put32(memory+12,YG_STRIDE); put32(memory+16,YG_DATAGRAM);
    put32(memory+20,YG_OUT); put32(memory+24,YG_IN);
    put16(memory+32,1); memory[38]=2;
    memory[44]=10; memory[45]=255; memory[46]=0; memory[47]=1; put16(memory+56,7777);
    CHECK(yg_enet_attach(memory,sizeof memory,b)==0);
}
static int send_to(ENetSocket s,unsigned link,const char *text) {
    ENetAddress a; ENetBuffer b;
    a.host=ENET_HOST_TO_NET_32(0x0aff0000u|link); a.port=7777;
    b.data=(void *)text; b.dataLength=strlen(text);
    return yg_enet_send(s,&a,&b,1);
}
static int receive(ENetSocket s,ENetAddress *a,char *out,size_t cap) {
    ENetBuffer b; int n; b.data=out; b.dataLength=cap;
    n=yg_enet_receive(s,a,&b,1);
    if(n>0 && (size_t)n<cap) out[n]=0;
    return n;
}
static void *late_datagram(void *p) {
    struct timespec d={0,30*1000000}; (void)p;
    while(nanosleep(&d,&d) && errno==EINTR) {}
    yg_enet_relay_in(YG_RELAY_LINK0,"late",4);
    return NULL;
}
/* The no_direct case: no ring was ever attached, the relay carries everything. */
static void relay_without_ring(void) {
    ENetSocket s; ENetAddress a; char out[YG_DATAGRAM+1]; enet_uint32 cond; uint64_t t0; pthread_t th; unsigned i;
    yg_enet_relay_callback(relay_send);
    yg_enet_select(1); yg_enet_enable(1);
    CHECK(yg_enet_create()==ENET_SOCKET_NULL);          /* no ring, no relay: no socket */
    CHECK(yg_enet_relay_in(YG_RELAY_LINK0,"x",1)==-1);  /* relay off: nothing queued */
    CHECK(yg_enet_relay_begin(0)==-1 && yg_enet_relay_begin(YG_RELAY_MAX+1)==-1);
    CHECK(yg_enet_relay_begin(2)==0 && yg_enet_relay_active());
    CHECK(yg_enet_link_state(YG_RELAY_LINK0)==2 && yg_enet_link_state(YG_RELAY_LINK0+1)==2);
    CHECK(yg_enet_link_state(YG_RELAY_LINK0+2)!=2 && yg_enet_link_state(1)!=2);
    CHECK(yg_enet_link_address(YG_RELAY_LINK0+1,&a)==0 && a.port==7777 &&
          ENET_NET_TO_HOST_32(a.host)==(0x0aff0000u|(YG_RELAY_LINK0+1)));
    CHECK(yg_enet_link_address(YG_RELAY_LINK0+2,&a)==-1);
    s=yg_enet_create(); CHECK(s!=ENET_SOCKET_NULL && yg_enet_is_socket(s));
    CHECK(yg_enet_enabled());
    /* send reaches the callback, with its link */
    CHECK(send_to(s,YG_RELAY_LINK0+1,"hello")==5);
    CHECK(sent==1 && sent_link==YG_RELAY_LINK0+1 && sent_len==5 && !memcmp(sent_data,"hello",5));
    CHECK(send_to(s,YG_RELAY_LINK0+5,"nobody")==6 && sent==1); /* unknown link: UDP-style drop */
    CHECK(send_to(s,1,"ring")==4 && sent==1);
    /* relay_in reaches receive, with the sender's link address */
    CHECK(receive(s,&a,out,sizeof out)==0);
    CHECK(yg_enet_relay_in(YG_RELAY_LINK0+1,"pong",4)==0);
    CHECK(yg_enet_relay_in(YG_RELAY_LINK0+9,"bad",3)==-1);
    CHECK(yg_enet_relay_in(YG_RELAY_LINK0,"",0)==-1);
    cond=ENET_SOCKET_WAIT_RECEIVE; CHECK(yg_enet_wait(s,&cond,1000)==0 && cond==ENET_SOCKET_WAIT_RECEIVE);
    CHECK(receive(s,&a,out,sizeof out)==4 && !strcmp(out,"pong"));
    CHECK(ENET_NET_TO_HOST_32(a.host)==(0x0aff0000u|(YG_RELAY_LINK0+1)) && a.port==7777);
    CHECK(receive(s,&a,out,sizeof out)==0);
    /* wait times out on an empty queue */
    t0=now_ms(); cond=ENET_SOCKET_WAIT_RECEIVE;
    CHECK(yg_enet_wait(s,&cond,60)==0 && cond==ENET_SOCKET_WAIT_NONE);
    CHECK(now_ms()-t0>=50 && now_ms()-t0<1000);
    /* and wakes early for a datagram that arrives while it waits */
    CHECK(pthread_create(&th,NULL,late_datagram,NULL)==0);
    t0=now_ms(); cond=ENET_SOCKET_WAIT_RECEIVE;
    CHECK(yg_enet_wait(s,&cond,2000)==0 && cond==ENET_SOCKET_WAIT_RECEIVE && now_ms()-t0<1500);
    pthread_join(th,NULL);
    CHECK(receive(s,&a,out,sizeof out)==4 && !strcmp(out,"late"));
    /* Slippi's local wake returns NONE at once, as on the ring */
    {
        ENetAddress local; ENetBuffer b; unsigned char zero=0;
        local.host=ENET_HOST_TO_NET_32(0x7f000001); local.port=1; b.data=&zero; b.dataLength=1;
        CHECK(yg_enet_send(s,&local,&b,1)==1 && sent==1);
        t0=now_ms(); cond=ENET_SOCKET_WAIT_RECEIVE;
        CHECK(yg_enet_wait(s,&cond,2000)==0 && cond==ENET_SOCKET_WAIT_NONE && now_ms()-t0<1000);
    }
    /* a full queue drops its oldest datagram */
    for(i=0;i<YG_SLOTS+44;i++) { char d[8]; snprintf(d,sizeof d,"%u",i); CHECK(yg_enet_relay_in(YG_RELAY_LINK0,d,strlen(d))==0); }
    CHECK(yg_enet_relay_dropped()==44);
    CHECK(receive(s,&a,out,sizeof out)==2 && !strcmp(out,"44"));
    /* disabled: nothing goes out, nothing comes in */
    yg_enet_enable(0);
    CHECK(send_to(s,YG_RELAY_LINK0,"off")==3 && sent==1);
    CHECK(receive(s,&a,out,sizeof out)==0);
    yg_enet_enable(1);
    /* end: the relay socket is gone, the queue with it; no ring means no new socket */
    yg_enet_relay_end();
    CHECK(!yg_enet_relay_active() && yg_enet_link_state(YG_RELAY_LINK0)!=2);
    cond=ENET_SOCKET_WAIT_RECEIVE; CHECK(yg_enet_wait(s,&cond,10)==-1);
    CHECK(receive(s,&a,out,sizeof out)==0);
    CHECK(yg_enet_relay_in(YG_RELAY_LINK0,"x",1)==-1);
    yg_enet_destroy(s);
    CHECK(yg_enet_create()==ENET_SOCKET_NULL);
    yg_enet_enable(0);
}
/* With a ring attached: relay off leaves it exactly as before; relay on never writes it. */
static void ring_untouched(void) {
    ENetSocket s; ENetAddress a; char out[64]; unsigned before;
    attach_ring(); yg_enet_select(1); yg_enet_enable(1);
    s=yg_enet_create(); CHECK(s!=ENET_SOCKET_NULL);
    sent=0; notified=0;
    CHECK(send_to(s,1,"ring")==4 && notified==1 && get32(memory+YG_OUT)==1 && sent==0);
    CHECK(yg_enet_link_state(YG_RELAY_LINK0)==0 && yg_enet_link_address(YG_RELAY_LINK0,&a)==-1);
    CHECK(send_to(s,YG_RELAY_LINK0,"relay")==5 && get32(memory+YG_OUT)==1 && sent==0);
    CHECK(yg_enet_relay_in(YG_RELAY_LINK0,"x",1)==-1);
    /* a match that switches to the relay mid-session keeps the ring socket and leaves the ring alone */
    CHECK(!yg_enet_relay_active() && yg_enet_relay_begin(1)==0 && yg_enet_relay_active());
    before=get32(memory+YG_OUT);
    CHECK(send_to(s,YG_RELAY_LINK0,"relay")==5 && sent==1 && sent_link==YG_RELAY_LINK0);
    CHECK(send_to(s,1,"ring")==4 && get32(memory+YG_OUT)==before && notified==1 && sent==1);
    CHECK(yg_enet_relay_in(YG_RELAY_LINK0,"in",2)==0);
    CHECK(receive(s,&a,out,sizeof out)==2 && get32(memory+YG_IN+64)==0);
    /* while the relay is on it takes every send, ring links included (a relay left on from an earlier
     * match would starve a match on links): yougame_link.cpp's relay_clear_stale ends it first */
    yg_enet_relay_end();
    CHECK(!yg_enet_relay_active());
    /* the ring socket made before the relay is still the ring's */
    CHECK(send_to(s,1,"ring")==4 && get32(memory+YG_OUT)==2 && notified==2 && sent==1);
    CHECK(yg_enet_link_state(1)==2 && yg_enet_channel_ready());
    yg_enet_destroy(s);
}
int main(void) {
    relay_without_ring();
    ring_untouched();
    puts("yg_enet_relay_test: ok");
    return 0;
}
