/* Host test for mesa-nvk patch 0008: drives the real sync-pool functions from
 * NVK's nvkmd_nvrm_sync.c against fake channels and checks that a freed
 * semaphore slot is never handed out again before every channel has retired
 * the work that may still use it.
 *
 * run.sh extracts the pool functions verbatim from the patched mesa-nvk tree
 * (after Build.sh has applied tools/patches/mesa-nvk) into pool_extract.c,
 * which this file includes, together with nvkmd_nvrm_retire.h. Runs on the
 * build machine; no GPU needed. */
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "u_dynarray.h"
typedef int VkResult;
#define VK_SUCCESS 0
#define VK_ERROR_OUT_OF_HOST_MEMORY -1
#define VK_ERROR_OUT_OF_DEVICE_MEMORY -2
#define NVKMD_MEM_GART 1
#define NVKMD_MEM_MAP_RDWR 1
typedef pthread_mutex_t simple_mtx_t;
#define simple_mtx_lock pthread_mutex_lock
#define simple_mtx_unlock pthread_mutex_unlock

struct nvkmd_mem { void *map; uint64_t size_B; };
struct nvkmd_dev { int dummy; };
struct nvkmd_nvrm_dev {
   struct nvkmd_dev base;
   simple_mtx_t sync_mutex;
   struct util_dynarray sync_pages, sync_events, sync_channels, sync_retiring;
   bool sync_retire_reported;
};
struct nvrm_sync_data { uint64_t value, submit_seq, wait_seq, reset_mark; };
#define NVRM_SYNC_SLOT_SIZE sizeof(struct nvrm_sync_data)
struct nvkmd_nvrm_sync_page { struct nvkmd_mem *mem; uint32_t num_slots, free_count; uint64_t bitmap[]; };
struct nvkmd_nvrm_sync_event { int fd; uint32_t hEvent; };

/* Fake channel: completed / submitted fences and whether its push has
 * unsubmitted methods. `bump_after_reads` > 0 makes the channel submit new
 * work (wSeq += 10) once its retire point has been read that many times, to
 * mimic another thread submitting while a slot is being freed. */
struct nvkmd_nvrm_channel { uint64_t completed, wSeq; bool push_pending; int bump_after_reads, reads; };
uint64_t nvkmd_nvrm_channel_completed(const struct nvkmd_nvrm_channel *c) { return c->completed; }
uint64_t nvkmd_nvrm_channel_retire_point(const struct nvkmd_nvrm_channel *cc)
{
   struct nvkmd_nvrm_channel *c = (struct nvkmd_nvrm_channel *)cc;
   uint64_t p = c->push_pending ? c->wSeq + 1 : c->wSeq;
   if (c->bump_after_reads > 0 && ++c->reads == c->bump_after_reads)
      c->wSeq += 10;
   return p;
}

/* Minimal stand-ins for sync_reset_data()'s environment. */
#define MAX2(a, b) ((a) > (b) ? (a) : (b))
struct nvkmd_nvrm_sync { struct nvkmd_mem *mem; uint64_t offset; };

static int mems_alive;
static VkResult nvkmd_dev_alloc_mapped_mem(struct nvkmd_dev *d, void *log, uint64_t size, uint64_t align, int f, int m, struct nvkmd_mem **out)
{ (void)d;(void)log;(void)align;(void)f;(void)m; struct nvkmd_mem *mem = calloc(1, sizeof(*mem)); mem->map = calloc(1, size); mem->size_B = size; *out = mem; mems_alive++; return VK_SUCCESS; }
static void nvkmd_mem_unref(struct nvkmd_mem *m) { free(m->map); free(m); mems_alive--; }
static void sync_event_destroy(struct nvkmd_nvrm_dev *d, int fd, uint32_t h) { (void)d;(void)fd;(void)h; }
static uint32_t sync_pool_slots_per_page(void) { return 0x1000 / NVRM_SYNC_SLOT_SIZE; }

#include "nvkmd_nvrm_retire.h"
static struct nvrm_sync_data *sync_data(struct nvkmd_nvrm_sync *s) { return (void *)((char *)s->mem->map + s->offset); }
bool nvkmd_nvrm_sync_register_channel(struct nvkmd_nvrm_dev *dev, struct nvkmd_nvrm_channel *chan);
void nvkmd_nvrm_sync_unregister_channel(struct nvkmd_nvrm_dev *dev, struct nvkmd_nvrm_channel *chan);
void nvkmd_nvrm_sync_pool_finish(struct nvkmd_nvrm_dev *dev);
#include "pool_extract.c"
#include "reset_extract.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); failures++; } } while (0)

static struct nvrm_sync_data *slot_data(struct nvkmd_mem *m, uint64_t off) { return (void *)((char *)m->map + off); }
static unsigned retiring(struct nvkmd_nvrm_dev *d) { return util_dynarray_num_elements(&d->sync_retiring, struct nvrm_retire_entry *); }

int main(void)
{
   struct nvkmd_nvrm_dev dev = {0};
   pthread_mutex_init(&dev.sync_mutex, NULL);
   util_dynarray_init(&dev.sync_pages, NULL); util_dynarray_init(&dev.sync_events, NULL);
   util_dynarray_init(&dev.sync_channels, NULL); util_dynarray_init(&dev.sync_retiring, NULL);

   struct nvkmd_nvrm_channel exec = {0}, bind = {0};
   struct nvkmd_mem *m; uint64_t a, b, c;

   /* 1. No channels: a freed slot is reused at once (old behaviour). */
   CHECK(sync_pool_alloc_slot(&dev, &m, &a) == VK_SUCCESS);
   sync_pool_free_slot(&dev, m, a);
   CHECK(retiring(&dev) == 0);
   CHECK(sync_pool_alloc_slot(&dev, &m, &b) == VK_SUCCESS && b == a);
   sync_pool_free_slot(&dev, m, b);

   nvkmd_nvrm_sync_register_channel(&dev, &exec);
   nvkmd_nvrm_sync_register_channel(&dev, &bind);

   /* 2. The OBS hang: a GPU acquire on slot A is queued (submitted, fence 5
    * not yet completed) when the temporary owning A is destroyed. The next
    * allocation must NOT get A back, and A's words must not be zeroed. */
   CHECK(sync_pool_alloc_slot(&dev, &m, &a) == VK_SUCCESS);
   slot_data(m, a)->value = 3; slot_data(m, a)->wait_seq = 4;   /* acquire waits for 4 */
   exec.wSeq = 5; exec.completed = 4;
   sync_pool_free_slot(&dev, m, a);
   CHECK(retiring(&dev) == 1);
   CHECK(dev.sync_retire_reported);
   CHECK(sync_pool_alloc_slot(&dev, &m, &b) == VK_SUCCESS);
   CHECK(b != a);
   CHECK(slot_data(m, a)->value == 3 && slot_data(m, a)->wait_seq == 4);

   /* 3. Methods still in an unsubmitted push count toward the next fence. */
   exec.completed = 5; exec.push_pending = true;
   CHECK(sync_pool_alloc_slot(&dev, &m, &c) == VK_SUCCESS);  /* reclaims A (point 5 done) */
   CHECK(c == a);                                            /* A is reusable now */
   sync_pool_free_slot(&dev, m, c);                          /* push pending: needs fence 6 */
   CHECK(retiring(&dev) == 1);
   uint64_t d2; CHECK(sync_pool_alloc_slot(&dev, &m, &d2) == VK_SUCCESS && d2 != c);
   exec.push_pending = false; exec.wSeq = 6; exec.completed = 6;
   uint64_t e2; CHECK(sync_pool_alloc_slot(&dev, &m, &e2) == VK_SUCCESS && e2 == c);
   CHECK(retiring(&dev) == 0);

   /* 4. Every channel must have retired: bind channel still busy. */
   bind.wSeq = 2; bind.completed = 1;
   sync_pool_free_slot(&dev, m, e2);
   exec.completed = 100;
   uint64_t f2; CHECK(sync_pool_alloc_slot(&dev, &m, &f2) == VK_SUCCESS && f2 != e2);
   CHECK(retiring(&dev) == 1);

   /* 5. Destroying the busy channel releases what waited only on it. */
   nvkmd_nvrm_sync_unregister_channel(&dev, &bind);
   CHECK(retiring(&dev) == 0);
   uint64_t g2; CHECK(sync_pool_alloc_slot(&dev, &m, &g2) == VK_SUCCESS && g2 == e2);

   /* 6. Unregistering a channel that was never registered is harmless. */
   struct nvkmd_nvrm_channel stray = {0};
   nvkmd_nvrm_sync_unregister_channel(&dev, &stray);

   /* 7. Teardown with entries still parked frees everything. */
   exec.wSeq = 200; exec.completed = 150;
   sync_pool_free_slot(&dev, m, g2); sync_pool_free_slot(&dev, m, f2);
   sync_pool_free_slot(&dev, m, b);  sync_pool_free_slot(&dev, m, d2);
   CHECK(retiring(&dev) == 4);
   nvkmd_nvrm_sync_unregister_channel(&dev, &exec);
   CHECK(retiring(&dev) == 0);
   nvkmd_nvrm_sync_pool_finish(&dev);
   CHECK(mems_alive == 0);

   /* 8. A dedicated sync's memory (kind MEM_UNREF) is unreffed only after the
    * channels retire, and right away when they are idle. */
   {
      struct nvkmd_nvrm_dev d2 = {0};
      pthread_mutex_init(&d2.sync_mutex, NULL);
      util_dynarray_init(&d2.sync_pages, NULL); util_dynarray_init(&d2.sync_events, NULL);
      util_dynarray_init(&d2.sync_channels, NULL); util_dynarray_init(&d2.sync_retiring, NULL);
      struct nvkmd_nvrm_channel ch2 = { .completed = 3, .wSeq = 4 };
      CHECK(nvkmd_nvrm_sync_register_channel(&d2, &ch2));
      struct nvkmd_mem *ded; int before = mems_alive;
      nvkmd_dev_alloc_mapped_mem(&d2.base, NULL, 0x1000, 0x1000, 0, 0, &ded);
      sync_retire(&d2, ded, 0, SYNC_RETIRE_MEM_UNREF);
      CHECK(mems_alive == before + 1);          /* still referenced */
      ch2.completed = 4;
      struct nvkmd_mem *pm; uint64_t po;
      CHECK(sync_pool_alloc_slot(&d2, &pm, &po) == VK_SUCCESS);  /* triggers reclaim */
      CHECK(mems_alive == before + 1);          /* ded gone, pool page added */
      nvkmd_dev_alloc_mapped_mem(&d2.base, NULL, 0x1000, 0x1000, 0, 0, &ded);
      sync_retire(&d2, ded, 0, SYNC_RETIRE_MEM_UNREF);   /* idle: at once */
      CHECK(mems_alive == before + 1);
      nvkmd_nvrm_sync_unregister_channel(&d2, &ch2);
      nvkmd_nvrm_sync_pool_finish(&d2);
      CHECK(mems_alive == before);
   }

   /* 9. A channel that submits while the slot is being freed must not push
    * another busy channel out of the retire entry. */
   {
      struct nvkmd_nvrm_dev d2 = {0};
      pthread_mutex_init(&d2.sync_mutex, NULL);
      util_dynarray_init(&d2.sync_pages, NULL); util_dynarray_init(&d2.sync_events, NULL);
      util_dynarray_init(&d2.sync_channels, NULL); util_dynarray_init(&d2.sync_retiring, NULL);
      struct nvkmd_nvrm_channel c0 = { .completed = 5, .wSeq = 5, .bump_after_reads = 1 };
      struct nvkmd_nvrm_channel c1 = { .completed = 1, .wSeq = 2 };
      CHECK(nvkmd_nvrm_sync_register_channel(&d2, &c0));
      CHECK(nvkmd_nvrm_sync_register_channel(&d2, &c1));
      struct nvkmd_mem *pm; uint64_t s0, s1;
      CHECK(sync_pool_alloc_slot(&d2, &pm, &s0) == VK_SUCCESS);
      sync_pool_free_slot(&d2, pm, s0);       /* c0 idle at read, then submits; c1 busy */
      c0.completed = c0.wSeq;                 /* c0 retires everything */
      CHECK(sync_pool_alloc_slot(&d2, &pm, &s1) == VK_SUCCESS);
      CHECK(s1 != s0);                        /* c1 has not retired: s0 stays parked */
      c1.completed = 2;
      uint64_t s2; CHECK(sync_pool_alloc_slot(&d2, &pm, &s2) == VK_SUCCESS && s2 == s0);
      nvkmd_nvrm_sync_unregister_channel(&d2, &c0);
      nvkmd_nvrm_sync_unregister_channel(&d2, &c1);
      nvkmd_nvrm_sync_pool_finish(&d2);
   }

   /* 10. Resetting a binary sync whose consumed signal is submitted but not
    * yet executed must leave it unsignaled even after that release lands
    * (reset_mark covers submit_seq, not just value). */
   {
      struct nvkmd_mem m10; struct nvrm_sync_data data = { .value = 4, .submit_seq = 5 };
      m10.map = &data; m10.size_B = sizeof(data);
      struct nvkmd_nvrm_sync s10 = { .mem = &m10, .offset = 0 };
      sync_reset_data(&s10);
      CHECK(data.reset_mark == 5);
      data.value = 5;                         /* the GPU release lands */
      CHECK(!(data.value > data.reset_mark)); /* still unsignaled */
      CHECK(!(MAX2(data.submit_seq, data.value) > data.reset_mark)); /* not pending either */
   }

   /* 11. Stress: random alloc/free with GPU lag never hands out a slot whose
    * retire point is not reached. */
   struct nvkmd_nvrm_dev d3 = {0};
   pthread_mutex_init(&d3.sync_mutex, NULL);
   util_dynarray_init(&d3.sync_pages, NULL); util_dynarray_init(&d3.sync_events, NULL);
   util_dynarray_init(&d3.sync_channels, NULL); util_dynarray_init(&d3.sync_retiring, NULL);
   struct nvkmd_nvrm_channel ch = {0};
   nvkmd_nvrm_sync_register_channel(&d3, &ch);
   enum { N = 4000, LIVE = 64 };
   struct { struct nvkmd_mem *m; uint64_t off; } live[LIVE]; int nlive = 0;
   /* point at which each (mem,offset) last became free; key by pointer+offset */
   struct { struct nvkmd_mem *m; uint64_t off; uint64_t free_point; } freed[N]; int nfreed = 0;
   srand(1);
   for (int it = 0; it < N; it++) {
      if (nlive < LIVE && (rand() & 1)) {
         struct nvkmd_mem *mm; uint64_t off;
         CHECK(sync_pool_alloc_slot(&d3, &mm, &off) == VK_SUCCESS);
         for (int k = nfreed - 1; k >= 0; k--)
            if (freed[k].m == mm && freed[k].off == off) {
               if (ch.completed < freed[k].free_point) { printf("FAIL: slot reused before retire (it %d)\n", it); failures++; }
               break;
            }
         live[nlive].m = mm; live[nlive].off = off; nlive++;
      } else if (nlive > 0) {
         int k = rand() % nlive;
         ch.push_pending = rand() & 1;
         freed[nfreed].m = live[k].m; freed[nfreed].off = live[k].off;
         freed[nfreed].free_point = nvkmd_nvrm_channel_retire_point(&ch); nfreed++;
         sync_pool_free_slot(&d3, live[k].m, live[k].off);
         live[k] = live[--nlive];
      }
      if (rand() % 3 == 0) { ch.wSeq++; ch.push_pending = false; }       /* submit */
      if (ch.completed < ch.wSeq && rand() % 2 == 0) ch.completed++;     /* GPU retires */
   }
   nvkmd_nvrm_sync_unregister_channel(&d3, &ch);
   nvkmd_nvrm_sync_pool_finish(&d3);

   printf(failures ? "%d FAILURE(S)\n" : "all checks passed\n", failures);
   return failures != 0;
}
