#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>
#include <time.h>

#include <ev.h>
#include "ebb.h"

#include "hiredis.h"
#include "async.h"
#include "adapters/libev.h"

static const char OUT1[] = "HTTP/1.0 302 Moved Temporarily\r\nLocation: http://production.cf.rubygems.org/gems/";
static const char OUT2[] = ".gem\r\n\r\n";

struct conn {
  struct ev_loop* loop;
  char full_name[256];
  size_t full_name_size;

  char output[1024];
};

struct serv {
  char* host;
  struct ev_loop* loop;
  redisAsyncContext* redis;
  ev_timer reconnect_timer;
};

void on_close(ebb_connection *connection) {
  free(connection->data);
  free(connection);
}

static void continue_responding(ebb_connection *connection) {
  int r;
  struct conn *data = connection->data;
  //printf("response complete \n");
  ebb_connection_schedule_close(connection);
}

/*
        commands = [
          %W!incr downloads!,
          %W!incr downloads:rubygem:#{name}!,
          %W!incr downloads:version:#{full_name}!,

          %W!zincrby downloads:today:#{today} 1 #{full_name}!,
          %W!zincrby downloads:all 1 #{full_name}!,

          %W!hincrby downloads:version_history:#{full_name} #{today} 1!,
          %W!hincrby downloads:rubygem_history:#{name} #{today} 1!
        ]
 */ 

static void get_full_name(redisAsyncContext *c, void *r, void *privdata) {
  redisReply *reply = r;
  if(!reply) return;

  char* name = reply->str;

  if(!name) return;
  char* full_name = (char*)privdata;

  time_t now;
  struct tm now_tm;
  char today[256];

  time(&now);
  gmtime_r(&now, &now_tm);
  strftime(today, sizeof(today), "%Y-%m-%d", &now_tm);

  redisAsyncCommand(c, 0, 0, "INCR downloads");
  redisAsyncCommand(c, 0, 0, "INCR downloads:rubygem:%s", name);
  redisAsyncCommand(c, 0, 0, "INCR downloads:version:%s", full_name);

  redisAsyncCommand(c, 0, 0, "ZINCRBY downloads:today:%s 1 %s",
                    today, full_name);
  redisAsyncCommand(c, 0, 0, "ZINCRBY downloads:all 1 %s",full_name);

  redisAsyncCommand(c, 0, 0, "HINCRBY downloads:version_history:%s %s 1",
                    full_name, today);
  redisAsyncCommand(c, 0, 0, "HINCRBY downloads:rubygem_history:%s %s 1",
                    name, today);

  free(privdata);
}

static void request_complete(ebb_request *request) {
  //printf("request complete \n");
  ebb_connection *connection = request->data;
  struct conn *data = connection->data;

  char* out = data->output;
  size_t total = 0;

  const size_t sz1 = sizeof(OUT1) - 1;

  memcpy(out, OUT1, sz1);
  out += sz1;

  memcpy(out, data->full_name, data->full_name_size);
  out += data->full_name_size;

  const size_t sz2 = sizeof(OUT2) - 1;
  memcpy(out, OUT2, sz2);

  out = data->output;
  total = sz1 + data->full_name_size + sz2;

  out[total] = 0;

  ebb_connection_write(connection, out, total, continue_responding);

  struct serv* s = connection->server->data;
  redisAsyncCommand(s->redis, get_full_name, strdup(data->full_name),
                    "GET v:%s", data->full_name);

  free(request);
}

static void request_path(ebb_request* request, const char* at, size_t len) {
  ebb_connection *connection = request->data;
  struct conn *data = connection->data;

  if(len > 10 && // /gems/ + .gem
      strncmp(at, "/gems/", 5) == 0 &&
      strncmp(at + (len - 4), ".gem", 4) == 0) {
    char* buf = data->full_name;
    size_t sz = len - 10;

    data->full_name_size = sz;

    memcpy(buf, at+6, sz);
    buf[sz] = 0;
  }
}

static ebb_request* new_request(ebb_connection *connection) {
  //printf("request %d\n", ++c);
  ebb_request *request = malloc(sizeof(ebb_request));
  ebb_request_init(request);
  request->data = connection;
  request->on_path = request_path;
  request->on_complete = request_complete;
  return request;
}

ebb_connection* new_connection(ebb_server *server, struct sockaddr_in *addr) {
  struct conn *data = malloc(sizeof(struct conn));
  if(data == NULL) return NULL;

  ebb_connection *connection = malloc(sizeof(ebb_connection));
  if(connection == NULL) {
    free(data);
    return NULL;
  }

  ebb_connection_init(connection);
  connection->data = data;
  connection->new_request = new_request;
  connection->on_close = on_close;
  
  return connection;
}

void start_redis(struct serv* s);

static void reconnect_cb(struct ev_loop *loop, ev_timer *w, int revents) {
  struct serv* s = w->data;
  start_redis(s);
}

static void connect_db(const redisAsyncContext* c, int status) {
  struct serv* s = c->data;

  if(status != REDIS_OK) {
    printf("Error connecting: %s\n", c->errstr);

    ev_timer_init(&(s->reconnect_timer), reconnect_cb, 5., 0.);
    ev_timer_start(s->loop, &(s->reconnect_timer));
    s->reconnect_timer.data = s;
    
    return;
  }

  printf("Connected to redis on %s.\n", s->host);
}

static void disconnect_cb(const redisAsyncContext* c, int status) {
  // Reconnect it.
  printf("reconnecting...");
  struct serv* s = c->data;

  start_redis(s);
}

void start_redis(struct serv* s) {
  s->redis = redisAsyncConnect(s->host, 6379);
  s->redis->data = s;

  redisLibevAttach(s->loop, s->redis);
  redisAsyncSetConnectCallback(s->redis, connect_db);
  redisAsyncSetDisconnectCallback(s->redis, disconnect_cb);
}

int main(int argc, char **argv)  {
  int port = 5000;
  char* host = "127.0.0.1";

  if(argc >= 2) {
    port = atoi(argv[1]);
  }

  if(argc == 3) {
    host = argv[2];
  }

  struct ev_loop *loop = ev_default_loop(0);
  ebb_server server;

  ebb_server_init(&server, loop);

  server.new_connection = new_connection;

  struct serv s;
  s.host = host;
  s.reconnect_timer.data = &s;

  s.loop = loop;
  server.data = &s;

  start_redis(&s);

  printf("rubygems redis stats server : port %d\n", port);
  ebb_server_listen_on_port(&server, port);
  ev_loop(loop, 0);

  return 0;
}
