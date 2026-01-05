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

// Ring buffer definition
#define MAX_PACKETS 4096 
// N.B. Smaller ring (4k) is often better for latency than 16k if we are shaping.
// But you can keep 16384 if you prefer. 
// For shaping, deep buffers = bad latency. 
// Let's stick to your original 16384 to avoid changing behavior too drastically, 
// but technically 4096 is plenty for 1ms delays.
#undef MAX_PACKETS
#define MAX_PACKETS 16384

#define MAX_DELAY_NS 25000000LL // 25ms

typedef struct {
  uint32_t id;
  uint32_t len;
  int64_t t_enqueue_ns;
} pktmeta;

static pktmeta ring[MAX_PACKETS];
// Standard non-atomic indices for single-threaded consumer/producer
static unsigned head = 0;
static unsigned tail = 0;

// Shared with Chapel
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

// --- Verdict Helper ---
static void verdict_accept(uint32_t id) {
    nfq_set_verdict(qh, id, NF_ACCEPT, 0, NULL);
}

static int cb(struct nfq_q_handle* qh_, struct nfgenmsg* nfmsg,
              struct nfq_data* nfa, void* data) {
  (void)qh_; (void)nfmsg; (void)data;

  struct nfqnl_msg_packet_hdr* ph = nfq_get_msg_packet_hdr(nfa);
  if (!ph) return 0;

  uint32_t id = ntohl(ph->packet_id);
  
  // OPTIMIZATION: Get true length from attribute, not payload copy
  uint32_t full_len = nfq_get_payload_len(nfa);
  
  atomic_fetch_add(&inflow_bytes, (int64_t)full_len);

  unsigned nt = next_idx(tail);

  // OVERFLOW STRATEGY: HEAD-RELEASE (Preserve Order)
  if (nt == head) {
      // Ring is full. We must free up space.
      // Release the OLDEST packet (head) to accept the NEWEST.
      // This maintains flow ordering better than dropping the new one 
      // or "fail-open" on the new one.
      
      pktmeta* old_p = &ring[head];
      verdict_accept(old_p->id);
      
      atomic_fetch_sub(&pressure_bytes, (int64_t)old_p->len);
      // Advance head
      head = next_idx(head);
  }

  // Enqueue new packet
  ring[tail].id = id;
  ring[tail].len = full_len;
  ring[tail].t_enqueue_ns = now_ns();
  
  tail = next_idx(tail); // tail = nt
  atomic_fetch_add(&pressure_bytes, (int64_t)full_len);
  
  return 0; 
}

static void release_fifo_bytes(int64_t budget) {
  if (budget <= 0) return;

  int64_t used = 0;
  
  while (head != tail) {
      pktmeta* p = &ring[head];
      
      // If releasing this packet exceeds budget, should we stop?
      // "Leaky bucket" usually allows the packet that crosses the threshold
      // provided we pay for it. 
      // However, to be strict and avoid debt spirals, let's stop if used > 0.
      if (used > 0 && (used + p->len) > budget) {
          break; 
      }

      verdict_accept(p->id);
      used += p->len;
      head = next_idx(head);
      
      // Safety break for huge budget
      if (used >= budget) break;
  }

  if (used > 0) {
      atomic_fetch_sub(&pressure_bytes, used);
      // Clamp pressure to 0 just in case
      if (atomic_load(&pressure_bytes) < 0) atomic_store(&pressure_bytes, 0);

      // Subtract what we actually used
      int64_t current_budget = atomic_load(&release_budget);
      int64_t new_budget = current_budget - used;
      if (new_budget < 0) new_budget = 0; // Prevent debt
      atomic_store(&release_budget, new_budget);
  }
}

static void release_overdue(void) {
  int64_t now = now_ns();
  int64_t released_bytes = 0;

  // Since ring is strictly FIFO by time, we just check head.
  while (head != tail) {
      pktmeta* p = &ring[head];
      if ((now - p->t_enqueue_ns) < MAX_DELAY_NS) {
          // Head is young enough, so are all subsequent packets.
          break;
      }
      
      // Head is too old. Release it.
      verdict_accept(p->id);
      released_bytes += p->len;
      head = next_idx(head);
  }

  if (released_bytes > 0) {
      atomic_fetch_sub(&pressure_bytes, released_bytes);
      if (atomic_load(&pressure_bytes) < 0) atomic_store(&pressure_bytes, 0);
  }
}

static void* poll_thread(void* arg) {
  (void)arg;
  // Reduced buffer size - we only asked for headers!
  // 4096 is safe for receiving netlink messages with attributes.
  unsigned char buf[4096] __attribute__((aligned));

  struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };

  while (atomic_load(&running)) {
    // 1ms poll timeout
    int pr = poll(&pfd, 1, 1);

    if (pr > 0 && (pfd.revents & POLLIN)) {
      // Drain the socket
      // Using a batch limit to allow interleaving of release logic
      int burst = 64; 
      while (burst-- > 0) {
        int rv = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (rv < 0) {
           if (errno != EAGAIN && errno != EWOULDBLOCK) {
               // Fatal error or weird state
               perror("recv"); 
           }
           break;
        }
        nfq_handle_packet(h, (char*)buf, rv);
      }
    }

    release_overdue();

    int64_t budget = atomic_load(&release_budget);
    release_fifo_bytes(budget);
  }

  // Flush remaining on exit
  while (head != tail) {
      verdict_accept(ring[head].id);
      head = next_idx(head);
  }

  return NULL;
}

int sluice_init(uint16_t queue_num) {
  h = nfq_open();
  if (!h) return -1;

  nfq_unbind_pf(h, AF_INET);
  nfq_bind_pf(h, AF_INET);
  nfq_unbind_pf(h, AF_INET6);
  nfq_bind_pf(h, AF_INET6);

  qh = nfq_create_queue(h, queue_num, &cb, NULL);
  if (!qh) {
    nfq_close(h);
    return -2;
  }
  
  if (nfq_set_queue_maxlen(qh, MAX_PACKETS) < 0) {
      perror("nfq_set_queue_maxlen");
  }

  // CRITICAL OPTIMIZATION: COPY_PACKET with size 128
  // This gives us IP + TCP headers, which is plenty.
  // We use nfq_get_payload_len() for the accounting.
  nfq_set_mode(qh, NFQNL_COPY_PACKET, 128);

  fd = nfq_fd(h);
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);

  atomic_store(&pressure_bytes, 0);
  atomic_store(&inflow_bytes, 0);
  atomic_store(&release_budget, 0);
  atomic_store(&running, 1);
  head = 0;
  tail = 0;

  if (pthread_create(&thr, NULL, poll_thread, NULL) != 0) {
    nfq_destroy_queue(qh);
    nfq_close(h);
    return -3;
  }
  return 0;
}

// ... Getters/Setters remain the same ...
int64_t sluice_get_pressure_bytes(void) { return atomic_load(&pressure_bytes); }
int64_t sluice_get_inflow_bytes_and_reset(void) { return atomic_exchange(&inflow_bytes, 0); }
void sluice_set_release_budget_bytes(int64_t bytes) { 
    if (bytes < 0) bytes = 0; 
    atomic_fetch_add(&release_budget, bytes); 
}
void sluice_shutdown(void) {
  atomic_store(&running, 0);
  pthread_join(thr, NULL);
  if (qh) nfq_destroy_queue(qh);
  if (h) nfq_close(h);
}
