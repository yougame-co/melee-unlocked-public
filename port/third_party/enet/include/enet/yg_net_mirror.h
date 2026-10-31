/* Fixed storage diagnostics; millisecond bins saturate at 2048, maxima do not.
 * SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef YG_NET_MIRROR_H
#define YG_NET_MIRROR_H
#include <stdint.h>
#include <string.h>
typedef struct YGMirrorHistogram {
    uint64_t bins[2049], count, max;
} YGMirrorHistogram;
static inline void yg_mirror_sample(YGMirrorHistogram *h,uint64_t n) {
    ++h->bins[n>2048?2048:n]; ++h->count; if(n>h->max) h->max=n;
}
static inline uint64_t yg_mirror_percentile(const YGMirrorHistogram *h,unsigned p) {
    uint64_t rank=(h->count*p+99)/100,sum=0; unsigned i;
    if(!h->count) return 0;
    for(i=0;i<2049;i++) if((sum+=h->bins[i])>=rank) return i;
    return 2048;
}
typedef struct YGNetMirror {
    YGMirrorHistogram depth, latency, service, ping;
    uint64_t start_ms, last_service_ms, notified_ms;
    uint32_t notified_until, out_batch, out_batch_max;
    int active, notified;
} YGNetMirror;
static inline void yg_mirror_reset(YGNetMirror *m,uint64_t now,int active) {
    memset(m,0,sizeof *m); m->start_ms=now; m->active=active;
}
static inline void yg_mirror_read(YGNetMirror *m,uint32_t fill,uint32_t rx,uint64_t now) {
    if(!m->active) return;
    yg_mirror_sample(&m->depth,fill);
    /* Only slots published when this doorbell was observed have that timestamp.
     * Later arrivals must wait for their own notification; never invent arrival times. */
    if(m->notified && (int32_t)(m->notified_until-rx)>0)
        yg_mirror_sample(&m->latency,now-m->notified_ms);
}
static inline void yg_mirror_service(YGNetMirror *m,uint64_t now,unsigned ping) {
    if(!m->active) return;
    if(m->last_service_ms) yg_mirror_sample(&m->service,now-m->last_service_ms);
    m->last_service_ms=now;
    yg_mirror_sample(&m->ping,ping);
    if(m->out_batch>m->out_batch_max) m->out_batch_max=m->out_batch;
    m->out_batch=0;
}
#endif
