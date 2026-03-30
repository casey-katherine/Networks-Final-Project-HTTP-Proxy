#ifndef HTTPPROXY_H
#define HTTPPROXY_H

#include <stdint.h>

int create_listen_socket(int listen_port);
int parse_url(const char *buffer, char *host, char *path, int *port);
int connect_to_server(const char *host, uint16_t port);
int fetch_from_server(const char *host, const char *path, const char *req_buf,
                      int req_len, char **resp_out, int *resp_size, uint16_t port);
void add_proxy_header(const char *original_resp, int original_size,
                      char **modified_resp, int *modified_size);
void send_page(const char* resp_data, const char *path, int total, int *final_size, char **final_resp);


#endif