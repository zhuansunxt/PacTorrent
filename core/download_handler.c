/**
 * @file download_handler.c
 * @brief Implementation for download component for the system.
 * @author Xiaotong Sun <xiaotons@andrew.cmu.edu>
 * @author Longqi Cai   <longqic@andrew.cmu.edu>
 */

#include "download_handler.h"

/**
 * GET packet contains only the chunk hash for the chunk
 * the client wants to fetch.
 * @param chunk_hash  string of hash_value of chunk.
 * @return built GET packet.
 */
packet_t *build_get_packet(const char *chunk_hash) {
  packet_t *get_packet = pkt_new();
  get_packet->hdr->magic = htons(15441);
  get_packet->hdr->version = 1;
  get_packet->hdr->type = 2;
  get_packet->hdr->hlen = htons(HDRSZ);
  get_packet->hdr->plen = htons(HDRSZ+SHA1_HASH_SIZE);
  get_packet->hdr->seqn = htonl(0);
  get_packet->hdr->ackn = htonl(0);

  /* Copy chunk hash to packet. */
  char hash_hex[SHA1_HASH_SIZE];
  ascii2hex(chunk_hash, SHA1_HASH_SIZE*2, (uint8_t*)hash_hex);
  memcpy(get_packet->payload, hash_hex, SHA1_HASH_SIZE);
  return get_packet;
}

/**
 * Construct DATA packet.
 * @param seq seqeunce number.
 * @param data_size size of attached data.
 * @param data pointer to attached data.
 * @return built DATA packet.
 */
packet_t *build_data_packet(unsigned int seq, size_t data_size, char *data) {
  packet_t *data_packet = pkt_new();
  uint16_t packet_len = HDRSZ + (uint16_t)data_size;

  data_packet->hdr->magic = htons(15441);
  data_packet->hdr->version = 1;
  data_packet->hdr->type = 3;                       // packet type: DATA
  data_packet->hdr->hlen = htons(HDRSZ);
  data_packet->hdr->plen = htons(packet_len);
  data_packet->hdr->seqn = htonl((uint32_t)seq);
  data_packet->hdr->ackn = htonl(0);

  memcpy(data_packet->payload, data, data_size);
  return data_packet;
}

/**
 * Build DATA packets and buffer them.
 * @param chunk_hash The given hashvalue for the chunk.
 * @param g Global state.
 * @param des_peer Destination peer of generated DATA packet.
 */
void build_chunk_data_packets(const char *chunk_hash, g_state_t *g, short des_peer) {
  /* Get chunk's offset in master-data-file */
  int *offset = (int*)malloc(sizeof(int));
  hashmap_get(g->g_config->chunks->has_chunk_map, chunk_hash, (any_t*) offset);
  console_log("Building DATA packet for chunk %s with offset %d", chunk_hash, *offset);

  int num_of_packet = (CHUNK_SIZE % DATA_PACKET_SIZE) > 0 ?
                      (CHUNK_SIZE / DATA_PACKET_SIZE)+1 : (CHUNK_SIZE / DATA_PACKET_SIZE);
  int i;
  FILE *f = fopen(g->g_config->chunk_file, "r");
  for (i = 0; i < num_of_packet; i++) {
    unsigned int seq = i+1;
    size_t data_size;
    char data[DATA_PACKET_SIZE];

    rewind(f);
    long data_offset = (*offset) * CHUNK_SIZE + i * DATA_PACKET_SIZE;
    fseek(f, data_offset, SEEK_SET);
    if (i < (num_of_packet-1)) {
      data_size = DATA_PACKET_SIZE;
    } else {
      data_size = CHUNK_SIZE % DATA_PACKET_SIZE;
    }
    fread(data, sizeof(uint8_t), data_size, f);
    packet_t *data_packet = build_data_packet(seq, data_size, data);
    g->upload_conn_pool[des_peer]->buffer[seq] = data_packet;
  }

  fclose(f);
  free(offset);
}

/**
 * Build ACK packet
 * @param ack ACK number.
 * @return  Built ACK packet
 */
packet_t *build_ack_packet(unsigned int ack) {
  packet_t *ack_packet = pkt_new();

  ack_packet->hdr->magic = htons(15441);
  ack_packet->hdr->version = 1;
  ack_packet->hdr->type = 4;                    //packet type: ACK
  ack_packet->hdr->hlen = htons(HDRSZ);
  ack_packet->hdr->plen = htons(HDRSZ);         //packet_len is header_len
  ack_packet->hdr->seqn = htonl(0);
  ack_packet->hdr->ackn = htonl((uint32_t)ack);

  return ack_packet;
}

/**
 * Send GET packet to designated peer.
 * The invoker of this function is the receiver in downloading process.
 * @param id designated peer's id.
 * @param get_packet GET packet
 * @param g global state to retrieve socket and peers info
 */
void send_packet(short id, packet_t *packet, g_state_t *g){
  bt_peer_t *peer = bt_peer_info(g->g_config, id);
  spiffy_sendto(g->peer_socket, packet->raw, ntohs(packet->hdr->plen),
                0, (struct sockaddr *)&(peer->addr), sizeof(peer->addr));
}

/**
 * Process incoming GET packet from peer.
 * The invoker of this function is the sender who uploads DATA packets.
 * @param g Global state.
 * @param get_packet GET packet.
 * @param from peer id.
 */
void process_get_packet(g_state_t *g, packet_t *get_packet, short from) {
  /* Retrieve hashed chunk */
  char *chunk_ptr = (char*)get_packet->payload;
  char chunk_hash[2*SHA1_HASH_SIZE+1];
  hex2ascii(chunk_ptr, SHA1_HASH_SIZE, chunk_hash);
  chunk_hash[2*SHA1_HASH_SIZE] = '\0';

  char *dummy;
  if (hashmap_get(g->g_config->chunks->has_chunk_map, chunk_hash, (any_t*)&dummy)
          == MAP_MISSING) {
    console_log("Peer %d: received GET packet for nl-chunk %s, drop it!",
                g->g_config->identity, chunk_hash);
    return;
  }

  console_log("Peer %d: received GET packet for chunk %s", g->g_config->identity, chunk_hash);

  if (g->upload_conn_pool[from] == NULL) {
    /* Establish new uploading connection with the requesting peer */
    console_log("Peer %d: ****** Establish new upload connection with peer %d ******",
                g->g_config->identity, from);
    init_send_window(g, from);
    build_chunk_data_packets(chunk_hash, g, from);
    console_log("Peer %d: DATA packets are built", g->g_config->identity);
  } else {
    /* There's existing uploading connection with the peer. Reject GET */
    console_log("Peer %d: Existing upload connection with peer %d, reject GET.",
                g->g_config->identity, from);
  }
}

/**
 * Process incoming DATA packet from peer.
 * The invoker of this function is the receiver who downloads DATA packets.
 * @param g Global state.
 * @param data_packet Received DATA packet.
 * @param from Uploading peer's id.
 */
void process_data_packet(g_state_t *g, packet_t *data_packet, short from) {
  if (g->download_conn_pool[from] == NULL) {
    /* First DATA packet received from this peer.
     * Establish downloading connection with this peer */
    init_recv_window(g, from);
    console_log("Peer %d: ****** Establish new download connection with peer %d ******",
                g->g_config->identity, from);
  }

  /* Make a local copy of DATA packet. Since argument <data_packet>  will
   * be freed outside this function */
  packet_t* data_packet_copy = pkt_new();
  memcpy(data_packet_copy, data_packet, sizeof(packet_t));

  recv_window_t *window = g->download_conn_pool[from];
  uint32_t seq_number = ntohl(data_packet_copy->hdr->seqn);
  window->buffer[seq_number] = data_packet_copy;

  if (seq_number == window->next_packet_expected) {

    /* Upon receiving next_packet_expected packet. Scan buffer to
     * update next_packet_expected.
     * e.g. If current next_packet_expected is 4, but packet 5 and 6 is
     * already in the buffer, then next_packet_expected should be set to 7
     * and should send ACK packet with ack number 6 (next_packet_expected-1).
     */

    while (window->buffer[window->next_packet_expected] != NULL)
      window->next_packet_expected++;
    packet_t *ack_packet = build_ack_packet(window->next_packet_expected-1);
    send_packet(from, ack_packet, g);
    console_log("Peer %d: Sent ACK %u back to peer %d",
                g->g_config->identity, window->next_packet_expected-1, from);
  } else if (seq_number > window->next_packet_expected){

    /* Upon receiving packet whose SEQ is larger than next_packet_expected,
     * it means currently there's a GAP in the buffer.
     * Should send duplicate ACK to sender to indicate this GAP.
     */

    packet_t *dup_ack_packet = build_ack_packet(window->next_packet_expected-1);
    send_packet(from, dup_ack_packet, g);
    console_log("Peer %d: Sent DUP ACK %u back to peer %d",
                g->g_config->identity, window->next_packet_expected-1, from);
  } else {
    /* next_packet_expected > SEQ. Discard it */
    console_log("Peer %d: Received SEQ is %u, smaller than expected (%u). Discard it!",
                g->g_config->identity, seq_number, window->next_packet_expected);
  }

//  console_log("Peer %d: current next_packet_expected is %u",
//              g->g_config->identity, window->next_packet_expected);
}

/**
 * Process incoming ACK packet from peer.
 * The invoker of this function is the sender who uploads DATA packets.
 * @param g Global state.
 * @param ack_packet Received ACK packet.
 * @param from Downloading peer's id.
 */
void process_ack_packet(g_state_t *g, packet_t *ack_packet, short from) {
  assert(g->upload_conn_pool[from] != NULL);

  uint32_t ack_number = htonl(ack_packet->hdr->ackn);
  send_window_t *window = g->upload_conn_pool[from];

  char ack_number_str[4];
  sprintf(ack_number_str, "%u", ack_number);
  any_t ack_cnt;
  if (hashmap_get(window->dup_ack, ack_number_str, &ack_cnt) == MAP_MISSING) {
    /* First appearance accumulative ACK.
     * It means all packet whose SEQ is less or equal to ACK is received by receiver.
     * Update last_packet_acked and last_packet_available to move forward.
     */

    /* Record this ACK for potential further ACK */
    ack_cnt = 1;
    hashmap_put(window->dup_ack, ack_number_str, ack_cnt);

    console_log("Peer %d: Received ACK %u from peer %d",
                g->g_config->identity, ack_number, from);
    window->last_packet_acked = ack_number;
    window->last_packet_available = window->last_packet_acked + window->max_window_size;
  } else {
    /* Duplicate ACK */
    if (ack_cnt == 1) {
      /* 2-time duplicate ACK. Ignore this case and update duplicate ACK table */
      ack_cnt = 2;
      hashmap_put(window->dup_ack, ack_number_str, ack_cnt);
    } else if (ack_cnt == 2) {
      /* 3-time duplicate ACK, which indicates lost packet whose SEQ == ACK.
       * Resend lost packet */
      console_log("Peer %d: Received DUP ACK %u from peer %d",
                  g->g_config->identity, ack_number, from);
      send_packet(from, window->buffer[ack_number], g);
    } else {
      /* Control flow should never reach here */
    }
  }
}

/**
 * Handle uploading.
 * This function will be invoked periodically in the peer's main event loop
 * @param g Global state.
 */
void do_upload(g_state_t *g) {
  int i;
  for (i = 0; i < MAX_PEER_NUM; i++) {
    if (g->upload_conn_pool[i] != NULL) {
      send_window_t * window= g->upload_conn_pool[i];

      /* Keep sending packets until last_packet_sent is equal to last_packet_available. */
      while (window->last_packet_sent < window->last_packet_available) {
        send_packet(i, window->buffer[++window->last_packet_sent], g);
      }
    }
  }
}

/**
 * Handle downloading.
 * This function will be invoked periodically in the peer's main event loop
 * @param g Global state.
 */
void do_download(g_state_t *g) {
}