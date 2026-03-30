#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <time.h>
#include "http_proxy.h"
#include "llm.h"

#define MAX_FILE_SIZE (10 * 1024 * 1024) // 10 MB

// Create listening socket for clients
int create_listen_socket(int listen_port)
{
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(listen_port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 80) < 0) { // backlog
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

// get addr info
int connect_to_server(const char *host, uint16_t port)
{
    struct hostent *he = gethostbyname(host);
    if (!he) return -1;

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr, he->h_addr, he->h_length);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return -1;
    }

    return sock;
}

int parse_url(const char *buffer, char *host, char *path, int *port)
{
    // Check for https first, then http
    const char *url_start = strstr(buffer, "https://");
    if (url_start) {
        *port = 443;  // default https port
        url_start += 8;  // skip "https://"
    } else {
        url_start = strstr(buffer, "http://");
        if (!url_start) return -1;
        *port = 80;  // default http port
        url_start += 7;  // skip "http://"
    }

    const char *path_start = strchr(url_start, '/');
    if (!path_start) return -1;

    // Copy path until space or end
    if (sscanf(path_start, "%[^ \r\n]", path) != 1) {
        return -1;
    }

    // Extract host (and optional port) from Host header
    const char *host_line = strstr(buffer, "Host: ");
    if (!host_line) return -1;
    host_line += 6;

    // Try to parse host:port format first
    int scanned = sscanf(host_line, "%[^:\r\n]:%d", host, port);

    if (scanned == 1) {
        // No port found, port stays at default (80 or 443)
    } else if (scanned != 2) {
        // Failed to parse
        return -1;
    }

    return 0;
}

// fetch from server and send to client
int fetch_from_server(const char *host, const char *path, const char *req_buf,
                      int req_len, char **resp_out, int *resp_size, uint16_t port)
{
    int server_fd = connect_to_server(host, port);
    if (server_fd < 0) {
        return -1;
    }

    char *new_request = malloc(MAX_FILE_SIZE);
    if (port != 80) {
        snprintf(new_request, MAX_FILE_SIZE,
                 "GET %s HTTP/1.1\r\nHost: %s:%d\r\n\r\n",
                 path, host, port);
    } else {
        snprintf(new_request, MAX_FILE_SIZE,
                 "GET %s HTTP/1.1\r\nHost: %s\r\n\r\n",
                 path, host);
    }

    int sent = send(server_fd, new_request, strlen(new_request), 0);
    free(new_request);
    if (sent < 0) {
        close(server_fd);
        return -1;
    }

    // Receive response with Content-Length parsing
    char *resp_data = malloc(MAX_FILE_SIZE);
    if (!resp_data) {
        close(server_fd);
        return -1;
    }

    int total = 0;
    int n;
    int headers_complete = 0;
    int content_length = -1;
    int header_len = 0;
    int is_chunked = 0;

    // Read until we have complete response
    while (total < MAX_FILE_SIZE) {
        n = recv(server_fd, resp_data + total, MAX_FILE_SIZE - total, 0);
        if (n <= 0) {
            break;
        }
        total += n;
        resp_data[total] = '\0';

        // Check if headers are complete
        if (!headers_complete) {
            char *header_end = strstr(resp_data, "\r\n\r\n");
            if (header_end) {
                headers_complete = 1;
                header_len = (header_end - resp_data) + 4;

                // Check for chunked encoding
                char *transfer_enc = strcasestr(resp_data, "Transfer-Encoding:");
                if (transfer_enc) {
                    char encoding[64];
                    sscanf(transfer_enc, "Transfer-Encoding: %63[^\r\n]", encoding);
                    if (strcasestr(encoding, "chunked")) {
                        is_chunked = 1;
                    }
                }

                // Parse Content-Length (only if not chunked)
                if (!is_chunked) {
                    char *cl_header = strcasestr(resp_data, "Content-Length:");
                    if (cl_header) {
                        sscanf(cl_header, "Content-Length: %d", &content_length);
                    }
                }
            }
        }
        // If headers done, check if we're complete
        if (headers_complete) {
            if (is_chunked) {
                // Check for end of chunked encoding: "0\r\n\r\n"
                if (strstr(resp_data + header_len, "\r\n0\r\n\r\n")) {
                    break;
                }
            } else if (content_length >= 0) {
                // Check if we have all the content
                int body_received = total - header_len;
                if (body_received >= content_length) {
                    break;
                }
            }
        }
    }

    // Ensure some response
    if (total == 0) {
        free(resp_data);
        close(server_fd);
        return -1;
    }

    char *final_resp;
    int final_size;

    send_page(resp_data, path, total, &final_size, &final_resp);

    *resp_out = final_resp;
    *resp_size = final_size;

    free(resp_data);
    close(server_fd);
    return 0;
}

void send_page(const char *resp_data, const char *path, int total, int *final_size, char **final_resp)
{
    int is_html = 0;
    char *content_type = strcasestr(resp_data, "Content-Type:");
    if (content_type) {
        char type[128];
        sscanf(content_type, "Content-Type: %127[^\r\n]", type);
        if (strcasestr(type, "text/html")) {
            is_html = 1;
        }
    }

    if (is_html) {
        // Convert webpage with style selector
        char *llm_response = process_webpage_with_llm(resp_data, total, path);

        if (llm_response) {
            // Wrap LLM response in HTTP headers
            size_t llm_len = strlen(llm_response);
            *final_resp = malloc(llm_len + 1024);
            if (!*final_resp) {
                perror("malloc failed");
                *final_size = 0;
                free(llm_response);
                return;
            }

            *final_size = snprintf(*final_resp, llm_len + 1024,
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/html; charset=utf-8\r\n"
                "Content-Length: %zu\r\n"
                "Connection: close\r\n"
                "\r\n"
                "%s",
                llm_len,
                llm_response
            );

            free(llm_response);
        } else {
            // If LLM fails, send original response with proxy header
            add_proxy_header(resp_data, total, final_resp, final_size);
        }
    } else {
        // Not HTML - just add proxy header and pass through
        add_proxy_header(resp_data, total, final_resp, final_size);
    }
}


void add_proxy_header(const char *original_resp, int original_size,
                      char **modified_resp, int *modified_size)
{
    char *header_end = strstr(original_resp, "\r\n\r\n");
    if (!header_end) {
        // No proper headers found, return as-is
        *modified_resp = malloc(original_size);
        memcpy(*modified_resp, original_resp, original_size);
        *modified_size = original_size;
        return;
    }

    int header_len = header_end - original_resp;
    int body_len = original_size - (header_len + 4);

    char *new_resp = malloc(MAX_FILE_SIZE);
    if (!new_resp) {
        *modified_resp = NULL;
        *modified_size = 0;
        return;
    }

    // Copy original headers
    memcpy(new_resp, original_resp, header_len);

    // Add X-Proxy header and end of headers
    int new_len = header_len;
    new_len += snprintf(new_resp + new_len, MAX_FILE_SIZE - new_len,
                        "\r\nX-Proxy:CS112\r\n\r\n");

    // Copy body
    memcpy(new_resp + new_len, header_end + 4, body_len);
    new_len += body_len;

    *modified_resp = new_resp;
    *modified_size = new_len;
}

