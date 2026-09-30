#define _POSIX_C_SOURCE 200809L
#include "cat2.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SERVER_NAME "cat2-mcp"
#define SERVER_VERSION CAT2_VERSION

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} Buf;

static void buf_init(Buf *b) {
    b->cap = 4096;
    b->len = 0;
    b->data = (char*)malloc(b->cap);
    if (b->data) b->data[0] = '\0';
}

static void buf_free(Buf *b) {
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static void buf_reserve(Buf *b, size_t extra) {
    while (b->len + extra + 1 > b->cap) {
        b->cap *= 2;
        b->data = (char*)realloc(b->data, b->cap);
    }
}

static void buf_append(Buf *b, const char *s) {
    size_t slen = strlen(s);
    buf_reserve(b, slen);
    memcpy(b->data + b->len, s, slen + 1);
    b->len += slen;
}

static void buf_append_escaped(Buf *b, const char *src) {
    static const char *hexd = "0123456789abcdef";
    for (; *src; src++) {
        unsigned char c = (unsigned char)*src;
        switch (c) {
            case '"':  buf_append(b, "\\\""); break;
            case '\\': buf_append(b, "\\\\"); break;
            case '\n': buf_append(b, "\\n"); break;
            case '\r': buf_append(b, "\\r"); break;
            case '\t': buf_append(b, "\\t"); break;
            default:
                if (c < 0x20) {
                    char u[7];
                    u[0] = '\\'; u[1] = 'u'; u[2] = '0'; u[3] = '0';
                    u[4] = hexd[(c >> 4) & 0xf]; u[5] = hexd[c & 0xf]; u[6] = '\0';
                    buf_append(b, u);
                } else {
                    buf_reserve(b, 1);
                    b->data[b->len++] = (char)c;
                    b->data[b->len] = '\0';
                }
        }
    }
}

static void send_line(const char *json) {
    fputs(json, stdout);
    fputc('\n', stdout);
    fflush(stdout);
}

static void send_error(const char *id, int code, const char *message) {
    Buf b; buf_init(&b);
    buf_append(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    buf_append(&b, id);
    buf_append(&b, ",\"error\":{\"code\":");
    char cb[32]; snprintf(cb, sizeof(cb), "%d", code);
    buf_append(&b, cb);
    buf_append(&b, ",\"message\":\"");
    buf_append_escaped(&b, message);
    buf_append(&b, "\"}}");
    send_line(b.data);
    buf_free(&b);
}

static void send_result(const char *id, const char *result_json) {
    Buf b; buf_init(&b);
    buf_append(&b, "{\"jsonrpc\":\"2.0\",\"id\":");
    buf_append(&b, id);
    buf_append(&b, ",\"result\":");
    buf_append(&b, result_json);
    buf_append(&b, "}");
    send_line(b.data);
    buf_free(&b);
}

static void tool_result(const char *id, const char *text, int is_error) {
    Buf r; buf_init(&r);
    buf_append(&r, "{\"content\":[{\"type\":\"text\",\"text\":\"");
    buf_append_escaped(&r, text);
    buf_append(&r, "\"}]");
    if (is_error) buf_append(&r, ",\"isError\":true");
    buf_append(&r, "}");
    send_result(id, r.data);
    buf_free(&r);
}

static int json_get_string(const char *json, const char *key, char *out, size_t outsz) {
    char needle[160];
    snprintf(needle, sizeof(needle), "\"%s\"", key);
    const char *p = strstr(json, needle);
    if (!p) return 0;
    p += strlen(needle);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ':')) p++;
    if (*p == 'n' && strncmp(p, "null", 4) == 0) return 0;
    if (*p != '"') return 0;
    p++;
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < outsz) {
        if (*p == '\\') {
            p++;
            switch (*p) {
                case '"': case '\\': case '/': out[i++] = *p; break;
                case 'n': out[i++] = '\n'; break;
                case 'r': out[i++] = '\r'; break;
                case 't': out[i++] = '\t'; break;
                default: out[i++] = *p; break;
            }
        } else {
            out[i++] = *p;
        }
        p++;
    }
    out[i] = '\0';
    return 1;
}

static void json_get_id(const char *json, char *out, size_t outsz) {
    const char *p = strstr(json, "\"id\"");
    if (!p) { snprintf(out, outsz, "null"); return; }
    p += 4;
    while (*p && (*p == ' ' || *p == '\t' || *p == ':')) p++;
    if (!*p) { snprintf(out, outsz, "null"); return; }
    size_t i = 0;
    if (*p == '"') {
        out[i++] = *p++;
        while (*p && i + 1 < outsz) {
            if (*p == '\\') {
                out[i++] = *p++;
                if (*p && i + 1 < outsz) out[i++] = *p++;
                continue;
            }
            out[i++] = *p;
            if (*p++ == '"') break;
        }
        out[i] = '\0';
        return;
    }
    while (*p && i + 1 < outsz) {
        if (*p == ',' || *p == '}' || *p == ']' || *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
            break;
        out[i++] = *p++;
    }
    out[i] = '\0';
    if (!i) snprintf(out, outsz, "null");
}

static const char *find_arguments(const char *req) {
    const char *params = strstr(req, "\"params\"");
    if (!params) return NULL;
    const char *a = strstr(params, "\"arguments\"");
    if (!a) return NULL;
    return strchr(a, '{');
}

static void handle_initialize(const char *id) {
    send_result(id,
        "{"
          "\"protocolVersion\":\"2024-11-05\","
          "\"capabilities\":{\"tools\":{}},"
          "\"serverInfo\":{\"name\":\"" SERVER_NAME "\","
                         "\"version\":\"" SERVER_VERSION "\"}"
        "}");
}

static void handle_tools_list(const char *id) {
    send_result(id,
        "{"
          "\"tools\":["
            "{"
              "\"name\":\"cat2_read\","
              "\"description\":\"Read and auto-decode a file (ASCII, IBM-1047 EBCDIC, UTF-8, double-converted) into clean readable text for AI analysis.\","
              "\"inputSchema\":{"
                "\"type\":\"object\","
                "\"properties\":{"
                  "\"path\":{\"type\":\"string\",\"description\":\"File path to read and decode.\"},"
                  "\"max_bytes\":{\"type\":\"number\",\"description\":\"Maximum bytes to read (default: 65536).\"}"
                "},"
                "\"required\":[\"path\"]"
              "}"
            "},"
            "{"
              "\"name\":\"cat2_detect\","
              "\"description\":\"Analyze a file or snippet to detect codepage distributions (ASCII, EBCDIC-1047, UTF-8, double-converted, binary).\" ,"
              "\"inputSchema\":{"
                "\"type\":\"object\","
                "\"properties\":{"
                  "\"path\":{\"type\":\"string\",\"description\":\"File path to analyze.\"}"
                "},"
                "\"required\":[\"path\"]"
              "}"
            "},"
            "{"
              "\"name\":\"cat2_convert\","
              "\"description\":\"Convert a text string directly using cat2's auto-detection or explicit EBCDIC/ASCII translation.\","
              "\"inputSchema\":{"
                "\"type\":\"object\","
                "\"properties\":{"
                  "\"text\":{\"type\":\"string\",\"description\":\"Text or hex byte sequence to decode.\"},"
                  "\"target\":{\"type\":\"string\",\"description\":\"Target encoding: 'ascii' (default) or 'ebcdic'.\"}"
                "},"
                "\"required\":[\"text\"]"
              "}"
            "},"
            "{"
              "\"name\":\"cat2_help\","
              "\"description\":\"Return cat2 usage summary and codepage details.\","
              "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
            "}"
          "]"
        "}");
}

static void handle_cat2_read(const char *id, const char *args) {
    char path[4096] = {0};
    json_get_string(args, "path", path, sizeof(path));
    if (!path[0]) {
        send_error(id, -32602, "cat2_read requires path parameter");
        return;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        char err[4300];
        snprintf(err, sizeof(err), "Cannot open '%s': %s", path, strerror(errno));
        tool_result(id, err, 1);
        return;
    }

    size_t max_read = 65536;
    unsigned char *raw = (unsigned char*)malloc(max_read + 1);
    if (!raw) {
        fclose(f);
        send_error(id, -32603, "Out of memory");
        return;
    }

    size_t nr = fread(raw, 1, max_read, f);
    int truncated = (fgetc(f) != EOF);
    fclose(f);
    raw[nr] = 0;

    cat2_options_t opts;
    cat2_options_init(&opts);
    cat2_detect_stats_t stats;
    unsigned char *out_text = NULL;
    size_t out_len = 0;

    int rc = cat2_convert_buffer(raw, nr, &out_text, &out_len, &opts, &stats);
    free(raw);

    if (rc < 0 || !out_text) {
        tool_result(id, "Error decoding file buffer", 1);
        return;
    }

    Buf b; buf_init(&b);
    char hdr[256];
    snprintf(hdr, sizeof(hdr),
             "[cat2] File: %s | Dominant Encoding: %s (%.1f%% confidence) | Size: %zu bytes%s\n\n",
             path, cat2_encoding_name(stats.dominant_encoding), stats.confidence * 100.0,
             out_len, truncated ? " [truncated at 64KB]" : "");
    buf_append(&b, hdr);
    buf_append(&b, (const char*)out_text);
    free(out_text);

    tool_result(id, b.data, 0);
    buf_free(&b);
}

static void handle_cat2_detect(const char *id, const char *args) {
    char path[4096] = {0};
    json_get_string(args, "path", path, sizeof(path));
    if (!path[0]) {
        send_error(id, -32602, "cat2_detect requires path parameter");
        return;
    }

    cat2_detect_stats_t stats;
    if (cat2_detect_file(path, &stats) < 0) {
        char err[4300];
        snprintf(err, sizeof(err), "Cannot open '%s': %s", path, strerror(errno));
        tool_result(id, err, 1);
        return;
    }

    char res[1024];
    snprintf(res, sizeof(res),
             "File: %s\n"
             "Dominant Encoding : %s (%.1f%% confidence)\n"
             "Total Bytes       : %zu\n"
             "ASCII Bytes       : %zu (%.1f%%)\n"
             "EBCDIC Bytes      : %zu (%.1f%%)\n"
             "UTF-8 Bytes       : %zu (%.1f%%)\n"
             "Double ASCII      : %zu (%.1f%%)\n"
             "Double EBCDIC     : %zu (%.1f%%)\n"
             "Invalid/Binary    : %zu (%.1f%%)\n"
             "Lines Processed   : %zu\n",
             path,
             cat2_encoding_name(stats.dominant_encoding), stats.confidence * 100.0,
             stats.total_bytes,
             stats.ascii_bytes, stats.total_bytes ? (stats.ascii_bytes * 100.0 / stats.total_bytes) : 0.0,
             stats.ebcdic_bytes, stats.total_bytes ? (stats.ebcdic_bytes * 100.0 / stats.total_bytes) : 0.0,
             stats.utf8_bytes, stats.total_bytes ? (stats.utf8_bytes * 100.0 / stats.total_bytes) : 0.0,
             stats.double_ascii_bytes, stats.total_bytes ? (stats.double_ascii_bytes * 100.0 / stats.total_bytes) : 0.0,
             stats.double_ebcdic_bytes, stats.total_bytes ? (stats.double_ebcdic_bytes * 100.0 / stats.total_bytes) : 0.0,
             stats.invalid_bytes, stats.total_bytes ? (stats.invalid_bytes * 100.0 / stats.total_bytes) : 0.0,
             stats.lines_processed);

    tool_result(id, res, 0);
}

static void handle_cat2_convert(const char *id, const char *args) {
    char text[65536] = {0};
    char target[32] = {0};
    json_get_string(args, "text", text, sizeof(text));
    json_get_string(args, "target", target, sizeof(target));

    cat2_options_t opts;
    cat2_options_init(&opts);
    if (strcmp(target, "ebcdic") == 0) opts.out_mode = CAT2_OUT_EBCDIC;

    cat2_detect_stats_t stats;
    unsigned char *out_text = NULL;
    size_t out_len = 0;

    int rc = cat2_convert_buffer((const unsigned char*)text, strlen(text), &out_text, &out_len, &opts, &stats);
    if (rc < 0 || !out_text) {
        tool_result(id, "Conversion error", 1);
        return;
    }

    tool_result(id, (const char*)out_text, 0);
    free(out_text);
}

static void handle_cat2_help(const char *id) {
    tool_result(id,
        "cat2 — Intelligent multi-candidate codepage decoder & viewer for z/OS and Unix.\n\n"
        "Tools:\n"
        "  - cat2_read: Read and auto-decode a file into clean readable text for AI analysis.\n"
        "  - cat2_detect: Analyze codepage breakdown (ASCII, IBM-1047 EBCDIC, UTF-8, double-converted, binary).\n"
        "  - cat2_convert: Convert in-memory text/stream to ASCII or EBCDIC.\n"
        "  - cat2_help: Show this guide.\n", 0);
}

static void handle_tools_call(const char *id, const char *req) {
    const char *args = find_arguments(req);
    char name[64] = {0};
    const char *params = strstr(req, "\"params\"");
    if (params) json_get_string(params, "name", name, sizeof(name));
    if (!name[0]) { send_error(id, -32602, "missing tool name"); return; }

    char empty[] = "{}";
    if (!args) args = empty;

    if (strcmp(name, "cat2_read") == 0) handle_cat2_read(id, args);
    else if (strcmp(name, "cat2_detect") == 0) handle_cat2_detect(id, args);
    else if (strcmp(name, "cat2_convert") == 0) handle_cat2_convert(id, args);
    else if (strcmp(name, "cat2_help") == 0) handle_cat2_help(id);
    else {
        char msg[128];
        snprintf(msg, sizeof(msg), "unknown tool: %s", name);
        send_error(id, -32601, msg);
    }
}

static void dispatch(const char *line) {
    char method[128] = {0};
    char id[128] = {0};
    json_get_string(line, "method", method, sizeof(method));
    json_get_id(line, id, sizeof(id));

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(id);
    } else if (strcmp(method, "tools/list") == 0) {
        handle_tools_list(id);
    } else if (strcmp(method, "tools/call") == 0) {
        handle_tools_call(id, line);
    } else if (strcmp(method, "notifications/initialized") == 0) {
        /* No-op */
    } else if (strcmp(method, "ping") == 0) {
        send_result(id, "{}");
    } else if (id[0] && strcmp(id, "null") != 0) {
        send_error(id, -32601, "Method not found");
    }
}

int main(void) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) > 0) {
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n > 0) dispatch(line);
    }
    free(line);
    return 0;
}
