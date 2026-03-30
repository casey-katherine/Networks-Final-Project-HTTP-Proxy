#include "SSL.h"
#include "http_proxy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <time.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509v3.h>
#include <openssl/rsa.h>
#include <openssl/bn.h>
#include <pthread.h>
#include <sys/time.h>
#include <stdbool.h>

static pthread_mutex_t serial_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long cert_serial_counter = 1;

SSL_CTX *g_server_ctx = NULL;

X509 *g_ca_cert = NULL;
EVP_PKEY *g_ca_key = NULL;

// Entry point for the per-connection HTTPS tunnel thread. Each CONNECT request
// gets its own thread so the main select() loop isn't blocked during SSL I/O.
void *tunnel_thread(void *arg)
{
    tunnel_args_t *args = (tunnel_args_t *)arg;

    relay_ssl_traffic(args->client_ssl, args->server_ssl,
                      args->client_fd, args->server_fd);

    // Two-phase SSL shutdown: the first SSL_shutdown sends our "close notify"
    // alert. A return of 0 means the peer's close notify hasn't arrived yet —
    // calling it a second time waits for that, giving a clean bidirectional
    // shutdown and preventing the peer from getting a truncated-stream error.
    int shutdown_ret = SSL_shutdown(args->client_ssl);
    if (shutdown_ret == 0) {
        SSL_shutdown(args->client_ssl);
    }

    shutdown_ret = SSL_shutdown(args->server_ssl);
    if (shutdown_ret == 0) {
        SSL_shutdown(args->server_ssl);
    }

    SSL_free(args->client_ssl);
    SSL_free(args->server_ssl);
    SSL_CTX_free(args->client_ctx);
    close(args->server_fd);
    close(args->client_fd);
    free(args);

    return NULL;
}


// Load the CA cert and key that the proxy uses to sign per-host fake certificates.
// These globals are read-only after startup, so no locking is needed later.
void init_ssl_contexts(const char *ca_cert_path, const char *ca_key_path)
{
    SSL_library_init();
    SSL_load_error_strings();
    OpenSSL_add_all_algorithms();

    // The CA cert is needed both to sign fake leaf certs and to set the issuer
    // field, so browsers that have trusted the CA root will accept them.
    FILE *ca_cert_file = fopen(ca_cert_path, "r");
    if (!ca_cert_file) {
        exit(EXIT_FAILURE);
    }
    g_ca_cert = PEM_read_X509(ca_cert_file, NULL, NULL, NULL);
    fclose(ca_cert_file);

    if (!g_ca_cert) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    FILE *ca_key_file = fopen(ca_key_path, "r");
    if (!ca_key_file) {
        exit(EXIT_FAILURE);
    }
    g_ca_key = PEM_read_PrivateKey(ca_key_file, NULL, NULL, NULL);
    fclose(ca_key_file);

    if (!g_ca_key) {
        ERR_print_errors_fp(stderr);
        exit(EXIT_FAILURE);
    }

    // Single shared context for all outbound (proxy→server) connections.
    // We disable certificate verification deliberately: the proxy's job is to
    // reach any server the client asks for, including those with self-signed or
    // expired certs, and verifying here would interfere with that.
    g_server_ctx = SSL_CTX_new(TLS_client_method());
    if (!g_server_ctx) {
        exit(EXIT_FAILURE);
    }
    SSL_CTX_set_verify(g_server_ctx, SSL_VERIFY_NONE, NULL);
}


void cleanup_ssl_contexts()
{
    if (g_ca_cert) X509_free(g_ca_cert);
    if (g_ca_key) EVP_PKEY_free(g_ca_key);
    if (g_server_ctx) SSL_CTX_free(g_server_ctx);
}

// Handle an HTTP CONNECT request by establishing a MITM TLS tunnel.
//
// The CONNECT method is how browsers set up HTTPS through a proxy:
//   1. Client sends:  CONNECT example.com:443 HTTP/1.1
//   2. Proxy replies: HTTP/1.1 200 Connection Established
//   3. Client then starts a TLS handshake — now targeting the proxy, not the server.
//
// Because we want to inspect (and eventually transform) the traffic, we act as
// a man-in-the-middle: we present the client a fake cert signed by our CA, while
// simultaneously opening a real TLS connection to the origin server.
int handle_https_connect(int client_fd, const char *host, int port)
{
    int server_fd = connect_to_server(host, port);
    if (server_fd < 0) {
        return -1;
    }

    // Per the CONNECT protocol, the proxy must send 200 before the client will
    // start its TLS handshake. Any failure after this point can't be reported
    // as an HTTP error — the connection is already "upgraded".
    const char *resp = "HTTP/1.1 200 Connection Established\r\n\r\n";
    if (send(client_fd, resp, strlen(resp), 0) != (ssize_t)strlen(resp)) {
        close(server_fd);
        return -1;
    }

    // Generate a fake cert for this specific hostname. We can't use a single
    // wildcard cert because modern browsers check the SAN against the exact
    // hostname and reject mismatches, so each host needs its own cert.
    SSL_CTX *client_ctx = create_fake_cert_context(host);
    if (!client_ctx) {
        close(server_fd);
        return -1;
    }

    // TLS handshake with the browser (proxy acts as the "server").
    // SSL_accept blocks until the client finishes the handshake or fails.
    SSL *client_ssl = SSL_new(client_ctx);
    if (!client_ssl) {
        SSL_CTX_free(client_ctx);
        close(server_fd);
        return -1;
    }
    SSL_set_fd(client_ssl, client_fd);

    int accept_ret = SSL_accept(client_ssl);
    if (accept_ret <= 0) {
        // Handshake failed — browser likely doesn't trust our CA root.
        SSL_free(client_ssl);
        SSL_CTX_free(client_ctx);
        close(server_fd);
        return -1;
    }

    // TLS handshake with the real server (proxy acts as the "client").
    // SNI must be set before SSL_connect so the server sends the right cert
    // for virtual-hosted sites (many servers share one IP).
    SSL *server_ssl = SSL_new(g_server_ctx);
    if (!server_ssl) {
        SSL_free(client_ssl);
        SSL_CTX_free(client_ctx);
        close(server_fd);
        return -1;
    }
    SSL_set_fd(server_ssl, server_fd);
    SSL_set_tlsext_host_name(server_ssl, host);

    if (SSL_connect(server_ssl) <= 0) {
        ERR_print_errors_fp(stderr);
        SSL_free(client_ssl);
        SSL_free(server_ssl);
        SSL_CTX_free(client_ctx);
        close(server_fd);
        return -1;
    }

    // Both handshakes succeeded. Hand off to a background thread so this
    // function can return and the main select() loop can handle other clients.
    // pthread_detach means the thread cleans up automatically when it exits.
    tunnel_args_t *args = malloc(sizeof(tunnel_args_t));
    args->client_fd = client_fd;
    args->server_fd = server_fd;
    args->client_ssl = client_ssl;
    args->server_ssl = server_ssl;
    args->client_ctx = client_ctx;

    pthread_t thread;
    if (pthread_create(&thread, NULL, tunnel_thread, args) != 0) {
        SSL_free(client_ssl);
        SSL_free(server_ssl);
        SSL_CTX_free(client_ctx);
        close(server_fd);
        free(args);
        return -1;
    }
    pthread_detach(thread);

    return 0;
}

// Bidirectional SSL relay loop. Forwards data between the browser and the origin
// server, injecting an X-Proxy header into the first response on each request.
//
// Both sides are serviced in the same loop using select(). We also check
// SSL_pending() on each iteration because OpenSSL may have already decrypted
// data buffered in its internal record layer that select() can't see — relying
// on select() alone would stall after the first SSL record.
void relay_ssl_traffic(SSL *client_ssl, SSL *server_ssl, int client_fd, int server_fd)
{
    fd_set readfds;
    int maxfd = (client_fd > server_fd) ? client_fd : server_fd;

    char client_buf[65536];
    char server_buf[65536];

    bool client_alive = true;
    bool server_alive = true;

    int idle_cycles = 0;

    // Accumulator for buffering the server's response headers before forwarding.
    // We need to see the full header block (\r\n\r\n) so we can splice in the
    // X-Proxy header without splitting an existing header across two writes.
    char header_acc[32768];
    int header_acc_len = 0;
    bool header_injected = false;
    const char *ENDSEQ = "\r\n\r\n";

    while (client_alive && server_alive) {
        FD_ZERO(&readfds);
        if (client_alive) FD_SET(client_fd, &readfds);
        if (server_alive) FD_SET(server_fd, &readfds);

        struct timeval tv = {5, 0};

        int activity = select(maxfd + 1, &readfds, NULL, NULL, &tv);
        if (activity < 0) break;

        if (activity == 0) {
            // No traffic for 5 s. Increment the idle counter and bail after
            // 2 minutes (24 × 5 s) to reclaim the thread for abandoned sockets.
            idle_cycles++;
            if (idle_cycles > 24) break;
            if (SSL_pending(client_ssl) == 0 && SSL_pending(server_ssl) == 0)
                continue;
        }

        idle_cycles = 0;

        // --- CLIENT → SERVER ---
        if (client_alive && (FD_ISSET(client_fd, &readfds) || SSL_pending(client_ssl) > 0)) {
            int n = SSL_read(client_ssl, client_buf, sizeof(client_buf));

            if (n <= 0) {
                int err = SSL_get_error(client_ssl, n);
                // WANT_READ / WANT_WRITE are non-fatal: OpenSSL needs more data
                // or buffer space for the TLS record. Any other error is real.
                if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
                    client_alive = false;
                continue;
            }

            // HTTP/1.1 keep-alive connections reuse the same TCP+TLS session
            // for multiple requests. Reset the injection state at the start of
            // each new request so we inject the header into every response.
            if (n >= 4 && (!memcmp(client_buf, "GET ", 4) ||
                           !memcmp(client_buf, "POST", 4) ||
                           !memcmp(client_buf, "HEAD", 4))) {
                header_injected = false;
                header_acc_len = 0;
            }

            // Forward the request to the server. SSL_write may not write all
            // bytes in one call (e.g. if the send buffer is full), so loop
            // until everything is sent or the connection is lost.
            int sent = 0;
            while (sent < n) {
                int w = SSL_write(server_ssl, client_buf + sent, n - sent);
                if (w > 0) sent += w;
                else {
                    int err = SSL_get_error(server_ssl, w);
                    if (err == SSL_ERROR_WANT_WRITE) continue;
                    server_alive = false;
                    break;
                }
            }
        }

        // --- SERVER → CLIENT ---
        if (server_alive && (FD_ISSET(server_fd, &readfds) || SSL_pending(server_ssl) > 0)) {
            int n = SSL_read(server_ssl, server_buf, sizeof(server_buf));

            if (n <= 0) {
                int err = SSL_get_error(server_ssl, n);
                if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE)
                    server_alive = false;
                continue;
            }

            // Buffer server data until we see the end of the response headers,
            // then inject X-Proxy before forwarding to the client.
            if (!header_injected) {

                if (header_acc_len + n < (int)sizeof(header_acc)) {
                    memcpy(header_acc + header_acc_len, server_buf, n);
                    header_acc_len += n;

                    char *p = NULL;
                    if (header_acc_len >= 4)
                        p = strstr(header_acc, ENDSEQ);

                    if (!p)
                        continue; // Headers still incomplete — keep buffering.

                    int header_len = (p - header_acc) + 4; // includes \r\n\r\n

                    // Rebuild the header block with our extra field inserted
                    // just before the final \r\n\r\n terminator.
                    char out[65536];
                    int out_len = 0;

                    memcpy(out, header_acc, header_len - 2);
                    out_len = header_len - 2;

                    out_len += snprintf(out + out_len,
                                        sizeof(out) - out_len,
                                        "X-Proxy:CS112\r\n\r\n");

                    SSL_write(client_ssl, out, out_len);

                    // Any bytes that arrived after the header terminator in the
                    // same read() are body data — send them immediately.
                    int body_len = header_acc_len - header_len;
                    if (body_len > 0) {
                        SSL_write(client_ssl, header_acc + header_len, body_len);
                    }

                    header_injected = true;
                    header_acc_len = 0;
                    continue;
                } else {
                    // Accumulator full before we saw \r\n\r\n — the response
                    // headers are unusually large. Skip injection rather than
                    // risk corrupting the stream; just relay bytes as-is.
                    header_injected = true;
                }
            }

            // Headers already injected — relay body bytes directly.
            int sent = 0;
            while (sent < n) {
                int w = SSL_write(client_ssl, server_buf + sent, n - sent);
                if (w > 0) sent += w;
                else {
                    int err = SSL_get_error(client_ssl, w);
                    if (err == SSL_ERROR_WANT_WRITE) continue;
                    client_alive = false;
                    break;
                }
            }
        }
    }
}

// Dynamically generate a self-signed leaf certificate for the given hostname,
// signed by the proxy's CA key, and return it wrapped in an SSL_CTX ready for
// SSL_accept(). Called once per CONNECT request; there is no cert cache.
//
// Why per-host rather than a single wildcard cert? Browsers validate the SAN
// against the exact hostname in the CONNECT request. A wildcard only covers one
// level of subdomain and is rejected for IP literals and bare domains. Generating
// on-the-fly is the only portable approach.
SSL_CTX *create_fake_cert_context(const char *hostname)
{
    if (!g_ca_cert || !g_ca_key) return NULL;

    X509 *cert = X509_new();
    if (!cert) return NULL;

    // Generate a fresh 2048-bit RSA key for this certificate. RSA_F4 (65537) is
    // the standard public exponent — small enough to be fast, large enough to
    // resist small-exponent attacks.
    EVP_PKEY *pkey = EVP_PKEY_new();
    RSA *rsa = RSA_new();
    BIGNUM *bn = BN_new();

    BN_set_word(bn, RSA_F4);
    RSA_generate_key_ex(rsa, 2048, bn, NULL);
    EVP_PKEY_assign_RSA(pkey, rsa);
    BN_free(bn);

    X509_set_version(cert, 2);

    // Serial numbers must be unique across all certs signed by the same CA.
    // Combine timestamp, a monotonic counter, and PID so that concurrent
    // threads generating certs for different hosts don't collide.
    pthread_mutex_lock(&serial_mutex);
    unsigned long counter = cert_serial_counter++;
    pthread_mutex_unlock(&serial_mutex);

    unsigned long long unique_serial =
        ((unsigned long long)time(NULL) << 32) |
        ((counter & 0xFFFF) << 16) |
        (getpid() & 0xFFFF);

    // Use lower 63 bits — ASN1_INTEGER_set takes a signed long, so the MSB
    // must be 0 to avoid negative values being rejected by strict parsers.
    ASN1_INTEGER_set(X509_get_serialNumber(cert), (long)(unique_serial & 0x7FFFFFFFFFFFFFFF));

    // Back-date notBefore by 24 h to tolerate clock skew between the proxy
    // host and the client machine. Without this, a client whose clock is
    // slightly ahead would see "certificate not yet valid" and reject the cert.
    X509_gmtime_adj(X509_get_notBefore(cert), -86400);  // -24 hours
    X509_gmtime_adj(X509_get_notAfter(cert), 31536000L); // +1 year from now

    X509_set_pubkey(cert, pkey);

    X509_NAME *name = X509_get_subject_name(cert);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                (unsigned char *)hostname, -1, -1, 0);
    X509_set_issuer_name(cert, X509_get_subject_name(g_ca_cert));

    // Modern browsers (Chrome 58+, Firefox) no longer accept the CN field as a
    // hostname match — they require a Subject Alternative Name (SAN) extension.
    // Without it the browser will show an error even if CN matches perfectly.
    char san_string[512];
    snprintf(san_string, sizeof(san_string), "DNS:%s", hostname);

    X509V3_CTX x509_ctx;
    X509V3_set_ctx_nodb(&x509_ctx);
    X509V3_set_ctx(&x509_ctx, g_ca_cert, cert, NULL, NULL, 0);

    X509_EXTENSION *ext = X509V3_EXT_conf_nid(NULL, &x509_ctx,
                                               NID_subject_alt_name, san_string);
    if (ext) {
        X509_add_ext(cert, ext, -1);
        X509_EXTENSION_free(ext);
    } else {
        // SAN generation failed (e.g. malformed hostname). The cert will still
        // work for older clients that fall back to CN, but modern browsers will
        // reject it. Log the error so it's visible during debugging.
        ERR_print_errors_fp(stderr);
    }

    // Sign the certificate with the CA key
    if (!X509_sign(cert, g_ca_key, EVP_sha256())) {
        ERR_print_errors_fp(stderr);
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return NULL;
    }

    // Create SSL_CTX
    SSL_CTX *ssl_ctx = SSL_CTX_new(TLS_server_method());
    if (!ssl_ctx) {
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return NULL;
    }

    if (!SSL_CTX_use_certificate(ssl_ctx, cert)) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ssl_ctx);
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return NULL;
    }

    if (!SSL_CTX_use_PrivateKey(ssl_ctx, pkey)) {
        ERR_print_errors_fp(stderr);
        SSL_CTX_free(ssl_ctx);
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return NULL;
    }

    if (!SSL_CTX_check_private_key(ssl_ctx)) {
        SSL_CTX_free(ssl_ctx);
        X509_free(cert);
        EVP_PKEY_free(pkey);
        return NULL;
    }

    return ssl_ctx;
}

