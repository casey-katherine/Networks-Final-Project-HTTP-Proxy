#ifndef LLM_H
#define LLM_H

#include <gumbo.h>


char *send_to_llm(const char *content, const char *style);
char *extract_http_body(const char *http_data, int data_len, int *body_len);
char *process_webpage_with_llm(const char *resp_data, int total, const char *url);
char *inject_style_selector(const char *html_body, bool pirate_ready, bool simple_ready,
                           bool genz_ready, bool tldr_ready);
char *extract_style_from_url(const char *url);
const char *get_style_prompt(const char *style);
char *extract_json_result(const char *json_text);
char *strip_markdown_blocks(const char *text);

#endif