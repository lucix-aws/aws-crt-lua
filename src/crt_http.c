/**
 * aws-crt-lua: Synchronous HTTP shim over the AWS CRT async HTTP client.
 *
 * Strategy: We own an event loop group + host resolver + client bootstrap.
 * Each request acquires a connection, makes the request with callbacks that
 * accumulate headers/body into buffers, and signals a mutex+condvar when done.
 * The calling thread blocks on the condvar.
 */

#include "crt_http.h"

#include <aws/common/allocator.h>
#include <aws/common/condition_variable.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>
#include <aws/common/byte_buf.h>
#include <aws/common/uri.h>
#include <aws/common/zero.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/event_loop.h>
#include <aws/io/host_resolver.h>
#include <aws/io/socket.h>
#include <aws/io/stream.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/http/connection.h>
#include <aws/http/connection_manager.h>
#include <aws/http/request_response.h>

#include <string.h>

struct aws_crt_lua_client {
    struct aws_allocator *allocator;
    struct aws_event_loop_group *event_loop_group;
    struct aws_host_resolver *host_resolver;
    struct aws_client_bootstrap *bootstrap;
    struct aws_tls_ctx *tls_ctx;
    struct aws_http_connection_manager *conn_manager;
};

/* Per-request state shared between the calling thread and CRT callbacks. */
struct request_context {
    struct aws_allocator *allocator;
    struct aws_mutex mutex;
    struct aws_condition_variable cv;
    bool conn_ready;    /* connection acquired (or failed) */
    bool complete;      /* request/response finished */

    /* Response accumulation */
    int status_code;
    int error_code;

    /* Headers: dynamic array */
    struct header_entry {
        struct aws_byte_buf name;
        struct aws_byte_buf value;
    } *headers;
    size_t num_headers;
    size_t headers_capacity;

    /* Body */
    struct aws_byte_buf body;

    /* Connection (for release) */
    struct aws_http_connection_manager *conn_manager;
    struct aws_http_connection *connection;
};

static bool s_conn_ready(void *user_data) {
    struct request_context *ctx = user_data;
    return ctx->conn_ready;
}

static bool s_request_done(void *user_data) {
    struct request_context *ctx = user_data;
    return ctx->complete;
}

static int s_on_response_headers(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    const struct aws_http_header *header_array,
    size_t num_headers,
    void *user_data) {

    (void)stream;
    (void)header_block;
    struct request_context *ctx = user_data;

    for (size_t i = 0; i < num_headers; i++) {
        if (ctx->num_headers >= ctx->headers_capacity) {
            size_t new_cap = ctx->headers_capacity ? ctx->headers_capacity * 2 : 16;
            struct header_entry *new_arr = aws_mem_calloc(ctx->allocator, new_cap, sizeof(struct header_entry));
            if (ctx->headers) {
                memcpy(new_arr, ctx->headers, ctx->num_headers * sizeof(struct header_entry));
                aws_mem_release(ctx->allocator, ctx->headers);
            }
            ctx->headers = new_arr;
            ctx->headers_capacity = new_cap;
        }

        struct header_entry *entry = &ctx->headers[ctx->num_headers++];
        aws_byte_buf_init_copy_from_cursor(&entry->name, ctx->allocator, header_array[i].name);
        aws_byte_buf_init_copy_from_cursor(&entry->value, ctx->allocator, header_array[i].value);
    }

    return AWS_OP_SUCCESS;
}

static int s_on_response_body(
    struct aws_http_stream *stream,
    const struct aws_byte_cursor *data,
    void *user_data) {

    (void)stream;
    struct request_context *ctx = user_data;
    aws_byte_buf_append_dynamic(&ctx->body, data);
    return AWS_OP_SUCCESS;
}

static void s_on_stream_complete(struct aws_http_stream *stream, int error_code, void *user_data) {
    struct request_context *ctx = user_data;

    if (error_code == 0) {
        int status = 0;
        aws_http_stream_get_incoming_response_status(stream, &status);
        ctx->status_code = status;
    }
    ctx->error_code = error_code;

    aws_http_stream_release(stream);

    /* Release connection back to pool */
    if (ctx->connection) {
        aws_http_connection_manager_release_connection(ctx->conn_manager, ctx->connection);
        ctx->connection = NULL;
    }

    /* Signal the waiting thread */
    aws_mutex_lock(&ctx->mutex);
    ctx->complete = true;
    aws_condition_variable_notify_one(&ctx->cv);
    aws_mutex_unlock(&ctx->mutex);
}

static void s_on_connection_acquired(struct aws_http_connection *connection, int error_code, void *user_data) {
    struct request_context *ctx = user_data;

    if (error_code || !connection) {
        ctx->error_code = error_code ? error_code : AWS_ERROR_UNKNOWN;
    } else {
        ctx->connection = connection;
    }

    aws_mutex_lock(&ctx->mutex);
    ctx->conn_ready = true;
    aws_condition_variable_notify_one(&ctx->cv);
    aws_mutex_unlock(&ctx->mutex);
}

/* ---- Public API ---- */

void aws_crt_lua_init(void) {
    aws_http_library_init(aws_default_allocator());
}

void aws_crt_lua_cleanup(void) {
    aws_http_library_clean_up();
}

aws_crt_lua_client *aws_crt_lua_client_new(size_t max_connections) {
    struct aws_allocator *alloc = aws_default_allocator();
    struct aws_crt_lua_client *client = aws_mem_calloc(alloc, 1, sizeof(struct aws_crt_lua_client));
    client->allocator = alloc;

    /* Event loop group */
    client->event_loop_group = aws_event_loop_group_new_default(alloc, 1, NULL);

    /* Host resolver */
    struct aws_host_resolver_default_options resolver_opts = {
        .el_group = client->event_loop_group,
        .max_entries = 8,
    };
    client->host_resolver = aws_host_resolver_new_default(alloc, &resolver_opts);

    /* Client bootstrap */
    struct aws_client_bootstrap_options bootstrap_opts = {
        .event_loop_group = client->event_loop_group,
        .host_resolver = client->host_resolver,
    };
    client->bootstrap = aws_client_bootstrap_new(alloc, &bootstrap_opts);

    /* TLS context (for HTTPS) */
    struct aws_tls_ctx_options tls_opts;
    aws_tls_ctx_options_init_default_client(&tls_opts, alloc);
    client->tls_ctx = aws_tls_client_ctx_new(alloc, &tls_opts);
    aws_tls_ctx_options_clean_up(&tls_opts);

    (void)max_connections; /* stored for future connection manager per-host pooling */

    return client;
}

void aws_crt_lua_client_free(aws_crt_lua_client *client) {
    if (!client) return;

    if (client->tls_ctx) aws_tls_ctx_release(client->tls_ctx);
    if (client->bootstrap) aws_client_bootstrap_release(client->bootstrap);
    if (client->host_resolver) aws_host_resolver_release(client->host_resolver);
    if (client->event_loop_group) aws_event_loop_group_release(client->event_loop_group);

    aws_mem_release(client->allocator, client);
}

aws_crt_lua_response *aws_crt_lua_request(
    aws_crt_lua_client *client,
    const aws_crt_lua_request_options *options) {

    struct aws_allocator *alloc = client->allocator;
    aws_crt_lua_response *response = aws_mem_calloc(alloc, 1, sizeof(aws_crt_lua_response));

    /* Parse the URI */
    struct aws_uri uri;
    struct aws_byte_cursor uri_cursor = aws_byte_cursor_from_c_str(options->uri);
    if (aws_uri_init_parse(&uri, alloc, &uri_cursor)) {
        response->error_code = aws_last_error();
        response->error_message = aws_error_str(response->error_code);
        return response;
    }

    /* Determine if TLS is needed */
    struct aws_byte_cursor scheme = *aws_uri_scheme(&uri);
    bool use_tls = aws_byte_cursor_eq_c_str_ignore_case(&scheme, "https");
    uint32_t port = aws_uri_port(&uri);
    if (port == 0) port = use_tls ? 443 : 80;

    /* Set up TLS connection options if needed */
    struct aws_tls_connection_options tls_conn_opts;
    AWS_ZERO_STRUCT(tls_conn_opts);
    if (use_tls) {
        aws_tls_connection_options_init_from_ctx(&tls_conn_opts, client->tls_ctx);
        struct aws_byte_cursor host = *aws_uri_host_name(&uri);
        aws_tls_connection_options_set_server_name(&tls_conn_opts, alloc, &host);
    }

    /* Create a per-request connection manager (simple approach for PoC) */
    struct aws_http_connection_manager_options cm_opts = {
        .bootstrap = client->bootstrap,
        .host = *aws_uri_host_name(&uri),
        .port = port,
        .max_connections = 1,
        .socket_options = &(struct aws_socket_options){
            .type = AWS_SOCKET_STREAM,
            .domain = AWS_SOCKET_IPV4,
            .connect_timeout_ms = 10000,
        },
        .tls_connection_options = use_tls ? &tls_conn_opts : NULL,
    };
    struct aws_http_connection_manager *conn_manager =
        aws_http_connection_manager_new(alloc, &cm_opts);

    if (!conn_manager) {
        response->error_code = aws_last_error();
        response->error_message = aws_error_str(response->error_code);
        aws_uri_clean_up(&uri);
        if (use_tls) aws_tls_connection_options_clean_up(&tls_conn_opts);
        return response;
    }

    /* Set up request context */
    struct request_context ctx;
    AWS_ZERO_STRUCT(ctx);
    ctx.allocator = alloc;
    aws_mutex_init(&ctx.mutex);
    aws_condition_variable_init(&ctx.cv);
    ctx.conn_manager = conn_manager;
    aws_byte_buf_init(&ctx.body, alloc, 4096);

    /* Acquire connection (async, but we'll wait) */
    aws_http_connection_manager_acquire_connection(conn_manager, s_on_connection_acquired, &ctx);

    /* Wait for connection */
    aws_mutex_lock(&ctx.mutex);
    aws_condition_variable_wait_pred(&ctx.cv, &ctx.mutex, s_conn_ready, &ctx);
    aws_mutex_unlock(&ctx.mutex);

    if (ctx.error_code || !ctx.connection) {
        response->error_code = ctx.error_code ? ctx.error_code : AWS_ERROR_UNKNOWN;
        response->error_message = aws_error_str(response->error_code);
        goto cleanup;
    }

    /* Reset completion flag for the actual request */
    ctx.complete = false;
    ctx.error_code = 0;

    /* Build the HTTP request */
    struct aws_http_message *request = aws_http_message_new_request(alloc);
    aws_http_message_set_request_method(request, aws_byte_cursor_from_c_str(options->method));

    /* Path = path + query from URI */
    struct aws_byte_cursor path = *aws_uri_path(&uri);
    struct aws_byte_cursor query = *aws_uri_query_string(&uri);

    struct aws_byte_buf path_buf;
    aws_byte_buf_init(&path_buf, alloc, path.len + query.len + 2);
    aws_byte_buf_append(&path_buf, &path);
    if (query.len > 0) {
        struct aws_byte_cursor q = aws_byte_cursor_from_c_str("?");
        aws_byte_buf_append(&path_buf, &q);
        aws_byte_buf_append(&path_buf, &query);
    }
    aws_http_message_set_request_path(request, aws_byte_cursor_from_buf(&path_buf));
    aws_byte_buf_clean_up(&path_buf);

    /* Add Host header */
    struct aws_byte_cursor host_name = *aws_uri_host_name(&uri);
    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_c_str("Host"),
        .value = host_name,
    };
    aws_http_message_add_header(request, host_header);

    /* Add user headers */
    for (size_t i = 0; i < options->num_headers; i++) {
        struct aws_http_header h = {
            .name = aws_byte_cursor_from_array(options->headers[i].name, options->headers[i].name_len),
            .value = aws_byte_cursor_from_array(options->headers[i].value, options->headers[i].value_len),
        };
        aws_http_message_add_header(request, h);
    }

    /* Body */
    struct aws_input_stream *body_stream = NULL;
    if (options->body && options->body_len > 0) {
        struct aws_byte_cursor body_cursor = aws_byte_cursor_from_array(options->body, options->body_len);
        body_stream = aws_input_stream_new_from_cursor(alloc, &body_cursor);
        aws_http_message_set_body_stream(request, body_stream);

        /* Add Content-Length header */
        char content_length_str[32];
        snprintf(content_length_str, sizeof(content_length_str), "%zu", options->body_len);
        struct aws_http_header cl_header = {
            .name = aws_byte_cursor_from_c_str("Content-Length"),
            .value = aws_byte_cursor_from_c_str(content_length_str),
        };
        aws_http_message_add_header(request, cl_header);
    }

    /* Make the request */
    struct aws_http_make_request_options req_opts = {
        .self_size = sizeof(req_opts),
        .request = request,
        .user_data = &ctx,
        .on_response_headers = s_on_response_headers,
        .on_response_body = s_on_response_body,
        .on_complete = s_on_stream_complete,
    };

    struct aws_http_stream *stream = aws_http_connection_make_request(ctx.connection, &req_opts);
    if (!stream) {
        ctx.error_code = aws_last_error();
        aws_http_message_destroy(request);
        if (body_stream) aws_input_stream_release(body_stream);
        aws_http_connection_manager_release_connection(conn_manager, ctx.connection);
        ctx.connection = NULL;
        response->error_code = ctx.error_code;
        response->error_message = aws_error_str(ctx.error_code);
        goto cleanup;
    }

    aws_http_stream_activate(stream);

    /* Wait for response */
    aws_mutex_lock(&ctx.mutex);
    aws_condition_variable_wait_pred(&ctx.cv, &ctx.mutex, s_request_done, &ctx);
    aws_mutex_unlock(&ctx.mutex);

    aws_http_message_destroy(request);
    if (body_stream) aws_input_stream_release(body_stream);

    /* Fill response */
    if (ctx.error_code) {
        response->error_code = ctx.error_code;
        response->error_message = aws_error_str(ctx.error_code);
    } else {
        response->status_code = ctx.status_code;

        /* Transfer headers: build the public header array pointing into the byte_buf storage */
        if (ctx.num_headers > 0) {
            response->headers = aws_mem_calloc(alloc, ctx.num_headers, sizeof(aws_crt_lua_header));
            response->num_headers = ctx.num_headers;
            for (size_t i = 0; i < ctx.num_headers; i++) {
                response->headers[i].name = (const char *)ctx.headers[i].name.buffer;
                response->headers[i].name_len = ctx.headers[i].name.len;
                response->headers[i].value = (const char *)ctx.headers[i].value.buffer;
                response->headers[i].value_len = ctx.headers[i].value.len;
            }
            /* ctx.headers array ownership transfers to response (freed in response_free) */
        }

        /* Transfer body ownership */
        if (ctx.body.len > 0) {
            response->body = ctx.body.buffer;
            response->body_len = ctx.body.len;
            AWS_ZERO_STRUCT(ctx.body);
        }
    }

cleanup:
    /* Only clean up body if it wasn't transferred */
    if (ctx.body.buffer) aws_byte_buf_clean_up(&ctx.body);
    /* Only clean up headers if they weren't transferred */
    if (response->error_code && ctx.headers) {
        for (size_t i = 0; i < ctx.num_headers; i++) {
            aws_byte_buf_clean_up(&ctx.headers[i].name);
            aws_byte_buf_clean_up(&ctx.headers[i].value);
        }
        aws_mem_release(alloc, ctx.headers);
    }
    aws_mutex_clean_up(&ctx.mutex);
    aws_condition_variable_clean_up(&ctx.cv);
    aws_uri_clean_up(&uri);
    if (use_tls) aws_tls_connection_options_clean_up(&tls_conn_opts);

    /* Release connection manager */
    aws_http_connection_manager_release(conn_manager);

    return response;
}

/**
 * Internal: the response owns both the public aws_crt_lua_header array AND
 * the underlying header_entry byte_bufs (stored as a parallel array stashed
 * right after the public headers in memory). We use a simpler scheme: the
 * header name/value pointers in the public struct point into separately
 * allocated byte_buf buffers. We store the header_entry array pointer in
 * a hidden slot.
 *
 * Actually for simplicity: we just iterate and free each name/value buffer.
 */
void aws_crt_lua_response_free(aws_crt_lua_response *response) {
    if (!response) return;
    struct aws_allocator *alloc = aws_default_allocator();

    if (response->headers) {
        for (size_t i = 0; i < response->num_headers; i++) {
            /* These point into aws_byte_buf.buffer allocations */
            aws_mem_release(alloc, (void *)response->headers[i].name);
            aws_mem_release(alloc, (void *)response->headers[i].value);
        }
        aws_mem_release(alloc, response->headers);
    }

    if (response->body) {
        aws_mem_release(alloc, response->body);
    }

    aws_mem_release(alloc, response);
}
