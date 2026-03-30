# LLM-Powered HTTP Proxy

A forward proxy written in C that intercepts HTTP traffic, rewrites page content using an LLM, and injects a style-selector widget so users can switch between translation modes in real time. HTTPS connections are tunneled via MITM; style transformation is not yet implemented for HTTPS.

---

## Table of Contents

- [How it works](#how-it-works)
- [Architecture](#architecture)
- [Dependencies](#dependencies)
- [Build](#build)
- [Run](#run)
- [Limitations](#limitations)

---

## How it works

When a browser makes an HTTP request through the proxy:

1. The proxy fetches the page from the origin server.
2. If the response is HTML, it immediately serves the original page with a **style selector widget** injected into the right sidebar — all styles initially marked as "(loading...)".
3. In the background, four worker threads call the LLM API in parallel, each generating a transformed version of the page: **Pirate Speak**, **Simplified**, **Gen Z**, and **TL;DR**.
4. A JavaScript snippet on the page polls the proxy every 3 seconds via HEAD requests. Once a style is ready, the dropdown enables it; selecting one reloads the page with that style applied.
5. Transformed pages are cached in memory (LRU, 100 entries). Repeat visits are served from cache in under a second.

For HTTPS, the proxy performs a full MITM TLS handshake — it generates a fake certificate for each hostname on the fly, signed by a local CA — but currently relays the decrypted content unchanged.

---

## Architecture

`main.c` runs a `select()`-based event loop that handles all HTTP connections in a single thread, buffering partial requests per file descriptor until a complete `\r\n\r\n` is received. It routes by method: CONNECT goes to `SSL.c`, GET and HEAD go to `handle_http_get()`.

`http_proxy.c` handles the TCP mechanics — connecting to origin servers, forwarding requests, and reading responses. Non-HTML responses are passed through with an `X-Proxy` header added.

`llm.c` extracts the HTML body, calls the LLM API with a style-specific prompt, and injects the style selector widget into the response before it's sent to the browser.

`cache.c` maintains an in-memory LRU cache of up to 100 entries. Each entry holds the original HTML plus up to four transformed variants. When a page is first fetched, a coordinator thread spawns four worker threads in parallel — one per style — that call the LLM API and write results back into the cache entry as they complete. All cache access is protected by a single mutex.

`SSL.c` handles HTTPS. On a CONNECT request it opens a TCP connection to the real server, sends the browser `200 Connection Established`, generates a per-host fake certificate signed by the proxy CA, completes TLS handshakes on both sides, then detaches a relay thread that bidirectionally forwards encrypted data. The relay injects an `X-Proxy` header into each response's header block. Connections time out after 2 minutes of inactivity.

---

## Dependencies

The proxy requires OpenSSL (`libssl-dev`), Gumbo for HTML parsing (`libgumbo-dev`), libcurl (`libcurl4-openssl-dev`), and pthreads — all installable from the system package manager. Jansson is built from source in `jansson/build/` rather than using the system package (see Build step 2). The `LLMProxy/c/` client library is vendored in the repo.

> **Platform note:** The Makefile links `-lnsl`, which is Linux-only. These instructions target **Debian/Ubuntu**. macOS is not supported without build changes.

---

## Build

### 1. Install system dependencies

```sh
sudo apt-get install build-essential libssl-dev libgumbo-dev libcurl4-openssl-dev
```

### 2. Build Jansson from source

The Makefile links against `jansson/build/lib/libjansson.a`. The system `libjansson-dev` package is **not** used.

```sh
cd jansson && mkdir -p build && cd build
cmake .. && make
cd ../..
```

### 3. Configure LLM credentials

Create a `.env` file in the repo root:

```
LLMPROXY_API_KEY="your-api-key"
LLMPROXY_ENDPOINT="your-endpoint-url"
```

The proxy reads this file at startup via the LLMProxy client library. Without it, style transformation will silently fail and the proxy will serve original HTML only.

### 4. Generate a CA certificate

The proxy needs a CA cert and key to sign per-host fake certificates for HTTPS interception. If you don't have one:

```sh
openssl genrsa -out ca.key 4096
openssl req -new -x509 -days 3650 -key ca.key -out ca.crt -subj "/CN=Proxy CA"
```

### 5. Compile

```sh
make
```

---

## Run

```sh
./proxy <port> <ca_cert_path> <ca_key_path>
```

Example:

```sh
./proxy 8080 ca.crt ca.key
```

Configure your browser to use `localhost:8080` as its HTTP **and** HTTPS proxy.

**To enable HTTPS interception without browser warnings**, import `ca.crt` as a trusted root certificate authority in your browser or OS certificate store. In Firefox: *Settings → Privacy & Security → Certificates → View Certificates → Authorities → Import*. In Chrome/Chromium: add to the OS trust store.

### Using the style selector

Navigate to any **HTTP** page. A **STYLE** tab appears on the right edge of the viewport. Hover or click to reveal the dropdown. Styles that are still being generated are labelled "(loading...)" and are disabled; the page auto-refreshes every 3 seconds until all four are ready.

**HTTPS pages** load normally but the style selector is not injected — transformation for HTTPS is not yet implemented.

---

## Limitations

- **HTTPS content transformation is not implemented.** HTTPS pages are decrypted and re-encrypted by the proxy but are not passed through the LLM or injected with the style selector.
- **In-memory cache only.** All cached pages and transformed variants are lost when the proxy stops.
- **Full-response buffering.** The entire response is buffered before being forwarded to the browser. Large pages will feel slow on first load.
- **No connection pooling.** A fresh TCP connection is opened to the origin server for every request.
- **Dynamic / JS-heavy pages** may not transform cleanly because the LLM receives raw HTML, not the rendered DOM.
