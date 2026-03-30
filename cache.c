#include "cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include "llm.h"

#define NS 1000000000LL
#define DEFAULT_MAXAGE (60 * 60)

extern Cache c;

// Add a mutex for cache safety
pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

// Thread data for parallel conversion
typedef struct {
    char *html_body;
    size_t html_size;
    const char *style;
    char **result;
    size_t *result_size;
} ConversionTask;

static CacheEntry *find(Cache *c, const char *url)
{
    CacheEntry *entry;
    LIST_FOREACH(entry, &c->list, entries)
        if (strcmp(entry->name, url) == 0) {
            return entry;
        }

    return NULL;
}

int64_t get_time_ns()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((int64_t)ts.tv_sec * 1000000000LL) + ts.tv_nsec;
}

static void evict(Cache *c)
{
    if (c->size < c->capacity) return;

    CacheEntry *entry, *victim = NULL;
    uint64_t now = get_time_ns();

    // 1st: Any stale item
    LIST_FOREACH(entry, &c->list, entries) {
        if ((now - entry->inserted_at) > entry->max_age) {
            victim = entry;
            break;
        }
    }

    // 2. Find LRU
    if (!victim) {
        victim = LIST_FIRST(&c->list);
        LIST_FOREACH(entry, &c->list, entries) {
            if (entry->used_at < victim->used_at) {
                victim = entry;
            }
        }
    }

    if (!victim) return;

    // remove from list and free
    LIST_REMOVE(victim, entries);
    free(victim->content);
    free(victim);
    c->size--;
}

void cache_init(Cache *c, int capacity)
{
    LIST_INIT(&c->list);
    c->size = 0;
    c->capacity = capacity;
}

void cache_put(Cache *c, const char *url, const char *response,
                size_t size, int64_t max_age, const char *html, size_t html_size)
{
    CacheEntry *entry = find(c, url);

    if (entry) {
        free(entry->content);
        if (entry->html_body) free(entry->html_body);
    } else {
        if (c->size >= c->capacity) evict(c);
        entry = calloc(1, sizeof(*entry));  // Use calloc to zero everything
        strcpy(entry->name, url);
        LIST_INSERT_HEAD(&c->list, entry, entries);
        c->size++;
    }

    entry->content = malloc(size);
    memcpy(entry->content, response, size);
    entry->size = size;
    entry->max_age = max_age;
    entry->inserted_at = get_time_ns();
    entry->used_at = entry->inserted_at;

    if (html && html_size > 0) {
        entry->html_body = malloc(html_size + 1);
        memcpy(entry->html_body, html, html_size);
        entry->html_body[html_size] = '\0';
        entry->html_size = html_size;
    }
}


CacheEntry *cache_get(Cache *c, const char *url)
{
    CacheEntry *entry = find(c, url);
    if (entry == NULL) return NULL;

    int64_t now = get_time_ns();
    if ((now - entry->inserted_at) >= entry->max_age) {
        return NULL;
    }

    entry->used_at = now;
    return entry;
}

void cache_cleanup(Cache *c)
{
    CacheEntry *entry;
    while ((entry = LIST_FIRST(&c->list)) != NULL) {
        LIST_REMOVE(entry, entries);
        free(entry->content);
        // Free all style variants
        if (entry->html_body) free(entry->html_body);
        if (entry->pirate_body) free(entry->pirate_body);
        if (entry->simple_body) free(entry->simple_body);
        if (entry->genz_body) free(entry->genz_body);
        if (entry->TLDR_body) free(entry->TLDR_body);
        free(entry);
    }
    c->size = 0;
}

void add_age(CacheEntry *entry)
{
    if (!entry || !entry->content) return;

    int64_t age = (get_time_ns() - entry->inserted_at) / NS;

    const char *og_content = entry->content;
    char *header_end = strstr(og_content, "\r\n\r\n");

    int header_len = (header_end - og_content); // not including \r\n\r\n
    int body_len = entry->size - (header_len + 4); // (\r\n\r\n)

    // Allocate enough space for headers + Age line + body
    char *new_response = malloc(entry->size + 32);

    // Copy headers and add Age header
    memcpy(new_response, og_content, header_len);
    int current_len = header_len;
    current_len += snprintf(new_response + current_len, 32,
                           "\r\nAge: %lld\r\n\r\n", (long long)age);

    // Copy the body (after the \r\n\r\n)
    memcpy(new_response + current_len, header_end + 4, body_len);
    int final_size = current_len + body_len;

    entry->content_age = new_response;
    entry->size_age = final_size;
}

int64_t find_MA(const char *response)
{
    const char *curr = strstr(response, "max-age=");
    if (!curr) {
        return DEFAULT_MAXAGE;
    }

    int age;
    if (sscanf(curr + 8, "%d", &age) != 1) {
        return DEFAULT_MAXAGE;
    }

    return age;
}

// Worker function for each style conversion
void *convert_style_thread(void *arg)
{
    ConversionTask *task = (ConversionTask *)arg;

    if (!task->html_body || task->html_size == 0) {
        free(task->html_body);
        free(arg);
        return NULL;
    }

    // Create a fake HTTP response with the HTML body
    char *fake_response = malloc(task->html_size + 1024);
    int fake_size = snprintf(fake_response, task->html_size + 1024,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/html\r\n"
        "\r\n%s",
        task->html_body);

    // Build URL with style parameter
    char url_with_style[256];
    snprintf(url_with_style, sizeof(url_with_style), "?proxy_style=%s", task->style);

    // Convert the HTML
    char *converted = process_webpage_with_llm(fake_response, fake_size, url_with_style);
    free(fake_response);
    free(task->html_body);  // Free our copy

    if (converted) {
        *task->result = converted;
        *task->result_size = strlen(converted);
    } else {
        printf("[THREAD] %s failed\n", task->style);
        *task->result = NULL;
        *task->result_size = 0;
    }

    free(arg);
    return NULL;
}

// Thread-safe storage of results back to cache
void store_style_results(const char *cache_key, char **results, size_t *sizes)
{
    pthread_mutex_lock(&cache_mutex);

    CacheEntry *entry = cache_get(&c, cache_key);
    if (entry) {
        // Only update if not already set (avoid double-free)
        if (!entry->pirate_body && results[0]) {
            entry->pirate_body = results[0];
            entry->pirate_size = sizes[0];
        } else if (results[0]) {
            free(results[0]);  // Already set, free duplicate
        }

        if (!entry->simple_body && results[1]) {
            entry->simple_body = results[1];
            entry->simple_size = sizes[1];
        } else if (results[1]) {
            free(results[1]);
        }

        if (!entry->genz_body && results[2]) {
            entry->genz_body = results[2];
            entry->genz_size = sizes[2];
        } else if (results[2]) {
            free(results[2]);
        }

        if (!entry->TLDR_body && results[3]) {
            entry->TLDR_body = results[3];
            entry->TLDR_size = sizes[3];
        } else if (results[3]) {
            free(results[3]);
        }

    } else {
        for (int i = 0; i < 4; i++) {
            if (results[i]) free(results[i]);
        }
    }

    pthread_mutex_unlock(&cache_mutex);
}

// Preload 4 styles in parallel
void *cache_preload_all_styles(void *arg)
{
    char *cache_key = (char *)arg;

    // Get HTML body with mutex protection and make a copy
    pthread_mutex_lock(&cache_mutex);
    CacheEntry *entry = cache_get(&c, cache_key);
    if (!entry || !entry->html_body) {
        pthread_mutex_unlock(&cache_mutex);
        free(cache_key);
        return NULL;
    }

    // Make copies of HTML for each thread
    char *html_copies[4];
    size_t html_size = entry->html_size;
    for (int i = 0; i < 4; i++) {
        html_copies[i] = malloc(html_size + 1);
        memcpy(html_copies[i], entry->html_body, html_size);
        html_copies[i][html_size] = '\0';
    }
    pthread_mutex_unlock(&cache_mutex);

    // Define the 4 styles to preload
    const char *styles[] = {"pirate", "simplified", "GenZ", "tldr"};

    // Storage for results
    char *results[4] = {NULL};
    size_t result_sizes[4] = {0};

    // Create thread tasks
    pthread_t threads[4];

    for (int i = 0; i < 4; i++) {
        ConversionTask *task = malloc(sizeof(ConversionTask));
        task->html_body = html_copies[i];
        task->html_size = html_size;
        task->style = styles[i];
        task->result = &results[i];
        task->result_size = &result_sizes[i];

        if (pthread_create(&threads[i], NULL, convert_style_thread, task) != 0) {
            printf("[ERROR] Failed to create thread for %s\n", styles[i]);
            free(html_copies[i]);
            free(task);
        }
    }

    // Wait for all threads to complete
    for (int i = 0; i < 4; i++) {
        pthread_join(threads[i], NULL);
    }

    // Store results back to cache (thread-safe)
    store_style_results(cache_key, results, result_sizes);

    free(cache_key);
    return NULL;
}
