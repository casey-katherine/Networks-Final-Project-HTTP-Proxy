#ifndef SSL_H
#define SSL_H

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <stdbool.h>

typedef struct {
    int client_fd;
    int server_fd;
    SSL *client_ssl;
    SSL *server_ssl;
    SSL_CTX *client_ctx;
} tunnel_args_t;
void *tunnel_thread(void *arg);

void init_ssl_contexts(const char *ca_cert_path, const char *ca_key_path);
void cleanup_ssl_contexts();
int handle_https_connect(int client_fd, const char *host, int port);
void relay_ssl_traffic(SSL *client_ssl, SSL *server_ssl, int client_fd, int server_fd);
SSL_CTX *create_fake_cert_context(const char *hostname);

#endif