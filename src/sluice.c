#define _GNU_SOURCE
#include "sluice.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <libnetfilter_queue/libnetfilter_queue.h>
#include <linux/netfilter.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>

#define MAX_PACKETS 16384
#define MAX_DELAY_NS 25000000LL // 25ms hard deadline

typedef struct {
  uint32_t id;
  uint32_t len;
  int64_t t_enqueue_ns;
} pktmeta;

static pktmeta ring[MAX_PACKETS];
// Simplify Concurrency Model: Single-threaded consumer/producer in poll_thread
static unsigned head = 0;
static unsigned tail = 0;

static atomic_llong pressure_bytes = 0;
static atomic_llong inflow_bytes = 0;
static atomic_llong release_budget = 0;

static atomic_int running = 0;

static struct nfq_handle* h = NULL;
static struct nfq_q_handle* qh = NULL;
static int fd = -1;
static pthread_t thr;

static inline int64_t now_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static inline unsigned next_idx(unsigned i) { return (i + 1u) % MAX_PACKETS; }

static int cb(struct nfq_q_handle* qh_, struct nfgenmsg* nfmsg,
              struct nfq_data* nfa, void* data) {
  (void)qh_; (void)nfmsg; (void)data;

  struct nfqnl_msg_packet_hdr* ph = nfq_get_msg_packet_hdr(nfa);
  if (!ph) return 0;

  uint32_t id = ntohl(ph->packet_id);

  unsigned char* payload = NULL;
  int len = nfq_get_payload(nfa, &payload); // correct usage: pass pointer
  (void)payload;
  if (len < 0) len = 0;

  atomic_fetch_add(&inflow_bytes, (int64_t)len);

  unsigned t = tail;
  unsigned h_ = head;
  unsigned nt = next_idx(t);

  // Ring full -> fail open (immediate accept)
  if (nt == h_) {
    nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
    return 0;
  }

  ring[t].id = id;
  ring[t].len = (uint32_t)len;
  ring[t].t_enqueue_ns = now_ns();

  tail = nt;
  atomic_fetch_add(&pressure_bytes, (int64_t)len);
  return 0; // delayed verdict (stored)
}

static void release_fifo_bytes(int64_t budget_bytes) {
  if (budget_bytes <= 0) return;

  if (head == tail) return;

  int64_t released = 0;
  uint32_t last_id = 0;
  unsigned hcur = head;
  unsigned t = tail;
  pktmeta* prev_pkt = NULL;

  while (hcur != t && released < budget_bytes) {
    pktmeta* p = &ring[hcur];
    
    // Detect ID wrap-around
    if (prev_pkt && p->id < prev_pkt->id) {
       nfq_set_verdict_batch(qh, prev_pkt->id, NF_ACCEPT);
       last_id = 0; 
    }

    released += (int64_t)p->len;
    last_id = p->id;
    prev_pkt = p;
    hcur = next_idx(hcur);
  }

  if (last_id != 0) {
    nfq_set_verdict_batch(qh, last_id, NF_ACCEPT);
  }
  
  if (released > 0) {
    head = hcur;
    atomic_fetch_sub(&pressure_bytes, released);
    if (atomic_load(&pressure_bytes) < 0) atomic_store(&pressure_bytes, 0);
    atomic_fetch_sub(&release_budget, released);
  }
}

static void release_overdue(void) {
  if (head == tail) return;
  
  // Optimization: Find the youngest packet that is overdue
  // Then release everything up to it in one batch.
  int64_t now = now_ns();
  unsigned hcur = head;
  unsigned t = tail;
  unsigned h_last_overdue = hcur; // Default to no move if head isn't overdue (check loop)
  uint32_t last_id = 0;
  int64_t released_bytes = 0;
  int found_overdue = 0;

  // We scan forward until we find a packet that is NOT overdue
  while (hcur != t) {
      if ((now - ring[hcur].t_enqueue_ns) < MAX_DELAY_NS) {
          // This packet is young enough. Stop.
          break;
      }
      // This packet is overdue.
      h_last_overdue = hcur;
      last_id = ring[hcur].id;
      released_bytes += ring[hcur].len;
      hcur = next_idx(hcur);
      found_overdue = 1;
  }

  if (found_overdue) {
      // release_fifo_bytes handles ID wrapping for normal generic release.
      // Here we need to be careful too. Or we can just reuse the batch concept.
      // If we see ID drop during scan, we should have flushed.
      // Complex optimization: strict scan for wrap needed?
      // Simpler approach for reliability:
      // Just release_fifo_bytes with a "virtual" budget equal to released_bytes?
      // No, we want to target specific ID.
      // Let's rely on batch behavior: release up to last_id.
      // Implicitly handles previous ones assuming no wrap between head and last_id.
      // If there IS a wrap, batch might fail for the pre-wrap ones if kernel checks ID > last_ID?
      // Actually, nfq_set_verdict_batch releases packet_id <= ID.
      // If we have 100, 101, ... 255, 0, 1, 2... and we want to release up to 2.
      // Batch(2) might release 0, 1, 2, but 100..255 are > 2, so they stay?
      // YES. Wrap-around breaks simple batching.
      
      // Re-scan for wrap-around to be safe
      unsigned scan = head;
      pktmeta* prev = NULL;
      uint32_t batch_target = 0;
      
      while (scan != hcur) { // iterate explicitly through the overdue range
          if (prev && ring[scan].id < prev->id) {
             // Wrapped. Flush up to prev.
             nfq_set_verdict_batch(qh, prev->id, NF_ACCEPT);
          }
          batch_target = ring[scan].id;
          prev = &ring[scan];
          scan = next_idx(scan);
      }
      
      // Final flush
      if (batch_target != 0) {
          nfq_set_verdict_batch(qh, batch_target, NF_ACCEPT);
      }

      // Update state
      head = hcur;
      atomic_fetch_sub(&pressure_bytes, released_bytes);
      if (atomic_load(&pressure_bytes) < 0) atomic_store(&pressure_bytes, 0);
  }
}

static void* poll_thread(void* arg) {
  (void)arg;
  unsigned char buf[65536] __attribute__((aligned));

  struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };

  while (atomic_load(&running)) {
    int pr = poll(&pfd, 1, 1);

    if (pr > 0 && (pfd.revents & POLLIN)) {
      // Fix Starvation: Burst limit
      int burst = 64;
      while (burst-- > 0) {
        int rv = recv(fd, buf, sizeof(buf), 0);
        if (rv < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          break;
        }
        nfq_handle_packet(h, (char*)buf, rv);
      }
    }

    release_overdue();

    int64_t budget = atomic_load(&release_budget);
    release_fifo_bytes(budget);
  }

  for (;;) {
    if (head == tail) break;
    atomic_store(&release_budget, 1LL << 60);
    release_fifo_bytes(1LL << 60);
  }

  return NULL;
}

int sluice_init(uint16_t queue_num) {
  h = nfq_open();
  if (!h) {
    fprintf(stderr, "nfq_open failed\n");
    return -1;
  }

  nfq_unbind_pf(h, AF_INET);
  nfq_bind_pf(h, AF_INET);
  nfq_unbind_pf(h, AF_INET6);
  nfq_bind_pf(h, AF_INET6);

  qh = nfq_create_queue(h, queue_num, &cb, NULL);
  if (!qh) {
    fprintf(stderr, "nfq_create_queue failed\n");
    nfq_close(h);
    h = NULL;
    return -2;
  }
  
  if (nfq_set_queue_maxlen(qh, MAX_PACKETS) < 0) {
      fprintf(stderr, "nfq_set_queue_maxlen failed\n");
  }

  nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff);

  fd = nfq_fd(h);

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  atomic_store(&pressure_bytes, 0);
  atomic_store(&inflow_bytes, 0);
  atomic_store(&release_budget, 0);
  atomic_store(&running, 1);
  
  // Reset ring state
  head = 0;
  tail = 0;

  if (pthread_create(&thr, NULL, poll_thread, NULL) != 0) {
    fprintf(stderr, "pthread_create failed\n");
    nfq_destroy_queue(qh);
    nfq_close(h);
    qh = NULL; h = NULL;
    return -3;
  }

  return 0;
}

int64_t sluice_get_pressure_bytes(void) {
  return atomic_load(&pressure_bytes);
}

int64_t sluice_get_inflow_bytes_and_reset(void) {
  return atomic_exchange(&inflow_bytes, 0);
}

void sluice_set_release_budget_bytes(int64_t bytes) {
  if (bytes < 0) bytes = 0;
  atomic_fetch_add(&release_budget, bytes);
}

void sluice_shutdown(void) {
  atomic_store(&running, 0);
  pthread_join(thr, NULL);
  if (qh) nfq_destroy_queue(qh);
  if (h) nfq_close(h);
  qh = NULL; h = NULL; fd = -1;
}

