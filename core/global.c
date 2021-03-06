/**
 * @file session.c
 * @brief implementation for user session.
 * @author Xiaotong Sun <xiaotons@andrew.cmu.edu>
 * @author Longqi Cai   <longqic@andrew.cmu.edu>
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include "global.h"

void g_state_init(g_state_t *g) {
  g->crash_timeout_millsec = 6000;    // TODO: reason about this value.
  g->data_timeout_millsec = 3000;     // TODO: do RTT estimation.
  g->curr_upload_conn_cnt = 0;
  g->curr_download_conn_cnt = 0;
  g->peer_socket = -1;
  g->g_config = NULL;
  g->g_session = NULL;

  int i;
  for (i = 0; i < MAX_PEER_NUM; i++)
    g->upload_conn_pool[i] = NULL;
  for (i = 0; i < MAX_PEER_NUM; i++)
    g->download_conn_pool[i] = NULL;

  g->pending_get_packets = queue_new();
}

void session_init(session_t *s) {
  s->state = NONE;
  s->chunk_map = hashmap_new();
  s->nlchunk_located = hashmap_new();
  s->nlchunk_map = hashmap_new();
  bzero(s->output_file, FILE_NAME_LEN);
  s->non_local_chunks = NULL;
}

void session_free(session_t *s) {
  hashmap_free(s->chunk_map);
  hashmap_free(s->nlchunk_map);
  session_nlchunk_t *iter = s->non_local_chunks;
  while (iter) {
    session_nlchunk_t *temp = iter;
    iter = iter->next;
    free(temp);
  }
  s->non_local_chunks = NULL;
}

/**
 * Iterator to tranverse and print map content.
 * Only int is allowed, though typed as any_t.
 */
int chunk_map_iter(const char* key, any_t val, map_t map) {
  console_log(" ---- <%s, %d>", key, (intptr_t) val);
  return MAP_OK;
}

void dump_session(session_t *s) {
  console_log("************* Peer's Current Session Info ************");
  console_log(" -- output-file-name: %s", s->output_file);
  console_log(" -- chunk-requested (%d):", hashmap_length(s->chunk_map));
  hashmap_iterate(s->chunk_map, chunk_map_iter, NULL);
  console_log(" -- non-local chunks (%d):", hashmap_length(s->nlchunk_map));
  hashmap_iterate(s->nlchunk_map, chunk_map_iter, NULL);
  console_log("*******************************************************");
}

/* ---------------------- Window related helpers ----------------------*/
void init_recv_window(g_state_t *g, short peer_id, const char *chunk) {
  recv_window_t *recv_window = (recv_window_t*)malloc(sizeof(recv_window_t));

  recv_window->state = IDLE;
  memcpy(recv_window->chunk_hash, chunk, HASH_STR_LEN);
  recv_window->max_window_size = INIT_WINDOW_SIZE;
  recv_window->next_packet_expected = 1;
  gettimeofday(&(recv_window->last_datapac_recvd), NULL);     // init timer for data packet.
  int i = 0;
  for (; i <= MAX_DATAPKT_FOR_CHUNK; i++)
    recv_window->buffer[i] = NULL;

  // change for passing test
  recv_window->accumulate_bytes = 0;

  g->download_conn_pool[peer_id] = recv_window;
}

void free_recv_window(g_state_t *g, short peer_id) {
  int i;
  for (i = 0; i <= MAX_SEQ_NUM; i++) {
    if (g->download_conn_pool[peer_id]->buffer[i] != NULL) {
      pkt_free(g->download_conn_pool[peer_id]->buffer[i]);
    }
  }
  free(g->download_conn_pool[peer_id]);
  g->download_conn_pool[peer_id] = NULL;
}

void init_send_window(g_state_t *g, short peer_id) {
  send_window_t *send_window = (send_window_t*)malloc(sizeof(send_window_t));

  send_window->max_window_size = 1;
  send_window->last_packet_acked = 0;
  send_window->last_packet_sent = 0;
  send_window->last_packet_available = 1;

  congctrl_t* cc = &send_window->cc;
  cc->state = SLOW_START;
  cc->cwnd = 1;
  cc->ssthresh = SSTHRESH;
  cc->fd = open(CC_LOG, O_CREAT|O_WRONLY|O_APPEND, 0640);
  cc->sender = g->g_config->identity;
  cc->recver = peer_id;
  gettimeofday(&cc->start, NULL);

  int i;
  for (i = 0; i <= MAX_SEQ_NUM; i++)
    send_window->buffer[i] = NULL;
  for (i = 0; i <= MAX_SEQ_NUM; i++)
    send_window->dup_ack_map[i] = 0;

  g->upload_conn_pool[peer_id] = send_window;
}

void free_send_window(g_state_t *g, short peer_id) {
  send_window_t* window = g->upload_conn_pool[peer_id];
  close(window->cc.fd);
  int i;
  for (i = 0; i <= MAX_SEQ_NUM; i++) {
    if (g->upload_conn_pool[peer_id]->buffer[i] != NULL) {
      pkt_free(g->upload_conn_pool[peer_id]->buffer[i]);
    }
  }
  free(g->upload_conn_pool[peer_id]);
  g->upload_conn_pool[peer_id] = NULL;
}

/* ---------------------- Other helpers ------------------------*/
pending_packet_t* build_pending_packet(packet_t *packet, short to_peer) {
  pending_packet_t *pending_packet
          = (pending_packet_t*)malloc(sizeof(pending_packet_t));
  pending_packet->packet = packet;
  pending_packet->to_peer = to_peer;
  return pending_packet;
}

void free_pending_packet(pending_packet_t* pending_packet) {
  pkt_free(pending_packet->packet);
  free(pending_packet);
}
