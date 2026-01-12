#include <llhttp.h>
#include <postgresql/libpq-fe.h>
#include <regex.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>
#include "cJSON.h"

#define DB_POOL_SIZE 4
#define DB_CONNINFO "host=localhost dbname=postgres user=postgres password=postgres"
#define PORT 8080
#define BACKLOG 128

static int getenv_int(const char *);

/*===========\
| Structures |
\===========*/

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
	uv_stream_t *server;
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

/*========\
| Globals |
\========*/

static route_node_t *router_root;
static uv_loop_t *loop;
static db_pool_t *db_pool;

/*=============================\
| Database pool implementation |
\=============================*/

db_pool_t *
db_pool_create(const char *conninfo, size_t pool_size) {
	db_pool_t *pool = (db_pool_t *)malloc(sizeof(db_pool_t));
	pool->size = pool_size;
	pool->active_count = 0;
	pool->connections = NULL;

	uv_mutex_init(&pool->mutex);
	uv_cond_init(&pool->cond);

	// Create connections
	for (size_t i = 0; i < pool_size; i++) {
		PGconn *conn = PQconnectdb(conninfo);

		if (PQstatus(conn) != CONNECTION_OK) {
			fprintf(stderr, "Connection %ld failed: %s\n", i, PQerrorMessage(conn));
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

		// Wait for available connection
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

/*======================\
| Router implementation |
\======================*/

void
router_add(const char *method, const char *path, route_handler_t handler) {
	route_node_t *node = (route_node_t *)calloc(1, sizeof(route_node_t));
	strncpy(node->method, method, sizeof(node->method) - 1);
	strncpy(node->path, path, sizeof(node->path) - 1);
	node->handler = handler;
	node->next = router_root;
	router_root = node;
}

route_handler_t
router_match(const char *method, const char *path) {
	route_node_t *current = router_root;

	while (current) {
		if (strcmp(method, current->method) == 0) {
			regex_t re;

			if (regcomp(&re, current->path, REG_EXTENDED|REG_NOSUB) != 0)
				return NULL;

			if (regexec(&re, path, 0, NULL, 0) == 0)
				return current->handler;
		}

		current = current->next;
	}

	return NULL;
}

/*======================\
| HTTP parser callbacks |
\======================*/

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

	// Check if this is the X-Auth-Token header
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

/*============\
| Middlewares |
\============*/

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

		char *json_str = cJSON_Print(json);
		strncpy(res->body, json_str, sizeof(res->body) - 1);
		res->body_len = strlen(json_str);
		res->status_code = 401;

		free(json_str);
		cJSON_Delete(json);
	}

	return is_valid;
}

/*===============\
| Route handlers |
\===============*/

void
handle_hello(http_request_t *req, http_response_t *res) {
	cJSON *json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "message", "Hello from C REST API!");
	cJSON_AddStringToObject(json, "method", req->method);
	cJSON_AddStringToObject(json, "path", req->path);

	char *json_str = cJSON_Print(json);
	strncpy(res->body, json_str, sizeof(res->body) - 1);
	res->body_len = strlen(json_str);
	res->status_code = 200;

	free(json_str);
	cJSON_Delete(json);
}

void
handle_user_by_id(http_request_t *req, http_response_t *res) {
	// Extract user ID from path
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

	char *json_str = cJSON_Print(json);
	strncpy(res->body, json_str, sizeof(res->body) - 1);
	res->body_len = strlen(json_str);

	free(json_str);
	cJSON_Delete(json);
}

void
handle_list_users(http_request_t *req, http_response_t *res) {
	size_t page = 0;
	sscanf(req->path, "/api/users?page=%ld", &page);

	char offset[16];
	int printf_len = snprintf(offset, sizeof(offset) - 1, "%ld", page * 100);
	offset[printf_len] = '\0';

	PGconn *conn = db_pool_acquire(db_pool);

	// Begin transaction
	PGresult *begin_result = PQexec(conn, "BEGIN");
	if (PQresultStatus(begin_result) != PGRES_COMMAND_OK) {
		cJSON *json = cJSON_CreateObject();
		cJSON_AddStringToObject(json, "error", "Failed to start transaction");

		char *json_str = cJSON_Print(json);
		strncpy(res->body, json_str, sizeof(res->body) - 1);
		res->body_len = strlen(json_str);
		res->status_code = 500;

		free(json_str);
		cJSON_Delete(json);
		PQclear(begin_result);
		db_pool_release(db_pool, conn);
		return;
	}
	PQclear(begin_result);

	// Get total count
	PGresult *count_result = PQexec(conn, "SELECT COUNT(*) FROM users");
	int total_count = 0;

	if (PQresultStatus(count_result) == PGRES_TUPLES_OK && PQntuples(count_result) > 0)
		total_count = atoi(PQgetvalue(count_result, 0, 0));

	PQclear(count_result);

	// Get users list
	const char *query = "SELECT id, name, email FROM users LIMIT 100 OFFSET $1";
	const char *params[1] = {offset};
	PGresult *result = PQexecParams(conn, query, 1, NULL, params, NULL, NULL, 0);

	cJSON *response = cJSON_CreateObject();
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

		cJSON_AddItemToObject(response, "users", users_array);
		cJSON_AddNumberToObject(response, "total", total_count);
		cJSON_AddNumberToObject(response, "count", rows);
		res->status_code = 200;

		// Commit transaction
		PGresult *commit_result = PQexec(conn, "COMMIT");
		PQclear(commit_result);
	} else {
		cJSON_Delete(users_array);
		cJSON_AddStringToObject(response, "error", PQerrorMessage(conn));
		res->status_code = 500;

		// Rollback on error
		PGresult *rollback_result = PQexec(conn, "ROLLBACK");
		PQclear(rollback_result);
	}

	PQclear(result);
	db_pool_release(db_pool, conn);

	char *json_str = cJSON_Print(response);
	strncpy(res->body, json_str, sizeof(res->body) - 1);
	res->body_len = strlen(json_str);

	free(json_str);
	cJSON_Delete(response);
}

void
handle_create_user(http_request_t *req, http_response_t *res) {
	cJSON *input = cJSON_Parse(req->body);
	cJSON *response = cJSON_CreateObject();

	if (input) {
		cJSON *name = cJSON_GetObjectItem(input, "name");
		cJSON *email = cJSON_GetObjectItem(input, "email");

		if (name && email) {
			PGconn *conn = db_pool_acquire(db_pool);

			const char *query = "INSERT INTO users (name, email) VALUES ($1, $2) RETURNING id";
			const char *params[2] = {name->valuestring, email->valuestring};
			PGresult *result = PQexecParams(conn, query, 2, NULL, params, NULL, NULL, 0);

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

	char *json_str = cJSON_Print(response);
	strncpy(res->body, json_str, sizeof(res->body) - 1);
	res->body_len = strlen(json_str);

	free(json_str);
	cJSON_Delete(response);
}

void
handle_not_found(http_request_t *req, http_response_t *res) {
	cJSON *json = cJSON_CreateObject();
	cJSON_AddStringToObject(json, "error", "Route not found");
	cJSON_AddStringToObject(json, "path", req->path);

	char *json_str = cJSON_Print(json);
	strncpy(res->body, json_str, sizeof(res->body) - 1);
	res->body_len = strlen(json_str);
	res->status_code = 404;

	free(json_str);
	cJSON_Delete(json);
}

/*===================\
| Thread pool worker |
\===================*/

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

/*===========================\
| Server connection handling |
\===========================*/

void
on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
	client_t *client = (client_t *)stream->data;

	if (nread > 0) {
		enum llhttp_errno err = llhttp_execute(&client->parser, buf->base, nread);

		if (err == HPE_OK || client->parser.finish == HTTP_FINISH_SAFE) {
			// Request complete, queue work
			work_request_t *work = (work_request_t *)malloc(sizeof(work_request_t));
			work->work.data = work;
			work->client = client;
			memcpy(&work->request, &client->request, sizeof(http_request_t));

			uv_queue_work(loop, &work->work, work_cb, after_work_cb);
		} else if (err != HPE_OK) {
			// Parser error
			fprintf(stderr, "HTTP parse error: %s\n", llhttp_errno_name(err));
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
	client->server = server;

	if (uv_accept(server, (uv_stream_t *)&client->handle) == 0) {
		// Initialize HTTP parser
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


/*=====\
| Main |
\=====*/

int
main() {
	// Initialize event loop
	loop = uv_default_loop();

	// Initialize database pool
	int db_pool_size = getenv_int("DB_POOL_SIZE");
	if (db_pool_size == 0)
		db_pool_size = DB_POOL_SIZE;

	char *db_conninfo = getenv("DB_CONNINFO");
	if (!db_conninfo)
		db_conninfo = DB_CONNINFO;

	db_pool = db_pool_create(db_conninfo, db_pool_size);
	if (!db_pool || db_pool->active_count == 0) {
		fprintf(stderr, "Failed to create database pool\n");
		return 1;
	}

	// Setup routes
	router_root = NULL;
	router_add("GET", "/api/hello$", handle_hello);
	router_add("GET", "/api/users(\\?.*)?$", handle_list_users);
	router_add("GET", "/api/users/[0-9]+$", handle_user_by_id);
	router_add("POST", "/api/users$", handle_create_user);

	// Setup server
	uv_tcp_t server;
	uv_tcp_init(loop, &server);

	int port = getenv_int("PORT");
	if (port == 0)
		port = PORT;

	struct sockaddr_in addr;
	uv_ip4_addr("0.0.0.0", port, &addr);

	uv_tcp_bind(&server, (const struct sockaddr *)&addr, 0);

	int backlog = getenv_int("BACKLOG");
	if (backlog == 0)
		backlog = BACKLOG;

	int r = uv_listen((uv_stream_t *)&server, backlog, on_connect);
	if (r) {
		fprintf(stderr, "Listen error: %s\n", uv_strerror(r));
		return 1;
	}

	printf("Server running on http://0.0.0.0:%d\n", port);

	int result = uv_run(loop, UV_RUN_DEFAULT);

	// Cleanup
	db_pool_destroy(db_pool);

	return result;
}

static int
getenv_int(const char *name) {
	const char *value = getenv(name);
	return value ? atoi(value) : 0;
}
