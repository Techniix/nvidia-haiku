/*
 * Host test for the channel report added by mesa-nvk patch 0009: the
 * semaphore log and nvkmd_nvrm_channel_report()/_rc_status(), compiled
 * verbatim from the patched nvkmd_nvrm_ctx.c (run.sh extracts them) against
 * fake channels. It checks what a stalled CPU wait would print for a channel
 * parked on an acquire and for one stopped by an RC error.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <inttypes.h>
#include <pthread.h>
#define MIN2(a,b) ((a)<(b)?(a):(b))
typedef struct { pthread_mutex_t m; } simple_mtx_t;
static void simple_mtx_lock(simple_mtx_t *m){pthread_mutex_lock(&m->m);}
static void simple_mtx_unlock(simple_mtx_t *m){pthread_mutex_unlock(&m->m);}
typedef struct { uint32_t info32; uint16_t info16; uint16_t status; } NvNotification;
typedef struct { uint32_t GPPut, GPGet; } KeplerBControlGPFifo;
#define NV906F_NOTIFIERS_RC 2
struct nvkmd_mem { void *map; };
struct nvkmd_nvrm_dev { simple_mtx_t sync_mutex; };
#define NVRM_CHAN_SEM_LOG 16
struct nvkmd_nvrm_sem_record { const volatile uint64_t *word; uint64_t addr, value, fence; };
struct nvkmd_nvrm_channel {
   struct nvkmd_nvrm_dev *dev; const char *kind;
   struct nvkmd_nvrm_sem_record acqLog[NVRM_CHAN_SEM_LOG], relLog[NVRM_CHAN_SEM_LOG];
   uint32_t acqLogNext, relLogNext;
   struct nvkmd_mem *notifier, *userD, *sem; uint64_t wSeq;
};
uint64_t nvkmd_nvrm_channel_completed(const struct nvkmd_nvrm_channel *c){ return *(volatile uint64_t*)c->sem->map; }
#include EXTRACTED

int main(void) {
   struct nvkmd_nvrm_dev dev; pthread_mutex_init(&dev.sync_mutex.m, NULL);
   NvNotification notif[4]; memset(notif,0,sizeof notif);
   KeplerBControlGPFifo ud = {10, 7}; uint64_t done = 0;
   struct nvkmd_mem mn={notif}, mu={&ud}, ms={&done};
   struct nvkmd_nvrm_channel c; memset(&c,0,sizeof c);
   c.dev=&dev; c.kind="exec"; c.notifier=&mn; c.userD=&mu; c.sem=&ms;
   volatile uint64_t timeline = 11, upload = 5;
   /* 20 submit cycles: each acquires the upload stream and releases the timeline */
   for (int i = 1; i <= 20; i++) {
      chan_log_sem(&c, true, &upload, 0x1000, (uint64_t)i);
      chan_log_sem(&c, false, &timeline, 0x2000, (uint64_t)i);
      c.wSeq++;
   }
   done = 11;  /* GPU completed cycle 11; parked on acquire of upload >= 12 */
   printf("== parked on an acquire\n");
   nvkmd_nvrm_channel_report(&c);
   int fails = 0;
   if (nvkmd_nvrm_channel_rc_status(&c) != 0) { puts("FAIL: rc on a healthy channel"); fails++; }
   notif[NV906F_NOTIFIERS_RC].status = 0x1f;
   done = 20;
   printf("== RC error, everything completed\n");
   nvkmd_nvrm_channel_report(&c);
   if (nvkmd_nvrm_channel_rc_status(&c) != 0x1f) { puts("FAIL: rc status"); fails++; }
   /* 20 releases logged into a 16-entry ring: the oldest are overwritten. */
   if (c.relLogNext != 20 || c.relLog[(20 - 1) % NVRM_CHAN_SEM_LOG].value != 20
       || c.acqLog[(20 - 16) % NVRM_CHAN_SEM_LOG].fence != 5) { puts("FAIL: ring"); fails++; }
   puts(fails ? "checks failed" : "all checks passed");
   return fails != 0;
}
