#include <llhttp.h>
#include <postgresql/libpq-fe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "cJSON.h"

#define PORT 8080
#define BACKLOG 128
#define DB_POOL_SIZE 10

// ============================================================================
// STRUCTURES
// ============================================================================

typedef struct db_connection {
  PGconn* conn;
  int in_use;
  struct db_connection* next;
} db_connection_t;

typedef struct {
  db_connection_t* connections;
  int size;
  int active_count;
  uv_mutex_t mutex;
  uv_cond_t cond;
} db_pool_t;

typedef struct {
  char method[16];
  char path[256];
  char body[4096];
  size_t body_len;
} http_request_t;

typedef struct {
  int status_code;
  char body[4096];
  size_t body_len;
} http_response_t;

typedef void (*route_handler_t)(http_request_t* req, http_response_t* res);

typedef struct route_node {
  char segment[128];
  int is_wildcard;
  route_handler_t handler;
  struct route_node* children[16];
  int child_count;
} route_node_t;

typedef struct {
  uv_tcp_t handle;
  uv_stream_t* server;
  llhttp_t parser;
  llhttp_settings_t parser_settings;
  http_request_t request;
  char read_buffer[8192];
} client_t;

typedef struct {
  uv_work_t work;
  client_t* client;
  http_request_t request;
  http_response_t response;
} work_request_t;

// ============================================================================
// GLOBALS
// ============================================================================

static route_node_t* router_root = NULL;
static uv_loop_t* loop;
static db_pool_t* db_pool = NULL;

// ============================================================================
// DATABASE POOL IMPLEMENTATION
// ============================================================================

db_pool_t* db_pool_create(const char* conninfo, int pool_size) {
  db_pool_t* pool = (db_pool_t*)malloc(sizeof(db_pool_t));
  pool->size = pool_size;
  pool->active_count = 0;
  pool->connections = NULL;

  uv_mutex_init(&pool->mutex);
  uv_cond_init(&pool->cond);

  // Create connections
  for (int i = 0; i < pool_size; i++) {
    PGconn* conn = PQconnectdb(conninfo);

    if (PQstatus(conn) != CONNECTION_OK) {
      fprintf(stderr, "Connection %d failed: %s\n", i, PQerrorMessage(conn));
      PQfinish(conn);
      continue;
    }

    db_connection_t* db_conn = (db_connection_t*)malloc(sizeof(db_connection_t));
    db_conn->conn = conn;
    db_conn->in_use = 0;
    db_conn->next = pool->connections;
    pool->connections = db_conn;
    pool->active_count++;
  }

  printf("Database pool created with %d connections\n", pool->active_count);
  return pool;
}

PGconn* db_pool_acquire(db_pool_t* pool) {
  uv_mutex_lock(&pool->mutex);

  while (1) {
    db_connection_t* current = pool->connections;
    while (current) {
      if (!current->in_use) {
        current->in_use = 1;
        uv_mutex_unlock(&pool->mutex);
        return current->conn;
      }
      current = current->next;
    }

    // Wait for available connection
    uv_cond_wait(&pool->cond, &pool->mutex);
  }
}

void db_pool_release(db_pool_t* pool, PGconn* conn) {
  uv_mutex_lock(&pool->mutex);

  db_connection_t* current = pool->connections;
  while (current) {
    if (current->conn == conn) {
      current->in_use = 0;
      uv_cond_signal(&pool->cond);
      break;
    }
    current = current->next;
  }

  uv_mutex_unlock(&pool->mutex);
}

void db_pool_destroy(db_pool_t* pool) {
  db_connection_t* current = pool->connections;
  while (current) {
    db_connection_t* next = current->next;
    PQfinish(current->conn);
    free(current);
    current = next;
  }

  uv_mutex_destroy(&pool->mutex);
  uv_cond_destroy(&pool->cond);
  free(pool);
}

// ============================================================================
// ROUTER IMPLEMENTATION
// ============================================================================

route_node_t* create_route_node(const char* segment, int is_wildcard) {
  route_node_t* node = (route_node_t*)calloc(1, sizeof(route_node_t));
  strncpy(node->segment, segment, sizeof(node->segment) - 1);
  node->is_wildcard = is_wildcard;
  node->handler = NULL;
  node->child_count = 0;
  return node;
}

void router_add(const char* method, const char* path, route_handler_t handler) {
  if (!router_root) {
    router_root = create_route_node("", 0);
  }

  char path_copy[256];
  strncpy(path_copy, path, sizeof(path_copy) - 1);

  route_node_t* current = router_root;
  char* token = strtok(path_copy, "/");

  while (token) {
    int is_wildcard = (token[0] == ':');
    char* segment = is_wildcard ? ":" : token;

    route_node_t* found = NULL;
    for (int i = 0; i < current->child_count; i++) {
      if (strcmp(current->children[i]->segment, segment) == 0) {
        found = current->children[i];
        break;
      }
    }

    if (!found) {
      found = create_route_node(segment, is_wildcard);
      current->children[current->child_count++] = found;
    }

    current = found;
    token = strtok(NULL, "/");
  }

  current->handler = handler;
}

route_handler_t router_match(const char* path) {
  if (!router_root)
    return NULL;

  char path_copy[256];
  strncpy(path_copy, path, sizeof(path_copy) - 1);

  route_node_t* current = router_root;
  char* token = strtok(path_copy, "/");

  while (token && current) {
    route_node_t* found = NULL;

    for (int i = 0; i < current->child_count; i++) {
      if (current->children[i]->is_wildcard || strcmp(current->children[i]->segment, token) == 0) {
        found = current->children[i];
        break;
      }
    }

    if (!found)
      return NULL;
    current = found;
    token = strtok(NULL, "/");
  }

  return current ? current->handler : NULL;
}

// ============================================================================
// HTTP PARSER CALLBACKS
// ============================================================================

int on_message_begin(llhttp_t* parser) {
  client_t* client = (client_t*)parser->data;
  memset(&client->request, 0, sizeof(http_request_t));
  return 0;
}

int on_url(llhttp_t* parser, const char* at, size_t length) {
  client_t* client = (client_t*)parser->data;
  size_t copy_len = length < sizeof(client->request.path) - 1 ? length : sizeof(client->request.path) - 1;
  strncpy(client->request.path, at, copy_len);
  client->request.path[copy_len] = '\0';
  return 0;
}

int on_body(llhttp_t* parser, const char* at, size_t length) {
  client_t* client = (client_t*)parser->data;
  size_t copy_len = length < sizeof(client->request.body) - 1 ? length : sizeof(client->request.body) - 1;
  strncpy(client->request.body, at, copy_len);
  client->request.body_len = copy_len;
  return 0;
}

int on_message_complete(llhttp_t* parser) {
  client_t* client = (client_t*)parser->data;
  snprintf(client->request.method, sizeof(client->request.method), "%s", llhttp_method_name(llhttp_get_method(parser)));
  return 0;
}

// ============================================================================
// ROUTE HANDLERS
// ============================================================================

void handle_hello(http_request_t* req, http_response_t* res) {
  cJSON* json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "message", "Hello from C REST API!");
  cJSON_AddStringToObject(json, "method", req->method);
  cJSON_AddStringToObject(json, "path", req->path);

  char* json_str = cJSON_Print(json);
  strncpy(res->body, json_str, sizeof(res->body) - 1);
  res->body_len = strlen(json_str);
  res->status_code = 200;

  free(json_str);
  cJSON_Delete(json);
}

void handle_user_by_id(http_request_t* req, http_response_t* res) {
  // Extract user ID from path
  char* id_str = strrchr(req->path, '/');
  if (id_str)
    id_str++;

  PGconn* conn = db_pool_acquire(db_pool);

  const char* params[1] = {id_str};
  PGresult* result =
      PQexecParams(conn, "SELECT id, name, email FROM users WHERE id = $1", 1, NULL, params, NULL, NULL, 0);

  cJSON* json = cJSON_CreateObject();

  if (PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) > 0) {
    cJSON_AddNumberToObject(json, "id", atoi(PQgetvalue(result, 0, 0)));
    cJSON_AddStringToObject(json, "name", PQgetvalue(result, 0, 1));
    cJSON_AddStringToObject(json, "email", PQgetvalue(result, 0, 2));
    res->status_code = 200;
  } else {
    cJSON_AddStringToObject(json, "error", "User not found");
    res->status_code = 404;
  }

  PQclear(result);
  db_pool_release(db_pool, conn);

  char* json_str = cJSON_Print(json);
  strncpy(res->body, json_str, sizeof(res->body) - 1);
  res->body_len = strlen(json_str);

  free(json_str);
  cJSON_Delete(json);
}

void handle_list_users(http_request_t* req, http_response_t* res) {
  PGconn* conn = db_pool_acquire(db_pool);

  PGresult* result = PQexec(conn, "SELECT id, name, email FROM users LIMIT 10");

  cJSON* json = cJSON_CreateArray();

  if (PQresultStatus(result) == PGRES_TUPLES_OK) {
    int rows = PQntuples(result);
    for (int i = 0; i < rows; i++) {
      cJSON* user = cJSON_CreateObject();
      cJSON_AddNumberToObject(user, "id", atoi(PQgetvalue(result, i, 0)));
      cJSON_AddStringToObject(user, "name", PQgetvalue(result, i, 1));
      cJSON_AddStringToObject(user, "email", PQgetvalue(result, i, 2));
      cJSON_AddItemToArray(json, user);
    }
    res->status_code = 200;
  } else {
    cJSON_Delete(json);
    json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "error", PQerrorMessage(conn));
    res->status_code = 500;
  }

  PQclear(result);
  db_pool_release(db_pool, conn);

  char* json_str = cJSON_Print(json);
  strncpy(res->body, json_str, sizeof(res->body) - 1);
  res->body_len = strlen(json_str);

  free(json_str);
  cJSON_Delete(json);
}

void handle_create_user(http_request_t* req, http_response_t* res) {
  cJSON* input = cJSON_Parse(req->body);
  cJSON* response = cJSON_CreateObject();

  if (input) {
    cJSON* name = cJSON_GetObjectItem(input, "name");
    cJSON* email = cJSON_GetObjectItem(input, "email");

    if (name && email) {
      PGconn* conn = db_pool_acquire(db_pool);

      const char* params[2] = {name->valuestring, email->valuestring};
      PGresult* result = PQexecParams(conn,
                                      "INSERT INTO users (name, email) "
                                      "VALUES ($1, $2) RETURNING id",
                                      2, NULL, params, NULL, NULL, 0);

      if (PQresultStatus(result) == PGRES_TUPLES_OK) {
        cJSON_AddStringToObject(response, "status", "created");
        cJSON_AddNumberToObject(response, "id", atoi(PQgetvalue(result, 0, 0)));
        cJSON_AddStringToObject(response, "name", name->valuestring);
        cJSON_AddStringToObject(response, "email", email->valuestring);
        res->status_code = 201;
      } else {
        cJSON_AddStringToObject(response, "error", PQerrorMessage(conn));
        res->status_code = 500;
      }

      PQclear(result);
      db_pool_release(db_pool, conn);
    } else {
      cJSON_AddStringToObject(response, "error", "Missing name or email");
      res->status_code = 400;
    }

    cJSON_Delete(input);
  } else {
    cJSON_AddStringToObject(response, "error", "Invalid JSON");
    res->status_code = 400;
  }

  char* json_str = cJSON_Print(response);
  strncpy(res->body, json_str, sizeof(res->body) - 1);
  res->body_len = strlen(json_str);

  free(json_str);
  cJSON_Delete(response);
}

void handle_not_found(http_request_t* req, http_response_t* res) {
  cJSON* json = cJSON_CreateObject();
  cJSON_AddStringToObject(json, "error", "Route not found");
  cJSON_AddStringToObject(json, "path", req->path);

  char* json_str = cJSON_Print(json);
  strncpy(res->body, json_str, sizeof(res->body) - 1);
  res->body_len = strlen(json_str);
  res->status_code = 404;

  free(json_str);
  cJSON_Delete(json);
}

// ============================================================================
// THREAD POOL WORKER
// ============================================================================

void work_cb(uv_work_t* req) {
  work_request_t* work = (work_request_t*)req->data;

  route_handler_t handler = router_match(work->request.path);
  if (handler) {
    handler(&work->request, &work->response);
  } else {
    handle_not_found(&work->request, &work->response);
  }
}

void after_work_cb(uv_work_t* req, int status);

void send_response(client_t* client, http_response_t* res) {
  char response[8192];
  int len = snprintf(response, sizeof(response),
                     "HTTP/1.1 %d OK\r\n"
                     "Content-Type: application/json\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n"
                     "\r\n"
                     "%s",
                     res->status_code, res->body_len, res->body);

  uv_buf_t buf = uv_buf_init(response, len);
  uv_write_t* write_req = (uv_write_t*)malloc(sizeof(uv_write_t));
  uv_write(write_req, (uv_stream_t*)&client->handle, &buf, 1, NULL);
}

void after_work_cb(uv_work_t* req, int status) {
  work_request_t* work = (work_request_t*)req->data;

  if (status == 0) {
    send_response(work->client, &work->response);
  }

  uv_close((uv_handle_t*)&work->client->handle, (uv_close_cb)free);
  free(work);
}

// ============================================================================
// SERVER CONNECTION HANDLING
// ============================================================================

void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf) {
  client_t* client = (client_t*)stream->data;

  if (nread > 0) {
    enum llhttp_errno err = llhttp_execute(&client->parser, buf->base, nread);

    if (err == HPE_OK || client->parser.finish == HTTP_FINISH_SAFE) {
      // Request complete, queue work
      work_request_t* work = (work_request_t*)malloc(sizeof(work_request_t));
      work->work.data = work;
      work->client = client;
      memcpy(&work->request, &client->request, sizeof(http_request_t));

      uv_queue_work(loop, &work->work, work_cb, after_work_cb);
    } else if (err != HPE_OK) {
      // Parser error
      fprintf(stderr, "HTTP parse error: %s\n", llhttp_errno_name(err));
      uv_close((uv_handle_t*)stream, (uv_close_cb)free);
    }
  } else if (nread < 0) {
    uv_close((uv_handle_t*)stream, (uv_close_cb)free);
  }

  if (buf->base)
    free(buf->base);
}

void alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf) {
  buf->base = (char*)malloc(suggested_size);
  buf->len = suggested_size;
}

void on_connect(uv_stream_t* server, int status) {
  if (status < 0)
    return;

  client_t* client = (client_t*)malloc(sizeof(client_t));
  uv_tcp_init(loop, &client->handle);
  client->handle.data = client;
  client->server = server;

  if (uv_accept(server, (uv_stream_t*)&client->handle) == 0) {
    // Initialize HTTP parser
    llhttp_settings_init(&client->parser_settings);
    client->parser_settings.on_message_begin = on_message_begin;
    client->parser_settings.on_url = on_url;
    client->parser_settings.on_body = on_body;
    client->parser_settings.on_message_complete = on_message_complete;

    llhttp_init(&client->parser, HTTP_REQUEST, &client->parser_settings);
    client->parser.data = client;

    uv_read_start((uv_stream_t*)&client->handle, alloc_buffer, on_read);
  } else {
    uv_close((uv_handle_t*)&client->handle, (uv_close_cb)free);
  }
}

// ============================================================================
// MAIN
// ============================================================================

int main() {
  loop = uv_default_loop();

  // Initialize database pool
  const char* db_conninfo = "host=localhost dbname=mydb user=postgres password=postgres";
  db_pool = db_pool_create(db_conninfo, DB_POOL_SIZE);

  if (!db_pool || db_pool->active_count == 0) {
    fprintf(stderr, "Failed to create database pool\n");
    // return 1;
  }

  // Setup routes
  router_add("GET", "/api/hello", handle_hello);
  router_add("GET", "/api/users", handle_list_users);
  router_add("GET", "/api/users/:id", handle_user_by_id);
  router_add("POST", "/api/users", handle_create_user);

  // Setup server
  uv_tcp_t server;
  uv_tcp_init(loop, &server);

  struct sockaddr_in addr;
  uv_ip4_addr("0.0.0.0", PORT, &addr);

  uv_tcp_bind(&server, (const struct sockaddr*)&addr, 0);

  int r = uv_listen((uv_stream_t*)&server, BACKLOG, on_connect);
  if (r) {
    fprintf(stderr, "Listen error: %s\n", uv_strerror(r));
    return 1;
  }

  printf("Server running on http://0.0.0.0:%d\n", PORT);
  printf("Database pool size: %d\n", db_pool->active_count);
  printf("\nAvailable routes:\n");
  printf("  GET  /api/hello\n");
  printf("  GET  /api/users\n");
  printf("  GET  /api/users/:id\n");
  printf("  POST /api/users\n");

  int result = uv_run(loop, UV_RUN_DEFAULT);

  // Cleanup
  // db_pool_destroy(db_pool);

  return result;
}