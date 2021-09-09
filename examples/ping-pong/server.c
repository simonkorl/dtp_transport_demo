// Copyright (C) 2018-2019, Cloudflare, Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR
// CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
// EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
// PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
// PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
// LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
// NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
// SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <errno.h>
#include <fcntl.h>

#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <ev.h>
#include <uthash.h>

#include <quiche.h>



#define LOCAL_CONN_ID_LEN 16

#define MAX_DATAGRAM_SIZE 1350
#define MAX_BLOCK_SIZE 65535*10
#define MAX_TOKEN_LEN                                                          \
  sizeof("quiche") - 1 + sizeof(struct sockaddr_storage) +                     \
      QUICHE_MAX_CONN_ID_LEN

//-----------------------------
#define DEBUG 1
#if DEBUG
#define WZX_DEBUG(format, args...)                                            \
    do {                                                                      \
        fprintf(stderr, "GG-->>> %s->%s()->line.%d : " format "\n", __FILE__, \
                __FUNCTION__, __LINE__, ##args);                              \
    } while (0)
#else
#define WZX_DEBUG(format, args...) \
    do {                           \
    } while (0)
#endif
#define MAX_FILE_PATH_LEN 1024
typedef struct cache_recv_streams_data {
    uint64_t id;                    /* key */
    char *value;
    uint64_t value_len;
    UT_hash_handle hh;         /* makes this structure hashable */
}STREAMS_DATA;

STREAMS_DATA *cache_streams_data_table = NULL;

void add_stream_data_to_table(int mykey, char *value,int value_len);
STREAMS_DATA *find_stream_data_by_streamid(int mykey);
void delete_cache_stream_data(STREAMS_DATA *user);
void delete_all_cache_stream_data();
void print_all_chahe_stream_data();
void print_all_chahe_stream_id();
void sort_streams_datas_by_streamid();
void save_recv_data_to_file();
//-----------------------------
struct connections {
  int sock;

  struct conn_io *h;
};

struct conn_io {
  ev_timer timer;

  int sock;

  uint8_t cid[LOCAL_CONN_ID_LEN];

  quiche_conn *conn;

  struct sockaddr_storage peer_addr;
  socklen_t peer_addr_len;

  UT_hash_handle hh;
};

static quiche_config *config = NULL;

static struct connections *conns = NULL;

static void timeout_cb(EV_P_ ev_timer *w, int revents);

static void debug_log(const char *line, void *argp) {
  fprintf(stderr, "%s\n", line);
}

static void flush_egress(struct ev_loop *loop, struct conn_io *conn_io) {
  static uint8_t out[MAX_DATAGRAM_SIZE];

  while (1) {
    ssize_t written = quiche_conn_send(conn_io->conn, out, sizeof(out));

    if (written == QUICHE_ERR_DONE) {
      fprintf(stderr, "done writing\n");
      break;
    }

    if (written < 0) {
      fprintf(stderr, "failed to create packet: %zd\n", written);
      return;
    }

    ssize_t sent =
        sendto(conn_io->sock, out, written, 0,
               (struct sockaddr *)&conn_io->peer_addr, conn_io->peer_addr_len);
    if (sent != written) {
      perror("failed to send");
      return;
    }

    fprintf(stderr, "sent %zd bytes\n", sent);
  }

  double t = quiche_conn_timeout_as_nanos(conn_io->conn) / 1e9f;
  conn_io->timer.repeat = t;
  ev_timer_again(loop, &conn_io->timer);
}

static void mint_token(const uint8_t *dcid, size_t dcid_len,
                       struct sockaddr_storage *addr, socklen_t addr_len,
                       uint8_t *token, size_t *token_len) {
  memcpy(token, "quiche", sizeof("quiche") - 1);
  memcpy(token + sizeof("quiche") - 1, addr, addr_len);
  memcpy(token + sizeof("quiche") - 1 + addr_len, dcid, dcid_len);

  *token_len = sizeof("quiche") - 1 + addr_len + dcid_len;
}

static bool validate_token(const uint8_t *token, size_t token_len,
                           struct sockaddr_storage *addr, socklen_t addr_len,
                           uint8_t *odcid, size_t *odcid_len) {
  if ((token_len < sizeof("quiche") - 1) ||
      memcmp(token, "quiche", sizeof("quiche") - 1)) {
    return false;
  }

  token += sizeof("quiche") - 1;
  token_len -= sizeof("quiche") - 1;

  if ((token_len < addr_len) || memcmp(token, addr, addr_len)) {
    return false;
  }

  token += addr_len;
  token_len -= addr_len;

  if (*odcid_len < token_len) {
    return false;
  }

  memcpy(odcid, token, token_len);
  *odcid_len = token_len;

  return true;
}

static struct conn_io *create_conn(uint8_t *odcid, size_t odcid_len) {
  struct conn_io *conn_io = malloc(sizeof(*conn_io));
  if (conn_io == NULL) {
    fprintf(stderr, "failed to allocate connection IO\n");
    return NULL;
  }

  int rng = open("/dev/urandom", O_RDONLY);
  if (rng < 0) {
    perror("failed to open /dev/urandom");
    return NULL;
  }

  ssize_t rand_len = read(rng, conn_io->cid, LOCAL_CONN_ID_LEN);
  if (rand_len < 0) {
    perror("failed to create connection ID");
    return NULL;
  }

  quiche_conn *conn =
      quiche_accept(conn_io->cid, LOCAL_CONN_ID_LEN, odcid, odcid_len, config);
  if (conn == NULL) {
    fprintf(stderr, "failed to create connection\n");
    return NULL;
  }

  conn_io->sock = conns->sock;
  conn_io->conn = conn;

  ev_init(&conn_io->timer, timeout_cb);
  conn_io->timer.data = conn_io;

  HASH_ADD(hh, conns->h, cid, LOCAL_CONN_ID_LEN, conn_io);

  fprintf(stderr, "new connection\n");

  return conn_io;
}

static void recv_cb(EV_P_ ev_io *w, int revents) {
  struct conn_io *tmp, *conn_io = NULL;

  static uint8_t buf[MAX_BLOCK_SIZE];
  static uint8_t out[MAX_DATAGRAM_SIZE];

  while (1) {
    struct sockaddr_storage peer_addr;
    socklen_t peer_addr_len = sizeof(peer_addr);
    memset(&peer_addr, 0, peer_addr_len);

    ssize_t read = recvfrom(conns->sock, buf, sizeof(buf), 0,
                            (struct sockaddr *)&peer_addr, &peer_addr_len);

    if (read < 0) {
      if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
        fprintf(stderr, "recv would block\n");
        break;
      }

      perror("failed to read");
      return;
    }

    uint8_t type;
    uint32_t version;

    uint8_t scid[QUICHE_MAX_CONN_ID_LEN];
    size_t scid_len = sizeof(scid);

    uint8_t dcid[QUICHE_MAX_CONN_ID_LEN];
    size_t dcid_len = sizeof(dcid);

    uint8_t odcid[QUICHE_MAX_CONN_ID_LEN];
    size_t odcid_len = sizeof(odcid);

    uint8_t token[MAX_TOKEN_LEN];
    size_t token_len = sizeof(token);

    int rc =
        quiche_header_info(buf, read, LOCAL_CONN_ID_LEN, &version, &type, scid,
                           &scid_len, dcid, &dcid_len, token, &token_len);
    if (rc < 0) {
      fprintf(stderr, "failed to parse header: %d\n", rc);
      continue;
    }

    HASH_FIND(hh, conns->h, dcid, dcid_len, conn_io);

    if (conn_io == NULL) {
      if (!quiche_version_is_supported(version)) {
        fprintf(stderr, "version negotiation\n");

        ssize_t written = quiche_negotiate_version(scid, scid_len, dcid,
                                                   dcid_len, out, sizeof(out));

        if (written < 0) {
          fprintf(stderr, "failed to create vneg packet: %zd\n", written);
          continue;
        }

        ssize_t sent = sendto(conns->sock, out, written, 0,
                              (struct sockaddr *)&peer_addr, peer_addr_len);
        if (sent != written) {
          perror("failed to send");
          continue;
        }

        fprintf(stderr, "sent %zd bytes\n", sent);
        continue;
      }

      if (token_len == 0) {
        fprintf(stderr, "stateless retry\n");

        mint_token(dcid, dcid_len, &peer_addr, peer_addr_len, token,
                   &token_len);

        ssize_t written =
            quiche_retry(scid, scid_len, dcid, dcid_len, dcid, dcid_len, token,
                         token_len, out, sizeof(out));

        if (written < 0) {
          fprintf(stderr, "failed to create retry packet: %zd\n", written);
          continue;
        }

        ssize_t sent = sendto(conns->sock, out, written, 0,
                              (struct sockaddr *)&peer_addr, peer_addr_len);
        if (sent != written) {
          perror("failed to send");
          continue;
        }

        fprintf(stderr, "sent %zd bytes\n", sent);
        continue;
      }

      if (!validate_token(token, token_len, &peer_addr, peer_addr_len, odcid,
                          &odcid_len)) {
        fprintf(stderr, "invalid address validation token\n");
        continue;
      }

      conn_io = create_conn(odcid, odcid_len);
      if (conn_io == NULL) {
        continue;
      }

      memcpy(&conn_io->peer_addr, &peer_addr, peer_addr_len);
      conn_io->peer_addr_len = peer_addr_len;
    }

    ssize_t done = quiche_conn_recv(conn_io->conn, buf, read);

    if (done < 0) {
      fprintf(stderr, "failed to process packet: %zd\n", done);
      continue;
    }

    //fprintf(stderr, "recv %zd bytes\n", done);

    if (quiche_conn_is_established(conn_io->conn)) {
      uint64_t s = 0;

      quiche_stream_iter *readable = quiche_conn_readable(conn_io->conn);
uint64_t block_buf[MAX_BLOCK_SIZE] = {0};
uint64_t block_priority = 0;
uint64_t block_deadline = 0;

      while (quiche_stream_iter_next(readable, &s)) {
        //printf("stream %" PRIu64 " is readable\n", s);

        bool fin = false;
        ssize_t recv_len =quiche_conn_stream_recv(conn_io->conn, s, buf, sizeof(buf), &fin);
        if (recv_len < 0) {
          break;
        }
        add_stream_data_to_table(s,buf,recv_len);
        if (fin) {
          //static const char *resp = "byez\n";
          //WZX_DEBUG("END------>fin %ld\n",s);
          //quiche_conn_stream_send(conn_io->conn, s, (uint8_t *)resp, 5, true);
          //printf("send: %s", resp);
        }
      }

      quiche_stream_iter_free(readable);

    } 

    
  }



  HASH_ITER(hh, conns->h, conn_io, tmp) {
    flush_egress(loop, conn_io);

    if (quiche_conn_is_closed(conn_io->conn)) {
WZX_DEBUG("CLOSE------>");
      quiche_stats stats;

 
      quiche_conn_stats(conn_io->conn, &stats);
      fprintf(stderr,
              "connection closed, recv=%zu sent=%zu lost=%zu rtt=%" PRIu64
              "ns cwnd=%zu\n",
              stats.recv, stats.sent, stats.lost, stats.rtt, stats.cwnd);

      HASH_DELETE(hh, conns->h, conn_io);

      ev_timer_stop(loop, &conn_io->timer);
      quiche_conn_free(conn_io->conn);
      free(conn_io);
    }
  }
}

static void timeout_cb(EV_P_ ev_timer *w, int revents) {
  struct conn_io *conn_io = w->data;
  quiche_conn_on_timeout(conn_io->conn);

  fprintf(stderr, "timeout\n");

  flush_egress(loop, conn_io);

  if (quiche_conn_is_closed(conn_io->conn)) {
    quiche_stats stats;

    quiche_conn_stats(conn_io->conn, &stats);
    fprintf(stderr,
            "connection closed, recv=%zu sent=%zu lost=%zu rtt=%" PRIu64
            "ns cwnd=%zu\n",
            stats.recv, stats.sent, stats.lost, stats.rtt, stats.cwnd);

    HASH_DELETE(hh, conns->h, conn_io);
 //存储到文件
        sort_streams_datas_by_streamid();
        save_recv_data_to_file();
        delete_all_cache_stream_data();
    printf("recv success!\n");
    ev_timer_stop(loop, &conn_io->timer);
    quiche_conn_free(conn_io->conn);
    free(conn_io);

    return;
  }
}

int main(int argc, char *argv[]) {
  const char *host = argv[1];
  const char *port = argv[2];

  const struct addrinfo hints = {.ai_family = PF_UNSPEC,
                                 .ai_socktype = SOCK_DGRAM,
                                 .ai_protocol = IPPROTO_UDP};

  quiche_enable_debug_logging(debug_log, NULL);

  struct addrinfo *local;
  if (getaddrinfo(host, port, &hints, &local) != 0) {
    perror("failed to resolve host");
    return -1;
  }

  int sock = socket(local->ai_family, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("failed to create socket");
    return -1;
  }

  if (fcntl(sock, F_SETFL, O_NONBLOCK) != 0) {
    perror("failed to make socket non-blocking");
    return -1;
  }

  if (bind(sock, local->ai_addr, local->ai_addrlen) < 0) {
    perror("failed to connect socket");
    return -1;
  }

  config = quiche_config_new(QUICHE_PROTOCOL_VERSION);
  if (config == NULL) {
    fprintf(stderr, "failed to create config\n");
    return -1;
  }

  quiche_config_load_cert_chain_from_pem_file(config, "./cert.crt");
  quiche_config_load_priv_key_from_pem_file(config, "./cert.key");

  quiche_config_set_application_protos(
      config, (uint8_t *)"\x05hq-28\x05hq-27\x08http/0.9", 21);

  quiche_config_set_max_idle_timeout(config, 5000);
  quiche_config_set_max_packet_size(config, MAX_DATAGRAM_SIZE);
  quiche_config_set_initial_max_data(config, 10000000);
  quiche_config_set_initial_max_stream_data_bidi_local(config, 1000000);
  quiche_config_set_initial_max_stream_data_bidi_remote(config, 1000000);
  quiche_config_set_initial_max_streams_bidi(config, 100);
  quiche_config_set_cc_algorithm(config, QUICHE_CC_RENO);

  struct connections c;
  c.sock = sock;
  c.h = NULL;

  conns = &c;

  ev_io watcher;

  struct ev_loop *loop = ev_default_loop(0);

  ev_io_init(&watcher, recv_cb, sock, EV_READ);
  ev_io_start(loop, &watcher);
  watcher.data = &c;

  ev_loop(loop, 0);

  freeaddrinfo(local);

  quiche_config_free(config);

  return 0;
}

//
void add_stream_data_to_table(int mykey, char *value,int value_len) {  
  if(value == NULL)
  {
    printf("input error \n");
    return;
  }
    STREAMS_DATA *s;  
    HASH_FIND_INT(cache_streams_data_table, &mykey, s);  /* mykey already in the hash? */  
    if (s==NULL) {  
      //printf("key --->%d don't exit\n",mykey);
      s = (STREAMS_DATA*)malloc(sizeof(STREAMS_DATA));  
      if(s == NULL){
        printf("calloc error s add_stream_data_to_table\n");
      }
      s->id = mykey; 
      s->value = (char *)calloc(1,MAX_BLOCK_SIZE+1); 
      if(s->value == NULL)
      {
          printf("calloc error int add_stream_data_to_table\n");

      }
      s->value_len = value_len;
      if(s->value == NULL){
        printf("calloc error int add_stream_data_to_table\n");
      } 
      HASH_ADD_INT( cache_streams_data_table, id, s );  /* id: name of key field */   
      memcpy(s->value,value,value_len);
    }else{
      sprintf(s->value,"%s%.*s",s->value,value_len,value);
      s->value_len+=value_len;
    }
}  

STREAMS_DATA *find_stream_data_by_streamid(int mykey) {  
    STREAMS_DATA *s;  
    HASH_FIND_INT( cache_streams_data_table, &mykey, s );  /* s: output pointer */  
    return s;  
}  
  
void delete_cache_stream_data(STREAMS_DATA *user) {  
    HASH_DEL( cache_streams_data_table, user);  /* user: pointer to deletee */  
    free(user->value);
    free(user);  
}   

void delete_all_cache_stream_data() {  
  STREAMS_DATA *current_user, *tmp;  
  HASH_ITER(hh, cache_streams_data_table, current_user, tmp) {  
    HASH_DEL(cache_streams_data_table,current_user);  /* delete it (users advances to next) */  
    free(current_user);            /* free it */  
  }  
}  
  
void print_all_chahe_stream_data() {  
    STREAMS_DATA *s;  
  
    for(s=cache_streams_data_table; s != NULL; s=(STREAMS_DATA*)(s->hh.next)) {  
        printf("-->user id %ld: value %s\n", s->id, s->value);  
    }  
}  

void print_all_chahe_stream_id() {  
    STREAMS_DATA *s;  
  
    for(s=cache_streams_data_table; s != NULL; s=(STREAMS_DATA*)(s->hh.next)) {  
        printf("-->user id %ld\n", s->id);  
    }  
}  
int id_sort(STREAMS_DATA*a, STREAMS_DATA*b) {
    return (a->id - b->id);
}
void sort_streams_datas_by_streamid() {  
    HASH_SORT(cache_streams_data_table, id_sort);  
}  




void save_recv_data_to_file() {
    
    static bool get_file_path_flag = false;
    FILE *fp = NULL;

  STREAMS_DATA *s_temp = find_stream_data_by_streamid(0);
  if(s_temp==NULL)
  {
    printf("0 stream not find\n");
    return;
  } 
  
  fp = fopen(s_temp->value, "a+");
  if (fp == NULL) {
      fprintf(stderr,"open %s file error\n","test_rest.txt");
      return;
  }
      STREAMS_DATA *s;  

  //print_all_chahe_stream_id();
  for(s=(STREAMS_DATA*)cache_streams_data_table->hh.next; s != NULL; s=(STREAMS_DATA*)(s->hh.next)){
        fwrite(s->value,(int)s->value_len,1,fp);
      }
    if(fp != NULL){
      fclose(fp);
    }
} 