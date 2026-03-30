#ifndef CACHE_H
#define CACHE_H

#include <stdint.h>
#include <time.h>
#include <sys/queue.h>
#include <pthread.h>

typedef struct CacheEntry {
    char name[100];
    char *content;
    char *html_body;
    size_t html_size;
    char *content_age;
    size_t size;
    size_t size_age;
    int64_t max_age;
    int64_t inserted_at;
    int64_t used_at;
    LIST_ENTRY(CacheEntry) entries;

    // Page Styles
    char *simple_body;
    size_t simple_size;
    char *TLDR_body;
    size_t TLDR_size;
    char *pirate_body;
    size_t pirate_size;
    char *genz_body;
    size_t genz_size;
} CacheEntry;

LIST_HEAD(CacheList, CacheEntry);

typedef struct {
    struct CacheList list;
    int size;
    int capacity;
} Cache;


void cache_init(Cache *c, int capacity);
void cache_put(Cache *c, const char *url, const char *response,
                size_t size, int64_t max_age, const char *html, size_t html_size);
CacheEntry *cache_get(Cache *c, const char *url);
void cache_cleanup(Cache *c);
int64_t get_time_ns();
void add_age(CacheEntry *entry);
int64_t find_MA(const char *response);
void *cache_preload_all_styles(void *arg);

#endif
