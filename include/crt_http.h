#ifndef AWS_CRT_LUA_HTTP_H
#define AWS_CRT_LUA_HTTP_H

/**
 * aws-crt-lua: Synchronous HTTP client shim over the AWS CRT.
 *
 * This provides a blocking request/response API suitable for consumption
 * via LuaJIT FFI. Internally it uses the CRT's async HTTP client and
 * blocks the calling thread until the response is complete.
 */

#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#define AWS_CRT_LUA_API __declspec(dllexport)
#else
#define AWS_CRT_LUA_API __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque handle to the CRT HTTP client (owns event loop, connection manager). */
typedef struct aws_crt_lua_client aws_crt_lua_client;

/** A single header key-value pair (borrowed pointers, valid until response is freed). */
typedef struct aws_crt_lua_header {
    const char *name;
    size_t name_len;
    const char *value;
    size_t value_len;
} aws_crt_lua_header;

/** Request options passed to aws_crt_lua_request. */
typedef struct aws_crt_lua_request_options {
    const char *method;
    const char *uri;                /* full URL: https://host/path */
    const aws_crt_lua_header *headers;
    size_t num_headers;
    const uint8_t *body;
    size_t body_len;
} aws_crt_lua_request_options;

/** Response returned from aws_crt_lua_request. Must be freed with aws_crt_lua_response_free. */
typedef struct aws_crt_lua_response {
    int status_code;
    aws_crt_lua_header *headers;
    size_t num_headers;
    uint8_t *body;
    size_t body_len;
    int error_code;                 /* 0 on success, CRT error code on failure */
    const char *error_message;      /* human-readable, NULL on success */
} aws_crt_lua_response;

/** Initialize the CRT. Call once at startup. */
AWS_CRT_LUA_API void aws_crt_lua_init(void);

/** Clean up the CRT. Call once at shutdown. */
AWS_CRT_LUA_API void aws_crt_lua_cleanup(void);

/** Create an HTTP client. max_connections controls the connection pool size. */
AWS_CRT_LUA_API aws_crt_lua_client *aws_crt_lua_client_new(size_t max_connections);

/** Destroy an HTTP client. */
AWS_CRT_LUA_API void aws_crt_lua_client_free(aws_crt_lua_client *client);

/**
 * Make a synchronous HTTP request. Blocks until the full response is received.
 * Returns a response that must be freed with aws_crt_lua_response_free.
 * Never returns NULL — check response->error_code.
 */
AWS_CRT_LUA_API aws_crt_lua_response *aws_crt_lua_request(
    aws_crt_lua_client *client,
    const aws_crt_lua_request_options *options);

/** Free a response. */
AWS_CRT_LUA_API void aws_crt_lua_response_free(aws_crt_lua_response *response);

#ifdef __cplusplus
}
#endif

#endif /* AWS_CRT_LUA_HTTP_H */
