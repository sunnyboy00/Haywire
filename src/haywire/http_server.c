#pragma comment (lib, "libuv.lib")
#pragma comment (lib, "ws2_32.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "Iphlpapi.lib")

#ifdef PLATFORM_POSIX
#include <signal.h>
#endif // PLATFORM_POSIX

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "uv.h"
#include "haywire.h"
#include "http_server.h"
#include "http_request.h"
#include "http_parser.h"
#include "http_connection.h"
#include "http_response_cache.h"
#include "server_stats.h"
#include "khash.h"
#include "route_compare_method.h"
#include "configuration/configuration.h"

#define UVERR(err, msg) fprintf(stderr, "%s: %s\n", msg, uv_strerror(err))
#define CHECK(r, msg) \
  if (r) { \
    uv_err_t err = uv_last_error(uv_loop); \
    UVERR(err, msg); \
    exit(1); \
  }

KHASH_MAP_INIT_STR(string_hashmap, char*)

static configuration* config;
static uv_tcp_t server;
static http_parser_settings parser_settings;

uv_loop_t* uv_loop;
void* routes;

http_connection* create_http_connection()
{
    http_connection* connection = malloc(sizeof(http_connection));
    connection->request = NULL;
    INCREMENT_STAT(stat_connections_created_total);
    return connection;
}

void free_http_connection(http_connection* connection)
{
    if (connection->request != NULL)
    {
        free_http_request(connection->request);
    }
    
    free(connection);
    INCREMENT_STAT(stat_connections_destroyed_total);
}

void set_route(void* hashmap, char* name, hw_route_entry* route_entry)
{
    int ret;
    khiter_t k;
    khash_t(string_hashmap) *h = hashmap;
    k = kh_put(string_hashmap, h, strdup(name), &ret);
    kh_value(h, k) = route_entry;
}

void hw_http_add_route(char *route, http_request_callback callback, void* user_data)
{
    hw_route_entry* route_entry = malloc(sizeof(hw_route_entry));
    route_entry->callback = callback;
    route_entry->user_data = user_data;
    
    if (routes == NULL)
    {
        routes = kh_init(string_hashmap);
    }
    set_route(routes, route, route_entry);
    printf("Added route %s\n", route); // TODO: Replace with logging instead.
}

int hw_init_from_config(char* configuration_filename)
{
    configuration* config = load_configuration(configuration_filename);
    if (config == NULL)
    {
        return 1;
    }
    return hw_init_with_config(config);
}

int hw_init_with_config(configuration* configuration)
{    
    int http_listen_address_length;
#ifdef DEBUG
    char route[] = "/stats";    
    hw_http_add_route(route, get_server_stats, NULL);
#endif /* DEBUG */
    /* Copy the configuration */
    http_listen_address_length = strlen(configuration->http_listen_address);
    config = malloc(sizeof(configuration));
    config->http_listen_address = malloc(http_listen_address_length + 1);
    memcpy(config->http_listen_address, configuration->http_listen_address, http_listen_address_length + 1);
    config->http_listen_port = configuration->http_listen_port;
    return 0;
}

void free_http_server()
{
    /* TODO: Shut down accepting incoming requests */
    khash_t(string_hashmap) *h = routes;
    const char* k;
    const char* v;
    kh_foreach(h, k, v, { free((char*)k); free((char*)v); });
    kh_destroy(string_hashmap, routes);
}

int hw_http_open()
{
    parser_settings.on_header_field = http_request_on_header_field;
    parser_settings.on_header_value = http_request_on_header_value;
    parser_settings.on_headers_complete = http_request_on_headers_complete;
    parser_settings.on_body = http_request_on_body;
    parser_settings.on_message_begin = http_request_on_message_begin;
    parser_settings.on_message_complete = http_request_on_message_complete;
    parser_settings.on_url = http_request_on_url;
    
#ifdef PLATFORM_POSIX
    signal(SIGPIPE, SIG_IGN);
#endif // PLATFORM_POSIX
  
    /* TODO: Use the return values from uv_tcp_init() and uv_tcp_bind() */
    uv_loop = uv_default_loop();
    uv_tcp_init(uv_loop, &server);

    initialize_http_request_cache();

    uv_tcp_bind(&server, uv_ip4_addr(config->http_listen_address, config->http_listen_port));
    uv_listen((uv_stream_t*)&server, 128, http_stream_on_connect);

    printf("Listening on %s:%d\n", config->http_listen_address, config->http_listen_port);

    uv_run(uv_loop, UV_RUN_DEFAULT);

    return 0;
}

void http_stream_on_connect(uv_stream_t* stream, int status)
{
    http_connection* connection = create_http_connection();
    uv_tcp_init(uv_loop, &connection->stream);
    http_parser_init(&connection->parser, HTTP_REQUEST);

    connection->parser.data = connection;
    connection->stream.data = connection;

    /* TODO: Use the return values from uv_accept() and uv_read_start() */
    uv_accept(stream, (uv_stream_t*)&connection->stream);
    uv_read_start((uv_stream_t*)&connection->stream, http_stream_on_alloc, http_stream_on_read);
}

uv_buf_t http_stream_on_alloc(uv_handle_t* client, size_t suggested_size)
{
    uv_buf_t buf;
    buf.base = (char *)malloc(suggested_size);
    buf.len = suggested_size;
    return buf;
}

void http_stream_on_close(uv_handle_t* handle)
{
    http_connection* connection = (http_connection*)handle->data;
    free_http_connection(connection);
}

void http_stream_on_read(uv_stream_t* tcp, ssize_t nread, uv_buf_t buf)
{
    size_t parsed;
    http_connection* connection = (http_connection*)tcp->data;

    if (nread >= 0) 
    {
        parsed = http_parser_execute(&connection->parser, &parser_settings, buf.base, nread);
        if (parsed < nread) 
        {
            /* uv_close((uv_handle_t*) &client->handle, http_stream_on_close); */
        }
    } 
    else 
    {
        uv_err_t err = uv_last_error(uv_loop);
        if (err.code != UV_EOF) 
        {
            /* UVERR(err, "read"); */
            if (connection->request != NULL)
            {
                free_http_request(connection->request);
            }
        }
        uv_close((uv_handle_t*) &connection->stream, http_stream_on_close);
    }
    free(buf.base);
}

int http_server_write_response(hw_write_context* write_context, hw_string* response)
{
    uv_write_t* write_req = (uv_write_t *)malloc(sizeof(*write_req) + sizeof(uv_buf_t));
    uv_buf_t* resbuf = (uv_buf_t *)(write_req+1);

    resbuf->base = response->value;
    resbuf->len = response->length + 1;

    write_req->data = write_context;

    /* TODO: Use the return values from uv_write() */
    uv_write(write_req, (uv_stream_t*)&write_context->connection->stream, resbuf, 1, http_server_after_write);
    return 0;
}

void http_server_after_write(uv_write_t* req, int status)
{
    hw_write_context* write_context = (hw_write_context*)req->data;
    uv_buf_t *resbuf = (uv_buf_t *)(req+1);

    if (!write_context->connection->keep_alive)
    {
        uv_close((uv_handle_t*)req->handle, http_stream_on_close);
    }
    
    if (write_context->callback != 0)
    {
        write_context->callback(write_context->user_data);
    }
    
    free(write_context);
    free(resbuf->base);
    free(req);
}
