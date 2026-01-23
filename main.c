#include <llhttp.h>
#include <postgresql/libpq-fe.h>
#include <regex.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <uv.h>
#include "cJSON.h"

#define DB_POOL_SIZE 4
#define DB_CONNINFO "host=localhost dbname=postgres user=postgres password=postgres"
#define HTTP_PORT 8080
#define HTTP_BACKLOG 128

/* Structures */

typedef enum log_level_e {
	DEBUG,
	INFO,
	WARN,
	ERROR
} log_level_t;

typedef struct db_connection_s {
	PGconn *conn;
	bool in_use;
	struct db_connection_s *next;
} db_connection_t;

typedef struct db_pool_s {
	db_connection_t *connections;
	size_t size;
	int active_count;
	uv_mutex_t mutex;
	uv_cond_t cond;
} db_pool_t;

typedef struct http_request_s {
	char method[16];
	char path[256];
	char auth_token[256];
	char body[4096];
	size_t body_len;
} http_request_t;

typedef struct http_response_s {
	int status_code;
	char body[4096];
	size_t body_len;
} http_response_t;

typedef void (*route_handler_t)(http_request_t *, http_response_t *);

typedef struct route_node_s {
	char method[16];
	char path[256];
	route_handler_t handler;
	struct route_node_s *next;
} route_node_t;

typedef struct client_s {
	uv_tcp_t handle;
	llhttp_settings_t parser_settings;
	llhttp_t parser;
	http_request_t request;
	char read_buffer[8192];
	char current_header_field[256];
	char current_header_value[256];
} client_t;

typedef struct work_request_s {
	uv_work_t work;
	client_t *client;
	http_request_t request;
	http_response_t response;
} work_request_t;

/* Globals */

static log_level_t log_level;
static db_pool_t *db_pool;
static route_node_t *router_root;
static uv_loop_t *loop;
static uv_tcp_t server;
static uv_signal_t sigint;
static uv_signal_t sigterm;

static int getenv_int(const char *);
static void copy_json_to_response(http_response_t *, cJSON *);

/* Logger */

void
log_message(log_level_t level, const char *format, ...) {
	if (level < log_level)
		return;

	/* Get current time with milliseconds */
	struct timeval tv;
	gettimeofday(&tv, NULL);

	time_t now = tv.tv_sec;
	int millisec = tv.tv_usec / 1000;

	char timestamp[32];
	strftime(timestamp, sizeof(timestamp), "%Y-%m-%dT%H:%M:%S", localtime(&now));

	const char *level_str[] = {"DEBUG", "INFO", "WARN", "ERROR"};
	printf("%s.%dZ [%s] - ", timestamp, millisec, level_str[level]);

	va_list args;
	va_start(args, format);
	vprintf(format, args);
	va_end(args);

	printf("\n");
	fflush(stdout);
}

/* Database pool implementation */

db_pool_t *
db_pool_create(const char *conninfo, size_t pool_size) {
	db_pool_t *pool = (db_pool_t *)malloc(sizeof(db_pool_t));
	pool->size = pool_size;
	pool->active_count = 0;
	pool->connections = NULL;

	uv_mutex_init(&pool->mutex);
	uv_cond_init(&pool->cond);

	/* Create connections */
	for (size_t i = 0; i < pool_size; i++) {
		PGconn *conn = PQconnectdb(conninfo);

		if (PQstatus(conn) != CONNECTION_OK) {
			log_message(ERROR, "Connection %ld failed: %s", i, PQerrorMessage(conn));
			PQfinish(conn);
			continue;
		}

		db_connection_t *db_conn = (db_connection_t *)malloc(sizeof(db_connection_t));
		db_conn->conn = conn;
		db_conn->in_use = false;
		db_conn->next = pool->connections;
		pool->connections = db_conn;
		pool->active_count += 1;
	}

	return pool;
}

PGconn *
db_pool_acquire(db_pool_t *pool) {
	uv_mutex_lock(&pool->mutex);

	while (true) {
		db_connection_t *current = pool->connections;
		while (current) {
			if (!current->in_use) {
				current->in_use = true;
				uv_mutex_unlock(&pool->mutex);
				return current->conn;
			}

			current = current->next;
		}

		/* Wait for available connection */
		uv_cond_wait(&pool->cond, &pool->mutex);
	}
}

void
db_pool_release(db_pool_t *pool, PGconn *conn) {
	uv_mutex_lock(&pool->mutex);

	db_connection_t *current = pool->connections;
	while (current) {
		if (current->conn == conn) {
			current->in_use = false;
			uv_cond_signal(&pool->cond);
			break;
		}

		current = current->next;
	}

	uv_mutex_unlock(&pool->mutex);
}

void
db_pool_destroy(db_pool_t *pool) {
	db_connection_t *current = pool->connections;
	while (current) {
		db_connection_t *next = current->next;
		PQfinish(current->conn);
		free(current);

		current = next;
	}

	uv_mutex_destroy(&pool->mutex);
	uv_cond_destroy(&pool->cond);
	free(pool);
}

/* Router implementation */

void
router_add(const char *method, const char *path, route_handler_t handler) {
	route_node_t *node = (route_node_t *)calloc(1, sizeof(route_node_t));
	strncpy(node->method, method, sizeof(node->method) - 1);
	strncpy(node->path, path, sizeof(node->path) - 1);
	node->handler = handler;
	node->next = router_root;
	router_root = node;
	log_message(INFO, "%s %s", method, path);
}

route_handler_t
router_match(const char *method, const char *path) {
	route_node_t *current = router_root;

	while (current) {
		if (strcmp(method, current->method) == 0) {
			regex_t re;

			if (regcomp(&re, current->path, REG_EXTENDED|REG_NOSUB) != 0)
				return NULL;

			if (regexec(&re, path, 0, NULL, 0) == 0) {
				regfree(&re);
				return current->handler;
			}

			regfree(&re);
		}

		current = current->next;
	}

	return NULL;
}

/* HTTP parser callbacks */

int
on_message_begin(llhttp_t *parser) {
	client_t *client = (client_t *)parser->data;
	memset(&client->request, 0, sizeof(http_request_t));
	return 0;
}

int
on_url(llhttp_t *parser, const char *at, size_t length) {
	client_t *client = (client_t *)parser->data;
	size_t copy_len = length < sizeof(client->request.path) - 1 ? length : sizeof(client->request.path) - 1;
	strncpy(client->request.path, at, copy_len);
	client->request.path[copy_len] = '\0';
	return 0;
}

int
on_header_field(llhttp_t *parser, const char *at, size_t length) {
	client_t *client = (client_t*)parser->data;
	size_t copy_len =
	  length < sizeof(client->current_header_field) - 1 ? length : sizeof(client->current_header_field) - 1;
	strncpy(client->current_header_field, at, copy_len);
	client->current_header_field[copy_len] = '\0';
	return 0;
}

int
on_header_value(llhttp_t *parser, const char *at, size_t length) {
	client_t *client = (client_t*)parser->data;
	size_t copy_len =
	  length < sizeof(client->current_header_value) - 1 ? length : sizeof(client->current_header_value) - 1;
	strncpy(client->current_header_value, at, copy_len);
	client->current_header_value[copy_len] = '\0';

	/* Check if this is the X-Auth-Token header */
	if (strcasecmp(client->current_header_field, "X-Auth-Token") == 0)
		strcpy(client->request.auth_token, client->current_header_value);

	return 0;
}

int
on_body(llhttp_t *parser, const char *at, size_t length) {
	client_t *client = (client_t *)parser->data;
	size_t copy_len = length < sizeof(client->request.body) - 1 ? length : sizeof(client->request.body) - 1;
	strncpy(client->request.body, at, copy_len);
	client->request.body_len = copy_len;
	return 0;
}

int
on_message_complete(llhttp_t *parser) {
	client_t *client = (client_t *)parser->data;
	const char *method = llhttp_method_name(llhttp_get_method(parser));
	snprintf(client->request.method, sizeof(client->request.method), "%s", method);
	return 0;
}

/* Middlewares */

bool
verify_auth_token(http_request_t *req, http_response_t *res) {
	PGconn *conn = db_pool_acquire(db_pool);

	const char *query = "SELECT id, is_active FROM auth_tokens WHERE key = $1";
	const char *params[1] = {req->auth_token};
	PGresult *result = PQexecParams(conn, query, 1, NULL, params, NULL, NULL, 0);

	bool is_valid = false;
	if (PQresultStatus(result) == PGRES_TUPLES_OK && PQntuples(result) > 0) {
		const char *is_active = PQgetvalue(result, 0, 1);
		is_valid = strcmp(is_active, "t") == 0;
	}

	PQclear(result);
	db_pool_release(db_pool, conn);

	if (!is_valid) {
		cJSON *json = cJSON_CreateObject();
		cJSON_AddStringToObject(json, "error", "Unauthorized");
		cJSON_AddStringToObject(json, "message", "Invalid or missing API key");
		res->status_code = 401;

		copy_json_to_response(res, json);
		cJSON_Delete(json);
	}

	return is_valid;
}

/* Route handlers */

void
handle_hello(http_request_t *req, http_response_t *res) {
	cJSON *json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "message", "Hello from C REST API!");
	cJSON_AddStringToObject(json, "method", req->method);
	cJSON_AddStringToObject(json, "path", req->path);
	res->status_code = 200;

	log_message(DEBUG, "%s %s %d", req->method, req->path, res->status_code);

	copy_json_to_response(res, json);
	cJSON_Delete(json);
}

void
handle_user_by_id(http_request_t *req, http_response_t *res) {
	/* Extract user ID from path */
	char *id_str = strrchr(req->path, '/');
	if (id_str)
		id_str += 1;

	PGconn *conn = db_pool_acquire(db_pool);

	const char *query = "SELECT id, name, email FROM users WHERE id = $1";
	const char *params[1] = {id_str};
	PGresult *result = PQexecParams(conn, query, 1, NULL, params, NULL, NULL, 0);

	cJSON *json = cJSON_CreateObject();

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

	log_message(DEBUG, "%s %s %d", req->method, req->path, res->status_code);

	copy_json_to_response(res, json);
	cJSON_Delete(json);
}

void
handle_list_users(http_request_t *req, http_response_t *res) {
	size_t page = 0;
	sscanf(req->path, "/api/users?page=%ld", &page);

	char offset[16];
	int printf_len = snprintf(offset, sizeof(offset) - 1, "%ld", page * 10);
	offset[printf_len] = '\0';

	PGconn *conn = db_pool_acquire(db_pool);

	/* Begin transaction */
	PGresult *begin_result = PQexec(conn, "BEGIN");
	if (PQresultStatus(begin_result) != PGRES_COMMAND_OK) {
		cJSON *json = cJSON_CreateObject();
		cJSON_AddStringToObject(json, "error", "Failed to start transaction");
		res->status_code = 500;

		PQclear(begin_result);
		db_pool_release(db_pool, conn);

		log_message(DEBUG, "%s %s %d", req->method, req->path, res->status_code);

		copy_json_to_response(res, json);
		cJSON_Delete(json);
		return;
	}
	PQclear(begin_result);

	/* Get total count */
	PGresult *count_result = PQexec(conn, "SELECT COUNT(*) FROM users");
	int total_count = 0;

	if (PQresultStatus(count_result) == PGRES_TUPLES_OK && PQntuples(count_result) > 0)
		total_count = atoi(PQgetvalue(count_result, 0, 0));

	PQclear(count_result);

	/* Get users list */
	const char *query = "SELECT id, name, email FROM users LIMIT 10 OFFSET $1";
	const char *params[1] = {offset};
	PGresult *result = PQexecParams(conn, query, 1, NULL, params, NULL, NULL, 0);

	cJSON *json_response = cJSON_CreateObject();
	cJSON *users_array = cJSON_CreateArray();

	if (PQresultStatus(result) == PGRES_TUPLES_OK) {
		int rows = PQntuples(result);
		for (int i = 0; i < rows; ++i) {
			cJSON *user = cJSON_CreateObject();
			cJSON_AddNumberToObject(user, "id", atoi(PQgetvalue(result, i, 0)));
			cJSON_AddStringToObject(user, "name", PQgetvalue(result, i, 1));
			cJSON_AddStringToObject(user, "email", PQgetvalue(result, i, 2));
			cJSON_AddItemToArray(users_array, user);
		}

		cJSON_AddItemToObject(json_response, "users", users_array);
		cJSON_AddNumberToObject(json_response, "total", total_count);
		cJSON_AddNumberToObject(json_response, "count", rows);
		res->status_code = 200;

		/* Commit transaction */
		PGresult *commit_result = PQexec(conn, "COMMIT");
		PQclear(commit_result);
	} else {
		cJSON_Delete(users_array);
		cJSON_AddStringToObject(json_response, "error", PQerrorMessage(conn));
		res->status_code = 500;

		/* Rollback on error */
		PGresult *rollback_result = PQexec(conn, "ROLLBACK");
		PQclear(rollback_result);
	}

	PQclear(result);
	db_pool_release(db_pool, conn);

	log_message(DEBUG, "%s %s %d", req->method, req->path, res->status_code);

	copy_json_to_response(res, json_response);
	cJSON_Delete(json_response);
}

void
handle_create_user(http_request_t *req, http_response_t *res) {
	cJSON *json_body = cJSON_Parse(req->body);
	cJSON *json_response = cJSON_CreateObject();

	if (json_body) {
		cJSON *name = cJSON_GetObjectItem(json_body, "name");
		cJSON *email = cJSON_GetObjectItem(json_body, "email");

		if (name && email) {
			PGconn *conn = db_pool_acquire(db_pool);

			const char *query = "INSERT INTO users (name, email) VALUES ($1, $2) RETURNING id";
			const char *params[2] = {name->valuestring, email->valuestring};
			PGresult *result = PQexecParams(conn, query, 2, NULL, params, NULL, NULL, 0);

			if (PQresultStatus(result) == PGRES_TUPLES_OK) {
				cJSON_AddStringToObject(json_response, "status", "created");
				cJSON_AddNumberToObject(json_response, "id", atoi(PQgetvalue(result, 0, 0)));
				cJSON_AddStringToObject(json_response, "name", name->valuestring);
				cJSON_AddStringToObject(json_response, "email", email->valuestring);
				res->status_code = 201;
			} else {
				cJSON_AddStringToObject(json_response, "error", PQerrorMessage(conn));
				res->status_code = 500;
			}

			PQclear(result);
			db_pool_release(db_pool, conn);
		} else {
			cJSON_AddStringToObject(json_response, "error", "Missing name or email");
			res->status_code = 400;
		}
	} else {
		cJSON_AddStringToObject(json_response, "error", "Invalid JSON");
		res->status_code = 400;
	}

	log_message(DEBUG, "%s %s %d", req->method, req->path, res->status_code);

	copy_json_to_response(res, json_response);
	cJSON_Delete(json_response);
	cJSON_Delete(json_body);
}

void
handle_not_found(http_request_t *req, http_response_t *res) {
	cJSON *json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "error", "Route not found");
	cJSON_AddStringToObject(json, "path", req->path);
	res->status_code = 404;

	log_message(DEBUG, "%s %s %d", req->method, req->path, res->status_code);

	copy_json_to_response(res, json);
	cJSON_Delete(json);
}

/* Thread pool worker */

void
work_cb(uv_work_t *req) {
	work_request_t *work = (work_request_t *)req->data;

	route_handler_t handler = router_match(work->request.method, work->request.path);

	if (!verify_auth_token(&work->request, &work->response))
		return;

	if (handler)
		handler(&work->request, &work->response);
	else
		handle_not_found(&work->request, &work->response);
}

void
send_response(client_t *client, http_response_t *res) {
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
	uv_write_t *write_req = (uv_write_t *)malloc(sizeof(uv_write_t));
	uv_write(write_req, (uv_stream_t *)&client->handle, &buf, 1, NULL);
}

void
after_work_cb(uv_work_t *req, int status) {
	work_request_t *work = (work_request_t *)req->data;

	if (status == 0)
		send_response(work->client, &work->response);

	uv_close((uv_handle_t *)&work->client->handle, (uv_close_cb)free);
	free(work);
}

/* Server connection handling */

void
on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
	client_t *client = (client_t *)stream->data;

	if (nread > 0) {
		enum llhttp_errno err = llhttp_execute(&client->parser, buf->base, nread);

		if (err == HPE_OK || client->parser.finish == HTTP_FINISH_SAFE) {
			/* Request complete, queue work */
			work_request_t *work = (work_request_t *)malloc(sizeof(work_request_t));
			work->work.data = work;
			work->client = client;
			memcpy(&work->request, &client->request, sizeof(http_request_t));

			uv_queue_work(loop, &work->work, work_cb, after_work_cb);
		} else if (err != HPE_OK) {
			/* Parser error */
			log_message(ERROR, "HTTP parse error: %s", llhttp_errno_name(err));
			uv_close((uv_handle_t *)stream, (uv_close_cb)free);
		}
	} else if (nread < 0)
		uv_close((uv_handle_t *)stream, (uv_close_cb)free);

	if (buf->base)
		free(buf->base);
}

void
alloc_buffer(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf) {
	buf->base = (char *)malloc(suggested_size);
	buf->len = suggested_size;
}

void
on_connect(uv_stream_t *server, int status) {
	if (status < 0)
		return;

	client_t *client = (client_t *)malloc(sizeof(client_t));
	uv_tcp_init(loop, &client->handle);
	client->handle.data = client;

	if (uv_accept(server, (uv_stream_t *)&client->handle) == 0) {
		/* Initialize HTTP parser */
		llhttp_settings_init(&client->parser_settings);
		client->parser_settings.on_message_begin = on_message_begin;
		client->parser_settings.on_url = on_url;
		client->parser_settings.on_header_field = on_header_field;
		client->parser_settings.on_header_value = on_header_value;
		client->parser_settings.on_body = on_body;
		client->parser_settings.on_message_complete = on_message_complete;

		llhttp_init(&client->parser, HTTP_REQUEST, &client->parser_settings);
		client->parser.data = client;

		uv_read_start((uv_stream_t*)&client->handle, alloc_buffer, on_read);
	} else
		uv_close((uv_handle_t*)&client->handle, (uv_close_cb)free);
}

void
on_signal(uv_signal_t *handle, int signum) {
    log_message(INFO, "Received signal %d, shutting down gracefully...", signum);

    uv_close((uv_handle_t*)&server, NULL);
    uv_signal_stop(&sigint);
    uv_signal_stop(&sigterm);
    uv_stop(loop);
}

/* Main */

int
main() {
	/* Initialize logger */
	log_level = getenv_int("LOG_LEVEL");

	/* Initialize database pool */
	int db_pool_size = getenv_int("DB_POOL_SIZE");
	if (db_pool_size == 0)
		db_pool_size = DB_POOL_SIZE;

	char *db_conninfo = getenv("DB_CONNINFO");
	if (!db_conninfo)
		db_conninfo = DB_CONNINFO;

	db_pool = db_pool_create(db_conninfo, db_pool_size);
	if (!db_pool || db_pool->active_count == 0) {
		log_message(ERROR, "Failed to create database pool");
		return 1;
	}

	/* Setup routes */
	router_root = NULL;
	router_add("GET", "/api/hello$", handle_hello);
	router_add("GET", "/api/users(\\?.*)?$", handle_list_users);
	router_add("GET", "/api/users/[0-9]+$", handle_user_by_id);
	router_add("POST", "/api/users$", handle_create_user);

	/* Initialize event loop */
	loop = uv_default_loop();

	/* Setup signal handlers */
	uv_signal_init(loop, &sigint);
	uv_signal_start(&sigint, on_signal, SIGINT);
	uv_signal_init(loop, &sigterm);
	uv_signal_start(&sigterm, on_signal, SIGTERM);

	/* Setup server */
	uv_tcp_init(loop, &server);

	int port = getenv_int("HTTP_PORT");
	if (port == 0)
		port = HTTP_PORT;

	struct sockaddr_in addr;
	uv_ip4_addr("0.0.0.0", port, &addr);

	uv_tcp_bind(&server, (const struct sockaddr *)&addr, 0);

	int backlog = getenv_int("HTTP_BACKLOG");
	if (backlog == 0)
		backlog = HTTP_BACKLOG;

	int r = uv_listen((uv_stream_t *)&server, backlog, on_connect);
	if (r) {
		log_message(ERROR, "Listen error: %s", uv_strerror(r));
		db_pool_destroy(db_pool);
		return 1;
	}

	log_message(INFO, "Server running on http://0.0.0.0:%d", port);

	int result = uv_run(loop, UV_RUN_DEFAULT);

	db_pool_destroy(db_pool);
	uv_loop_close(loop);

	return result;
}

static int
getenv_int(const char *name) {
	const char *value = getenv(name);
	return value ? atoi(value) : 0;
}

static void
copy_json_to_response(http_response_t *res, cJSON *json) {
	char *json_str = cJSON_Print(json);
	strncpy(res->body, json_str, sizeof(res->body) - 1);
	res->body_len = strlen(json_str);
	free(json_str);
}
