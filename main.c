#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/select.h>
#include <time.h>
#include <stdbool.h>
#include <string.h>
#include <signal.h>
#include "http_proxy.h"
#include "cache.h"
#include "SSL.h"
#include "llm.h"

#define MAX_FILE_SIZE (10 * 1024 * 1024) // 10 MB
#define MAX_URL   4096
Cache c;

typedef struct {
    char *partial_buff;
    int partial_len;
    int buffer_cap;
} ClientState;
ClientState clients[FD_SETSIZE];

extern pthread_mutex_t cache_mutex;

void handle_http_get(int fd, const char *buffer, const char *url,
                     const char *host, const char *path, int port, int msg_size);


int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: <port> <ca_cert_path> <ca_key_path>\n");
        return EXIT_FAILURE;
    }

    int proxy_port = atoi(argv[1]);
    char *ca_cert_path = argv[2];
    char *ca_key_path  = argv[3];

    // Init SSL contexts and cache
    init_ssl_contexts(ca_cert_path, ca_key_path);
    cache_init(&c, 100);
    signal(SIGPIPE, SIG_IGN);

    // Make a listening socket
    int server_fd = create_listen_socket(proxy_port);
    if (server_fd < 0) return EXIT_FAILURE;

    fd_set master, readfds;
    FD_ZERO(&master);
    FD_SET(server_fd, &master);
    int fdmax = server_fd;

    // init client array
    memset(clients, 0, sizeof(clients));

    while (1) {
        readfds = master;
        struct timeval tv = {0, 100000};
        int s = select(fdmax + 1, &readfds, NULL, NULL, &tv);
        if (s < 0) exit(1);

        for (int i = 0; i <= fdmax; i++) {
            if (!FD_ISSET(i, &readfds)) continue;

            // a new connection is waiting to be accepted
            if (i == server_fd) {
                struct sockaddr_in clientAddr;
                socklen_t clientLen = sizeof(clientAddr);
                int newfd = accept(server_fd, (struct sockaddr *)&clientAddr, &clientLen);
                if (newfd == -1) continue;

                FD_SET(newfd, &master);
                if (newfd > fdmax) {
                    fdmax = newfd;
                }
            } else {
                char temp_buff[4096];
                int n = read(i, temp_buff, 4096);
                // conn closed or error
                if (n <= 0) {
                    if (clients[i].partial_buff) {
                        free(clients[i].partial_buff);
                        clients[i].partial_buff = NULL;
                    }
                    close(i);
                    FD_CLR(i, &master);
                    continue;
                }

                // Append to partial buffer
                if (!clients[i].partial_buff) {
                    clients[i].buffer_cap = MAX_FILE_SIZE;
                    clients[i].partial_buff = malloc(MAX_FILE_SIZE);
                    clients[i].partial_len = 0;
                }

                // Check for buffer overflow
                if (clients[i].partial_len + n > clients[i].buffer_cap) {
                    free(clients[i].partial_buff);
                    clients[i].partial_buff = NULL;
                    close(i);
                    FD_CLR(i, &master);
                    continue;
                }

                // Copy new data into partial buffer
                memcpy(clients[i].partial_buff + clients[i].partial_len, temp_buff, n);
                clients[i].partial_len += n;
                clients[i].partial_buff[clients[i].partial_len] = '\0';

                // Check if we have a complete request
                if (strstr(clients[i].partial_buff, "\r\n\r\n") == NULL) {
                    continue; // Wait for more data
                }

                // complete request!
                char *buffer = clients[i].partial_buff;
                int msg_size = clients[i].partial_len;

                if (strncmp(buffer, "CONNECT", 7) == 0) {
                    char connect_host[256];
                    int connect_port;
                    sscanf(buffer, "CONNECT %255[^:]:%d", connect_host, &connect_port);

                    free(clients[i].partial_buff);
                    clients[i].partial_buff = NULL;
                    clients[i].partial_len = 0;

                    int result = handle_https_connect(i, connect_host, connect_port);
                    if (result != 0) {
                        close(i);
                    }
                    FD_CLR(i, &master);
                } else if (strncmp(buffer, "GET", 3) == 0) {
                    char host[256], path[1024], url[MAX_URL];
                    int port;

                    if (parse_url(buffer, host, path, &port) < 0) {
                        continue;
                    }

                    snprintf(url, sizeof(url), "%s:%d%s", host, port, path);

                    // Plain HTTP request
                    handle_http_get(i, buffer, url, host, path, port, msg_size);

                    const char *conn_header = strcasestr(buffer, "Connection:");
                    bool should_close = false;
                    if (conn_header) {
                        char conn_val[256];
                        sscanf(conn_header, "Connection: %255[^\r\n]", conn_val);
                        if (strcasecmp(conn_val, "close") == 0) {
                            should_close = true;
                        }
                    }

                    // reset the buffer!
                    free(clients[i].partial_buff);
                    clients[i].partial_buff = NULL;
                    clients[i].partial_len = 0;

                    if (should_close) {
                        close(i);
                        FD_CLR(i, &master);
                    }
                } else {
                    // Unsupported request
                    const char *resp = "HTTP/1.1 400 Bad Request\r\n\r\n";
                    write(i, resp, strlen(resp));
                    free(clients[i].partial_buff);
                    clients[i].partial_buff = NULL;
                    clients[i].partial_len = 0;
                    close(i);
                    FD_CLR(i, &master);
                }
            }
        }
    }

    cleanup_ssl_contexts();
    close(server_fd);
    return EXIT_SUCCESS;
}

void handle_http_get(int fd, const char *buffer, const char *url,
                     const char *host, const char *path, int port, int msg_size)
{
    // Check if this is a HEAD request (for style checking)
    bool is_head = strncmp(buffer, "HEAD", 4) == 0;

    // Extract style from URL
    char *style = extract_style_from_url(url);
    if (!style) style = strdup("none");

    // Generate cache key without style parameter
    char cache_key[MAX_URL];
    strncpy(cache_key, url, sizeof(cache_key));
    char *q = strchr(cache_key, '?');
    if (q) *q = '\0';

    // Handle HEAD requests FIRST - these are for polling
    if (is_head) {
        printf("[HEAD REQUEST] Checking style '%s' for URL: %s\n", style, cache_key);
        fflush(stdout);

        pthread_mutex_lock(&cache_mutex);
        CacheEntry *entry = cache_get(&c, cache_key);

        bool style_ready = false;
        size_t content_len = 0;

        if (entry) {
            printf("[HEAD] Cache entry found!\n");
            // Check which style is requested and if it's ready
            if (strcmp(style, "pirate") == 0) {
                style_ready = (entry->pirate_body && entry->pirate_size > 0);
                content_len = style_ready ? entry->pirate_size : 0;
            } else if (strcmp(style, "simplified") == 0) {
                style_ready = (entry->simple_body && entry->simple_size > 0);
                content_len = style_ready ? entry->simple_size : 0;
            } else if (strcmp(style, "GenZ") == 0) {
                style_ready = (entry->genz_body && entry->genz_size > 0);
                content_len = style_ready ? entry->genz_size : 0;
            } else if (strcmp(style, "tldr") == 0) {
                style_ready = (entry->TLDR_body && entry->TLDR_size > 0);
                content_len = style_ready ? entry->TLDR_size : 0;
            } else {
                // "none" style - always ready if entry exists
                style_ready = true;
                content_len = entry->html_body ? strlen(entry->html_body) : 0;
            }
        }

        pthread_mutex_unlock(&cache_mutex);
        fflush(stdout);

        char head_response[512];
        int head_size = snprintf(head_response, sizeof(head_response),
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/html; charset=utf-8\r\n"
            "Content-Length: %zu\r\n"
            "X-Style-Ready: %s\r\n"
            "Cache-Control: no-store, no-cache, must-revalidate, max-age=0\r\n"
            "Pragma: no-cache\r\n"
            "Expires: 0\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Access-Control-Expose-Headers: X-Style-Ready\r\n"
            "Connection: close\r\n"
            "\r\n",
            content_len,
            style_ready ? "true" : "false");

        send(fd, head_response, head_size, 0);
        free(style);
        return;
    }

    // Regular GET request handling below...
    pthread_mutex_lock(&cache_mutex);
    CacheEntry *entry = cache_get(&c, cache_key);
    pthread_mutex_unlock(&cache_mutex);

    if (entry) {
        pthread_mutex_lock(&cache_mutex);

        // Check which styles are ready
        bool pirate_ready = (entry->pirate_body && entry->pirate_size > 0);
        bool simple_ready = (entry->simple_body && entry->simple_size > 0);
        bool genz_ready = (entry->genz_body && entry->genz_size > 0);
        bool tldr_ready = (entry->TLDR_body && entry->TLDR_size > 0);

        // For ALL GET requests, determine which HTML to serve
        char *html_to_serve = NULL;

        if (strcmp(style, "none") == 0) {
            html_to_serve = entry->html_body;
        } else if (strcmp(style, "pirate") == 0 && pirate_ready) {
            html_to_serve = entry->pirate_body;
        } else if (strcmp(style, "simplified") == 0 && simple_ready) {
            html_to_serve = entry->simple_body;
        } else if (strcmp(style, "GenZ") == 0 && genz_ready) {
            html_to_serve = entry->genz_body;
        } else if (strcmp(style, "tldr") == 0 && tldr_ready) {
            html_to_serve = entry->TLDR_body;
        } else {
            // Style not ready, fall back to original
            html_to_serve = entry->html_body;
        }

        // Inject selector with current ready states into whatever we're serving
        if (html_to_serve) {
            char *updated_html = inject_style_selector(html_to_serve, pirate_ready,
                                                      simple_ready, genz_ready, tldr_ready);
            pthread_mutex_unlock(&cache_mutex);

            if (updated_html) {
                size_t html_len = strlen(updated_html);
                char *response = malloc(html_len + 1024);
                int resp_size = snprintf(response, html_len + 1024,
                    "HTTP/1.1 200 OK\r\n"
                    "Content-Type: text/html; charset=utf-8\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: keep-alive\r\n"
                    "\r\n"
                    "%s",
                    html_len, updated_html);
                send(fd, response, resp_size, 0);
                free(response);
                free(updated_html);
                free(style);
                return;
            }
        }

        // Fallback
        add_age(entry);
        pthread_mutex_unlock(&cache_mutex);
        send(fd, entry->content_age, entry->size_age, 0);

    } else {
        // Cache miss - fetch from server
        char *resp_data = NULL;
        int resp_size = 0;

        if (fetch_from_server(host, path, buffer, msg_size, &resp_data,
                              &resp_size, port) == 0) {

            // Extract raw HTML body
            int body_len = 0;
            char *raw_body = extract_http_body(resp_data, resp_size, &body_len);

            // INJECT SELECTOR ON FIRST LOAD (all styles will be "loading")
            if (raw_body && body_len > 0) {
                char *html_with_selector = inject_style_selector(raw_body, false, false, false, false);

                if (html_with_selector) {
                    // Send modified response with selector
                    size_t html_len = strlen(html_with_selector);
                    char *response = malloc(html_len + 1024);
                    int resp_size_new = snprintf(response, html_len + 1024,
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/html; charset=utf-8\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: keep-alive\r\n"
                        "\r\n"
                        "%s",
                        html_len, html_with_selector);
                    send(fd, response, resp_size_new, 0);
                    free(response);
                    free(html_with_selector);
                } else {
                    // Fallback to original
                    send(fd, resp_data, resp_size, 0);
                }
            } else {
                // No HTML body, send original
                send(fd, resp_data, resp_size, 0);
            }

            int64_t max_age = find_MA(resp_data);
            max_age = max_age * NS;

            if (max_age > 0 && raw_body && body_len > 0) {
                // Store in cache with HTML body (WITHOUT selector)
                pthread_mutex_lock(&cache_mutex);
                cache_put(&c, cache_key, resp_data, resp_size, max_age, raw_body, body_len);
                pthread_mutex_unlock(&cache_mutex);

                // Start preloading in background
                char *key_copy = strdup(cache_key);
                pthread_t preload_thread;
                pthread_create(&preload_thread, NULL, cache_preload_all_styles, key_copy);
                pthread_detach(preload_thread);
            }

            free(resp_data);
        }
    }

    free(style);
}

