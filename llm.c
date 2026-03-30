#define _GNU_SOURCE
#include "llm.h"
#include "LLMProxy/c/include/llmproxy.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <jansson.h>
#include <gumbo.h>

#define MAX_FILE_SIZE (10 * 1024 * 1024) // 10 MB

// Get system prompt based on style
const char *get_style_prompt(const char *style)
{
    if (strcmp(style, "pirate") == 0) {
        return "Convert this webpage content to Pirate Speak. Use pirate dialect, "
               "say 'arr', 'matey', 'ye', 'aye', and use nautical terms. Be enthusiastic!";
    } else if (strcmp(style, "simplified") == 0) {
        return "Convert this webpage content to simpler language. Use simple language, "
               "so a second grader could read it.";
    } else if (strcmp(style, "GenZ") == 0) {
        return "Convert this webpage content to brain rot language. Use teenage language, "
               "like unc, six-seven, and rizz. You must include six-seven somewhere.";
    } else if (strcmp(style, "tldr") == 0) {
        return "Convert this webpage content into a concise TL;DR summary. "
            "Summarize the key points in 3-5 short sentences using clear, "
            "plain language. Do not include unnecessary details.";
    }
    return NULL; // No conversion
}


// Extract "result" field from JSON response
char *extract_json_result(const char *json_text)
{
    json_error_t err;
    json_t *root = json_loads(json_text, 0, &err);
    if (!root) {
        printf("[JSON ERROR] %s at line %d\n", err.text, err.line);
        return NULL;
    }

    json_t *result = json_object_get(root, "result");
    if (!json_is_string(result)) {
        json_decref(root);
        return NULL;
    }

    const char *utf8 = json_string_value(result);
    char *out = strdup(utf8);

    json_decref(root);
    return out;
}

// Strip markdown code blocks from LLM response
char *strip_markdown_blocks(const char *text)
{
    if (!text) return NULL;

    // Look for ```html or ``` at start
    const char *start = text;
    if (strncmp(text, "```html", 7) == 0) {
        start = strchr(text, '\n');
        if (start) start++; // Skip the newline
    } else if (strncmp(text, "```", 3) == 0) {
        start = strchr(text, '\n');
        if (start) start++; // Skip the newline
    }

    if (!start) start = text;

    // Find ending ```
    const char *end = strstr(start, "```");

    int len;
    if (end) {
        len = end - start;
    } else {
        len = strlen(start);
    }

    // Copy the content
    char *result = malloc(len + 1);
    if (result) {
        memcpy(result, start, len);
        result[len] = '\0';

        // Trim trailing whitespace
        while (len > 0 && isspace(result[len - 1])) {
            result[--len] = '\0';
        }
    }

    return result;
}

// Load config from .env in the current directory
char *send_to_llm(const char *content, const char *style)
{
    const char *prompt = get_style_prompt(style);
    if (!prompt) {
        return NULL; // No conversion needed
    }

    ClientConfig cfg = llmproxy_load_config();

    // Call llmproxy_generate - this returns JSON!
    char *json_resp = llmproxy_generate(
        &cfg,
        "4o-mini",          // model
        prompt,             // system prompt based on style
        content,            // user query
        NULL,  // temperature (NULL = default)
        NULL,  // lastk (NULL = default)
        NULL,  // session ID (NULL -> "GenericSession")
        NULL,  // rag_threshold
        NULL,  // rag_usage
        NULL   // rag_k
    );

    if (!json_resp) return NULL;

    // Extract JSON result field
    char *json_result = extract_json_result(json_resp);
    free(json_resp);

    if (!json_result) {
        printf("[PROXY] Failed to extract JSON result\n");
        return NULL;
    }

    // Strip markdown code blocks if present
    char *cleaned = strip_markdown_blocks(json_result);
    free(json_result);

    return cleaned;
}

// Extract body from HTTP response
char *extract_http_body(const char *http_data, int data_len, int *body_len)
{
    char *body_start = strstr(http_data, "\r\n\r\n");
    if (!body_start) {
        *body_len = 0;
        return NULL;
    }

    body_start += 4;
    *body_len = data_len - (body_start - http_data);

    if (*body_len <= 0) {
        return NULL;
    }

    char *body = malloc(*body_len + 1);
    if (body) {
        memcpy(body, body_start, *body_len);
        body[*body_len] = '\0';
    }

    return body;
}

// Inject style selector UI into HTML - polls every 3 secs
char *inject_style_selector(const char *html_body, bool pirate_ready, bool simple_ready,
                           bool genz_ready, bool tldr_ready)
{
    if (!html_body) return NULL;

    // Remove any existing selector first to avoid duplicates
    char *clean_html = NULL;
    const char *existing_selector = strstr(html_body, "<div id='proxy-style-container'");

    if (existing_selector) {
        const char *selector_end = strstr(existing_selector, "</script>");
        if (selector_end) {
            selector_end += 9;
            size_t prefix_len = existing_selector - html_body;
            size_t suffix_len = strlen(selector_end);
            clean_html = malloc(prefix_len + suffix_len + 1);
            memcpy(clean_html, html_body, prefix_len);
            strcpy(clean_html + prefix_len, selector_end);
        }
    }

    const char *html_to_use = clean_html ? clean_html : html_body;

    // Create a status endpoint in the page itself
    char style_ui[10240];
    snprintf(style_ui, sizeof(style_ui),
        "<div id='proxy-style-container' style='"
        "position: fixed; top: 50%%; right: 0; transform: translateY(-50%%); "
        "z-index: 999999; transition: transform 0.3s ease;'>"

        "<div id='proxy-style-tab' style='"
        "background: linear-gradient(135deg, #667eea 0%%, #764ba2 100%%); "
        "color: white; padding: 10px 6px; border-radius: 10px 0 0 10px; "
        "cursor: pointer; writing-mode: vertical-rl; text-orientation: mixed; "
        "font-size: 12px; font-weight: 600; letter-spacing: 1px; "
        "box-shadow: 0 4px 16px rgba(0,0,0,0.25);'>STYLE</div>"

        "<div id='proxy-style-panel' style='"
        "position: absolute; right: 0; top: 0; transform: translateX(100%%); "
        "background: linear-gradient(135deg, #667eea 0%%, #764ba2 100%%); "
        "padding: 12px; border-radius: 10px 0 0 10px; min-width: 180px; "
        "box-shadow: 0 8px 32px rgba(0,0,0,0.3); "
        "font-family: -apple-system, BlinkMacSystemFont, \"Segoe UI\", Roboto, sans-serif; "
        "transition: transform 0.3s ease;'>"

        "<div style='font-weight: 600; margin-bottom: 8px; font-size: 12px; "
        "text-transform: uppercase; letter-spacing: 1px; color: white;'>Translation</div>"

        "<select id='proxy-style-dropdown' style='"
        "width: 100%%; padding: 8px; border-radius: 6px; border: none; "
        "font-size: 13px; cursor: pointer; background: white; color: #333; font-weight: 500;' "
        "onchange='if(!this.options[this.selectedIndex].disabled) window.location.search = \"proxy_style=\" + this.value'>"
        "<option value='none'>Normal</option>"
        "<option value='pirate' %s>🏴‍☠️ Pirate%s</option>"
        "<option value='simplified' %s>✏️ Simplified%s</option>"
        "<option value='GenZ' %s>🧠💀 Gen Z%s</option>"
        "<option value='tldr' %s>⚡ TL;DR%s</option>"
        "</select>"
        "</div></div>"

        "<!-- Embedded style readiness data -->"
        "<script id='proxy-style-data' type='application/json'>"
        "{"
        "  \"pirate\": %s,"
        "  \"simplified\": %s,"
        "  \"GenZ\": %s,"
        "  \"tldr\": %s"
        "}"
        "</script>"

        "<script>"
        "(function(){"
        "  console.log('[PROXY] Style selector initialized');"
        "  "
        "  const container=document.getElementById('proxy-style-container');"
        "  const panel=document.getElementById('proxy-style-panel');"
        "  const tab=document.getElementById('proxy-style-tab');"
        "  const dd=document.getElementById('proxy-style-dropdown');"
        "  "
        "  container.onmouseenter=()=>panel.style.transform='translateX(0)';"
        "  container.onmouseleave=()=>panel.style.transform='translateX(100%%)';"
        "  tab.onclick=()=>{panel.style.transform=(panel.style.transform==='translateX(0)'?'translateX(100%%)':'translateX(0)');};"
        "  "
        "  const params=new URLSearchParams(window.location.search);"
        "  const currentStyle=params.get('proxy_style')||'none';"
        "  dd.value=currentStyle;"
        "  "
        "  function updateOptionStyles(){"
        "    for(let i=0; i<dd.options.length; i++){"
        "      const opt=dd.options[i];"
        "      if(opt.disabled){"
        "        opt.style.color='#999';"
        "        opt.style.fontStyle='italic';"
        "        opt.style.cursor='not-allowed';"
        "      } else if(opt.value !== 'none'){"
        "        opt.style.color='#333';"
        "        opt.style.fontStyle='normal';"
        "        opt.style.cursor='pointer';"
        "      }"
        "    }"
        "  }"
        "  updateOptionStyles();"
        "  "
        "  let checkCount=0;"
        "  const maxChecks=60;"
        "  "
        "  function pollByReloading(){"
        "    if(checkCount>=maxChecks){"
        "      console.log('[POLL] Max checks reached');"
        "      return;"
        "    }"
        "    checkCount++;"
        "    console.log('[POLL] Check', checkCount, '- reloading page to check status');"
        "    "
        "    setTimeout(()=>{"
        "      window.location.reload();"
        "    }, 3000);"
        "  }"
        "  "
        "  let hasDisabled=Array.from(dd.options).some(o=>o.disabled);"
        "  if(hasDisabled && currentStyle==='none'){"
        "    console.log('[POLL] Styles still loading, will auto-refresh in 3s');"
        "    pollByReloading();"
        "  } else {"
        "    console.log('[POLL] All styles ready or user selected a style');"
        "  }"
        "})();"
        "</script>",

        pirate_ready ? "" : "disabled",
        pirate_ready ? "" : " (loading...)",
        simple_ready ? "" : "disabled",
        simple_ready ? "" : " (loading...)",
        genz_ready ? "" : "disabled",
        genz_ready ? "" : " (loading...)",
        tldr_ready ? "" : "disabled",
        tldr_ready ? "" : " (loading...)",

        pirate_ready ? "true" : "false",
        simple_ready ? "true" : "false",
        genz_ready ? "true" : "false",
        tldr_ready ? "true" : "false"
    );

    // Inject before </body>
    char *body_end = strcasestr(html_to_use, "</body>");
    if (!body_end) {
        int new_len = strlen(html_to_use) + strlen(style_ui) + 1;
        char *new_html = malloc(new_len);
        if (!new_html) {
            if (clean_html) free(clean_html);
            return NULL;
        }
        snprintf(new_html, new_len, "%s%s", html_to_use, style_ui);
        if (clean_html) free(clean_html);
        return new_html;
    }

    int prefix_len = body_end - html_to_use;
    int new_len = strlen(html_to_use) + strlen(style_ui) + 1;
    char *new_html = malloc(new_len);
    if (!new_html) {
        if (clean_html) free(clean_html);
        return NULL;
    }
    memcpy(new_html, html_to_use, prefix_len);
    strcpy(new_html + prefix_len, style_ui);
    strcpy(new_html + prefix_len + strlen(style_ui), body_end);

    if (clean_html) free(clean_html);
    return new_html;
}


// Extract style parameter from URL query string
char *extract_style_from_url(const char *url)
{
    char *query = strchr(url, '?');
    if (!query) return NULL;

    char *style_param = strstr(query, "proxy_style=");
    if (!style_param) return NULL;

    style_param += 12; // Skip "proxy_style="

    // Extract until & or end of string
    char *style = malloc(32);
    int i = 0;
    while (style_param[i] && style_param[i] != '&' && style_param[i] != ' '
           && style_param[i] != '\r' && style_param[i] != '\n' && i < 31) {
        style[i] = style_param[i];
        i++;
    }
    style[i] = '\0';

    return style;
}

// Process webpage: send to LLM and return response with style selector
char *process_webpage_with_llm(const char *resp_data, int total, const char *url)
{
    int body_len = 0;
    char *body = extract_http_body(resp_data, total, &body_len);

    if (!body || body_len <= 0) {
        return NULL;
    }

    // Extract style from URL
    char *style = extract_style_from_url(url);
    if (!style) {
        style = strdup("none");
    }

    char *processed_body = NULL;

    // Only convert if style is not "none"
    if (strcmp(style, "none") != 0) {
        processed_body = send_to_llm(body, style);
    }

    // Use original if LLM failed or style is "none"
    if (!processed_body) {
        processed_body = strdup(body);
    }

    free(body);
    free(style);

    return processed_body;
}
