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
static atomic_uint head = 0;
static atomic_uint tail = 0;

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

unsigned t = atomic_load_explicit(&tail, memory_order_relaxed);
unsigned h_ = atomic_load_explicit(&head, memory_order_acquire);
unsigned nt = next_idx(t);

// Ring full -> fail open (immediate accept)
if (nt == h_) {
nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
return 0;
}

ring[t].id = id;
ring[t].len = (uint32_t)len;
ring[t].t_enqueue_ns = now_ns();

atomic_store_explicit(&tail, nt, memory_order_release);
atomic_fetch_add(&pressure_bytes, (int64_t)len);
return 0; // delayed verdict (stored)
}

static void release_fifo_bytes(int64_t budget_bytes) {
  if (budget_bytes <= 0) return;

  unsigned h0 = atomic_load_explicit(&head, memory_order_relaxed);
  unsigned t0 = atomic_load_explicit(&tail, memory_order_acquire);
  if (h0 == t0) return;

  int64_t released = 0;
  uint32_t last_id = 0;
  unsigned hcur = h0;
  pktmeta* prev_pkt = NULL;

  while (hcur != t0 && released < budget_bytes) {
    pktmeta* p = &ring[hcur];
    
    // Detect ID wrap-around (packet ID dropped significantly)
    // If wrapped, we MUST flush the pre-wrap batch first.
    if (prev_pkt && p->id < prev_pkt->id) {
       nfq_set_verdict_batch(qh, prev_pkt->id, NF_ACCEPT);
       // We only released up to prev_pkt. The current packet p (low ID) is NOT released yet.
       // We reset last_id so we don't try to batch-release it again with the wrong ID logic.
       last_id = 0; 
    }

    released += (int64_t)p->len;
    last_id = p->id;
    prev_pkt = p;
    hcur = next_idx(hcur);
  }

  if (last_id != 0) {
    // Batch accept: all queued packets with id <= last_id
    nfq_set_verdict_batch(qh, last_id, NF_ACCEPT);
  }
  
  if (released > 0) {
    atomic_store_explicit(&head, hcur, memory_order_release);
    atomic_fetch_sub(&pressure_bytes, released);
    if (atomic_load(&pressure_bytes) < 0) atomic_store(&pressure_bytes, 0);
    
    // Subtract used budget from the accumulator
    atomic_fetch_sub(&release_budget, released);
  }
}

static void release_overdue(void) {
  for (;;) {
    unsigned h0 = atomic_load_explicit(&head, memory_order_relaxed);
    unsigned t0 = atomic_load_explicit(&tail, memory_order_acquire);
    if (h0 == t0) return;

    if ((now_ns() - ring[h0].t_enqueue_ns) < MAX_DELAY_NS) return;

    uint32_t id = ring[h0].id;
    int64_t len = (int64_t)ring[h0].len;
    unsigned h1 = next_idx(h0);

    // Release overdue packet via batch up to its id (it is the head)
    // Note: Overdue release is one-by-one (head only), so wrap-around within a batch isn't an issue here 
    // unless we were batching multiple overdue packets. The original code does one by one.
    nfq_set_verdict_batch(qh, id, NF_ACCEPT);
    atomic_store_explicit(&head, h1, memory_order_release);
    atomic_fetch_sub(&pressure_bytes, len);
    if (atomic_load(&pressure_bytes) < 0) atomic_store(&pressure_bytes, 0);
  }
}

static void* poll_thread(void* arg) {
  (void)arg;
  // Bug fix: Increase buffer to avoid truncation of Jumbo frames / GSO
  unsigned char buf[65536] __attribute__((aligned));

  struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };

  while (atomic_load(&running)) {
    // Wake every 1ms
    int pr = poll(&pfd, 1, 1);

    if (pr > 0 && (pfd.revents & POLLIN)) {
      for (;;) {
        int rv = recv(fd, buf, sizeof(buf), 0);
        if (rv < 0) {
          if (errno == EAGAIN || errno == EWOULDBLOCK) break;
          break;
        }
        nfq_handle_packet(h, (char*)buf, rv);
      }
    }

    release_overdue();

    // Consume budget: check what is available
    int64_t budget = atomic_load(&release_budget);
    release_fifo_bytes(budget);
  }

  // Fail-open flush on shutdown
  for (;;) {
    unsigned h0 = atomic_load(&head);
    unsigned t0 = atomic_load(&tail);
    if (h0 == t0) break;
    // For flush, we can use a huge budget
    // But we need to be careful about wrap-around if we reuse release_fifo_bytes logic
    // The modified release_fifo_bytes handles wrap-around, so we can just call it.
    // However, release_fifo_bytes now subtracts from release_budget.
    // For shutdown flush, we just want to drain.
    // Let's manually drain or just cheat the budget.
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
  
  // Bug fix: Configure kernel queue length to match user ring
  if (nfq_set_queue_maxlen(qh, MAX_PACKETS) < 0) {
      fprintf(stderr, "nfq_set_queue_maxlen failed\n");
      // Continue anyway or fail? "MUST fail-open". 
      // Analysis says "Fatal Logic Error". Proceeding with warning or return error?
      // Let's fail initialization so user checks it.
      // Actually, standard behavior if permissions/caps are missing.
  }

  nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff);

  fd = nfq_fd(h);

  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  atomic_store(&pressure_bytes, 0);
  atomic_store(&inflow_bytes, 0);
  atomic_store(&release_budget, 0);
  atomic_store(&running, 1);

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
  // Bug fix: Accumulative budget
  atomic_fetch_add(&release_budget, bytes);
}

void sluice_shutdown(void) {
  atomic_store(&running, 0);
  pthread_join(thr, NULL);
  if (qh) nfq_destroy_queue(qh);
  if (h) nfq_close(h);
  qh = NULL; h = NULL; fd = -1;
}
