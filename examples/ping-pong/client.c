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

#include <quiche.h>

#include <uthash.h>

//----------------------------------
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

#define MAX_BLOCK_SIZE 65535*10
#define MAX_FILE_PATH_LEN 1024
typedef struct transport_file_parameters {
    uint8_t local_file_path[MAX_FILE_PATH_LEN];
    uint8_t remote_file_path[MAX_FILE_PATH_LEN];
    char *pdata;
    unsigned int data_len;
    int need_blocks_num;
} TFPs;

typedef struct send_data_struct {  
    int id;                    /* key */  
    char *value;  
    unsigned int value_len;
    UT_hash_handle hh;         /* makes this structure hashable */  
}SEND_STRUCT;  

struct conn_io {
  ev_timer timer;

  int sock;

  quiche_conn *conn;

  TFPs tfps;
};
SEND_STRUCT *cache_send_data = NULL;  
//********************
//打印提示信息
void help_print(void);

//计算出需要多少个块
static int calculate_need_blocks_num(int file_data_len, int max_blocks_size);

//获得读取文件内容的长度
static int get_file_data_len(char *file_path);

//将数据从本地读取出来
int read_all_contents_from_file(char *file_path, char *file_data,unsigned int file_data_len);

//在头部添加指定的路径信息
static int add_remote_filename_in_head(char *buff, char *filename);

//计算还有多少个数据没有读取，大于块按块大小读取，否则按剩余大小读取
int calculate_readable_size_from_file(int file_data_len, int readed_data_len,
                                int max_block_size);
void send_all_data(struct conn_io *conn_io);
void cache_file_data_to_cache_table(char * remote_file_path,char * data,unsigned int data_len);

void add_stream_data_to_table(int mykey, char *value,int value_len); 

SEND_STRUCT *find_stream_data_by_streamid(int mykey);

void delete_cache_stream_data(SEND_STRUCT *user);
void delete_all_cache_stream_data(); 

void print_all_chahe_stream_data();
int id_sort(SEND_STRUCT*a, SEND_STRUCT*b);
void sort_streams_datas_by_streamid();
void print_all_chahe_stream_id();
//----------------------------------

#define LOCAL_CONN_ID_LEN 16

#define MAX_DATAGRAM_SIZE 1350



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

    ssize_t sent = send(conn_io->sock, out, written, 0);
    if (sent != written) {
      perror("failed to send");
      return;
    }

    //fprintf(stderr, "sent %zd bytes\n", sent);
  }

  double t = quiche_conn_timeout_as_nanos(conn_io->conn) / 1e9f;
  conn_io->timer.repeat = t;
  ev_timer_again(loop, &conn_io->timer);
}

static void recv_cb(EV_P_ ev_io *w, int revents) {
  static bool req_sent = false;

  struct conn_io *conn_io = w->data;

  static uint8_t buf[65535];

  while (1) {
    ssize_t read = recv(conn_io->sock, buf, sizeof(buf), 0);

    if (read < 0) {
      if ((errno == EWOULDBLOCK) || (errno == EAGAIN)) {
        fprintf(stderr, "recv would block\n");
        break;
      }

      perror("failed to read");
      return;
    }

    ssize_t done = quiche_conn_recv(conn_io->conn, buf, read);

    if (done < 0) {
      fprintf(stderr, "failed to process packet\n");
      continue;
    }

    fprintf(stderr, "recv %zd bytes\n", done);
  }

  fprintf(stderr, "done reading\n");

  if (quiche_conn_is_closed(conn_io->conn)) {
    fprintf(stderr, "connection closed\n");

    ev_break(EV_A_ EVBREAK_ONE);
    return;
  }

  if (quiche_conn_is_established(conn_io->conn) && !req_sent) {
    const uint8_t *app_proto;
    size_t app_proto_len;

    quiche_conn_application_proto(conn_io->conn, &app_proto, &app_proto_len);

    fprintf(stderr, "connection established: %.*s\n", (int)app_proto_len,
            app_proto);
    //发送数据
    //print_all_chahe_stream_id();
    send_all_data(conn_io);
    //发送完成清空 HASH表
    delete_all_cache_stream_data();
    printf("sent over\n");
    req_sent = true;
  }

  if (quiche_conn_is_established(conn_io->conn)) {
    uint64_t s = 0;

    quiche_stream_iter *readable = quiche_conn_readable(conn_io->conn);

    while (quiche_stream_iter_next(readable, &s)) {
      fprintf(stderr, "stream %" PRIu64 " is readable\n", s);

      bool fin = false;
      ssize_t recv_len =
          quiche_conn_stream_recv(conn_io->conn, s, buf, sizeof(buf), &fin);
      if (recv_len < 0) {
        break;
      }

      printf("recv: %.*s", (int)recv_len, buf);

      
      // if (fin) {
      //   if (quiche_conn_close(conn_io->conn, true, 0, NULL, 0) < 0) {
      //     fprintf(stderr, "failed to close connection\n");
      //   }
      // }
    }

    quiche_stream_iter_free(readable);
  }
  flush_egress(loop, conn_io);



}

static void timeout_cb(EV_P_ ev_timer *w, int revents) {
  struct conn_io *conn_io = w->data;
  quiche_conn_on_timeout(conn_io->conn);

  fprintf(stderr, "timeout\n");

  flush_egress(loop, conn_io);

  if (quiche_conn_is_closed(conn_io->conn)) {
    quiche_stats stats;

    quiche_conn_stats(conn_io->conn, &stats);
    delete_all_cache_stream_data();
    fprintf(stderr,
            "connection closed, recv=%zu sent=%zu lost=%zu rtt=%" PRIu64 "ns\n",
            stats.recv, stats.sent, stats.lost, stats.rtt);

    ev_break(EV_A_ EVBREAK_ONE);
    return;
  }
}

int main(int argc, char *argv[]) {
  const char *host = argv[1];
  const char *port = argv[2];
  //输入命令打印提示
  if (argc < 5) {
        help_print();
        return -1;
    }

  const struct addrinfo hints = {.ai_family = PF_UNSPEC,
                                 .ai_socktype = SOCK_DGRAM,
                                 .ai_protocol = IPPROTO_UDP};

  quiche_enable_debug_logging(debug_log, NULL);

  struct addrinfo *peer;
  if (getaddrinfo(host, port, &hints, &peer) != 0) {
    perror("failed to resolve host");
    return -1;
  }

  int sock = socket(peer->ai_family, SOCK_DGRAM, 0);
  if (sock < 0) {
    perror("failed to create socket");
    return -1;
  }

  if (fcntl(sock, F_SETFL, O_NONBLOCK) != 0) {
    perror("failed to make socket non-blocking");
    return -1;
  }

  if (connect(sock, peer->ai_addr, peer->ai_addrlen) < 0) {
    perror("failed to connect socket");
    return -1;
  }

  quiche_config *config = quiche_config_new(0xbabababa);
  if (config == NULL) {
    fprintf(stderr, "failed to create config\n");
    return -1;
  }

  quiche_config_set_application_protos(
      config, (uint8_t *)"\x05hq-28\x05hq-27\x08http/0.9", 21);

  quiche_config_set_max_idle_timeout(config, 5000);
  quiche_config_set_max_packet_size(config, MAX_DATAGRAM_SIZE);
  quiche_config_set_initial_max_data(config, 10000000);
  quiche_config_set_initial_max_stream_data_bidi_local(config, 1000000);
  quiche_config_set_initial_max_stream_data_uni(config, 1000000);
  quiche_config_set_initial_max_streams_bidi(config, 100);
  quiche_config_set_initial_max_streams_uni(config, 100);
  quiche_config_set_disable_active_migration(config, true);

  if (getenv("SSLKEYLOGFILE")) {
    quiche_config_log_keys(config);
  }

  uint8_t scid[LOCAL_CONN_ID_LEN];
  int rng = open("/dev/urandom", O_RDONLY);
  if (rng < 0) {
    perror("failed to open /dev/urandom");
    return -1;
  }

  ssize_t rand_len = read(rng, &scid, sizeof(scid));
  if (rand_len < 0) {
    perror("failed to create connection ID");
    return -1;
  }

  quiche_conn *conn =
      quiche_connect(host, (const uint8_t *)scid, sizeof(scid), config);
  if (conn == NULL) {
    fprintf(stderr, "failed to create connection\n");
    return -1;
  }

  struct conn_io *conn_io = malloc(sizeof(*conn_io));
  if (conn_io == NULL) {
    fprintf(stderr, "failed to allocate connection IO\n");
    return -1;
  }

  conn_io->sock = sock;
  conn_io->conn = conn;
  snprintf((char *)conn_io->tfps.local_file_path,
            sizeof(conn_io->tfps.local_file_path), "%s", argv[3]);
    snprintf((char *)conn_io->tfps.remote_file_path,
            sizeof(conn_io->tfps.remote_file_path), "%s", argv[4]);
  conn_io->tfps.data_len = get_file_data_len((char *)conn_io->tfps.local_file_path);
    WZX_DEBUG("file_data_len %d", conn_io->tfps.data_len);
    conn_io->tfps.pdata = (char *)calloc( 1, conn_io->tfps.data_len + 1);  //+1的目的 内存末尾存放'\0'
    if (conn_io->tfps.pdata == NULL) {
        return 0;
    }
    read_all_contents_from_file((char *)conn_io->tfps.local_file_path,
                                (char *)conn_io->tfps.pdata,conn_io->tfps.data_len);
    // WZX_DEBUG("File contents %s",conn_io->tfps.pdata);
    cache_file_data_to_cache_table(conn_io->tfps.remote_file_path,conn_io->tfps.pdata,conn_io->tfps.data_len);
    //print_all_chahe_stream_data();
    sort_streams_datas_by_streamid();
  ev_io watcher;

  struct ev_loop *loop = ev_default_loop(0);

  ev_io_init(&watcher, recv_cb, conn_io->sock, EV_READ);
  ev_io_start(loop, &watcher);
  watcher.data = conn_io;

  ev_init(&conn_io->timer, timeout_cb);
  conn_io->timer.data = conn_io;

  flush_egress(loop, conn_io);

  ev_loop(loop, 0);

  freeaddrinfo(peer);

  quiche_conn_free(conn);

  quiche_config_free(config);

  return 0;
}

//-------------------------------------------
//计算出需要多少个块
static int calculate_need_blocks_num(int file_data_len, int max_blocks_size) {
    WZX_DEBUG("len: %d %d \n", file_data_len, max_blocks_size);
    int ret = 0;
    ret = file_data_len / max_blocks_size;
    if (file_data_len % max_blocks_size != 0) {
        ret += 1;
    }
    return ret;
}

//获得读取文件内容的长度
static int get_file_data_len(char *file_path) {
    if (file_path == NULL) {
        fprintf(stderr, "error: Input arguments are NULL\n");
        return -1;
    }
    FILE *fp = NULL;
    unsigned long file_len = 0;
    fp = fopen(file_path, "r");
    if (fp == NULL) {
        fprintf(stderr, "error: open %s file error\n", file_path);
        return -1;
    }
    fseek(fp, 0, 2);
    file_len = ftell(fp);
    rewind(fp);
    fclose(fp);
    return file_len;
}

//将数据从本地读取出来
int read_all_contents_from_file(char *file_path, char *file_data,unsigned int file_data_len) {
    FILE *fp = NULL;
    fp = fopen(file_path, "r");
    if (fp == NULL) {
        perror("fopen");
        return -1;
    }

    fread(file_data, file_data_len, 1, fp);
    fclose(fp);
    return 0;
}

//在头部添加指定的路径信息
static int add_remote_filename_in_head(char *buff, char *filename) {
    if (buff == NULL || filename == NULL) {
        fprintf(stderr, "error: input arguments error\n");
        return -1;
    }
    sprintf(buff, "%s&", filename);
    WZX_DEBUG("add file name at head len %ld  %s", strlen(buff), buff);
    return strlen(filename) + 1;
}

//计算还有多少个数据没有读取，大于块按块大小读取，否则按剩余大小读取
int calculate_readable_size_from_file(int file_data_len, int readed_data_len,
                                int max_block_size) {
    int read_size = 0;
    if (file_data_len - readed_data_len > max_block_size) {
        read_size = max_block_size;
    } else {
        read_size = file_data_len - readed_data_len;
    }
    return read_size;
}
//打印输出信息
void help_print(void) {
    printf(
        "Notes:input format error.\n"
        "Please try again like that :ip port local_file_path remote_file_pathe\n");
}

void add_stream_data_to_table(int mykey, char *value,int value_len) {  
    SEND_STRUCT *s;  
  
    HASH_FIND_INT(cache_send_data, &mykey, s);  /* mykey already in the hash? */  
    if (s==NULL) {  
      s = (SEND_STRUCT*)malloc(sizeof(SEND_STRUCT));  
      s->id = mykey; 
      s->value = (char *)calloc(1,value_len); 
      s->value_len = value_len;
      HASH_ADD_INT( cache_send_data, id, s );  /* id: name of key field */  
    }  
    strncpy(s->value,value,value_len); 
}  

void cache_file_data_to_cache_table(char * remote_file_path,char * data,unsigned int data_len){
  if(data == NULL||remote_file_path == NULL){
  fprintf(stderr,"input args is NULL\n");
  return;
  }
  unsigned int need_blocks_num = calculate_need_blocks_num(data_len,MAX_BLOCK_SIZE);
  unsigned int readed_data_record = 0;
  int readable_len = 0;
  int i = 0;

  add_stream_data_to_table(i*4,remote_file_path,strlen(remote_file_path));

  for(i = 1 ;i <= need_blocks_num;++i){
    readable_len = calculate_readable_size_from_file(data_len,readed_data_record,MAX_BLOCK_SIZE);
      add_stream_data_to_table(i*4,data+readed_data_record,readable_len);
      readed_data_record+=readable_len;
  }
}

SEND_STRUCT *find_stream_data_by_streamid(int mykey) {  
    SEND_STRUCT *s;  
  
    HASH_FIND_INT( cache_send_data, &mykey, s );  /* s: output pointer */  
    return s;  
}  
  
void delete_cache_stream_data(SEND_STRUCT *user) {  
    HASH_DEL( cache_send_data, user);  /* user: pointer to deletee */  
    free(user->value);
    free(user);  
}  
void delete_all_cache_stream_data() {  
  SEND_STRUCT *current_user, *tmp;  
  
  HASH_ITER(hh, cache_send_data, current_user, tmp) { 
    free(current_user->value);
    HASH_DEL(cache_send_data,current_user);  /* delete it (users advances to next) */  
    free(current_user);            /* free it */  
  }  
}  
  
void print_all_chahe_stream_data() {  
    SEND_STRUCT *s;  
  
    for(s=cache_send_data; s != NULL; s=(SEND_STRUCT*)(s->hh.next)) {  
        printf("-->user id %d: value %s\n", s->id, s->value);  
    }  
}  

void print_all_chahe_stream_id() {  
    SEND_STRUCT *s;  
  
    for(s=cache_send_data; s != NULL; s=(SEND_STRUCT*)(s->hh.next)) {  
        printf("-->user id %d\n", s->id);  
    }  
}  
int id_sort(SEND_STRUCT*a, SEND_STRUCT*b) {
    return (a->id - b->id);
}
void sort_streams_datas_by_streamid() {  
    HASH_SORT(cache_send_data, id_sort);  
}  


void send_all_data(struct conn_io *conn_io){
  SEND_STRUCT *s;  
  
  long int send_all_data = 0;
  int i=1;
    for(s=cache_send_data; s != NULL; s=(SEND_STRUCT*)(s->hh.next)) {

      send_all_data+= s->value_len;
       //printf("SEND is %d\n",s->id);quiche_conn_stream_send(conn_io->conn,s->id,s->value, s->value_len, 1)
        if ( quiche_conn_stream_send_full(conn_io->conn,s->id,s->value,s->value_len,1,55555,i++,0)< 0) {
          fprintf(stderr, "failed to send %d \n",s->id);
          return;
        }
     // WZX_DEBUG("%d %.*s\n",s->id,s->value_len,s->value);
      
    }  
    //printf("SEND all1 %ld\n",send_all_data);

    
}