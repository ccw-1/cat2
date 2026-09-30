#define _POSIX_C_SOURCE 200809L
#include "cat2.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TAB_COUNT 5
#define SLIDE_WIDTH 4096

enum {
    TAB_ASCII = 0,
    TAB_EBCDIC = 1,
    TAB_ASCII2 = 2,
    TAB_EBCDIC2 = 3,
    TAB_INVALID = 4
};

static const unsigned char* g_tables[TAB_COUNT] = {
    cat2_print_ascii,
    cat2_print_ebcdic,
    cat2_print_ascii2,
    cat2_print_ebcdic2,
    cat2_print_invalid
};

static inline void unblock(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
}

static inline void block(int fd) {
    int fl = fcntl(fd, F_GETFL);
    if (fl >= 0) fcntl(fd, F_SETFL, fl & ~O_NONBLOCK);
}

void cat2_options_init(cat2_options_t *opts) {
    if (!opts) return;
    opts->out_mode = CAT2_OUT_ASCII;
    opts->out_fd = 1;
    opts->raw_fd = -1;
    opts->buffer_size = SLIDE_WIDTH;
}

const char* cat2_encoding_name(cat2_encoding_t enc) {
    switch (enc) {
        case CAT2_ENC_ASCII: return "ASCII";
        case CAT2_ENC_EBCDIC: return "IBM-1047 (EBCDIC)";
        case CAT2_ENC_UTF8: return "UTF-8";
        case CAT2_ENC_DOUBLE_ASCII: return "Double-Converted ASCII (a2e)";
        case CAT2_ENC_DOUBLE_EBCDIC: return "Double-Converted EBCDIC (e22a)";
        case CAT2_ENC_BINARY: return "Binary / Invalid";
        default: return "Unknown";
    }
}

/* UTF-8 length validation. Returns valid UTF-8 sequence byte length, or 0 if invalid. */
static size_t validate_utf8_char(const unsigned char *s, size_t max_len) {
    if (max_len == 0) return 0;
    unsigned char c = s[0];
    if (c < 0x80) {
        return (c >= 0x20 || c == '\n' || c == '\r' || c == '\t' || c == '\a' || c == '\b' || c == '\v' || c == '\f') ? 1 : 0;
    }
    if ((c & 0xE0) == 0xC0) {
        if (max_len < 2) return 0;
        if ((s[1] & 0xC0) != 0x80) return 0;
        if (c < 0xC2) return 0; /* Overlong */
        return 2;
    }
    if ((c & 0xF0) == 0xE0) {
        if (max_len < 3) return 0;
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80) return 0;
        if (c == 0xE0 && s[1] < 0xA0) return 0; /* Overlong */
        return 3;
    }
    if ((c & 0xF8) == 0xF0) {
        if (max_len < 4) return 0;
        if ((s[1] & 0xC0) != 0x80 || (s[2] & 0xC0) != 0x80 || (s[3] & 0xC0) != 0x80) return 0;
        if (c == 0xF0 && s[1] < 0x90) return 0; /* Overlong */
        return 4;
    }
    return 0;
}

/* Scan UTF-8 run in raw input buffer */
static size_t measure_utf8_run(const unsigned char *raw, size_t count, unsigned int *score_out) {
    size_t i = 0;
    unsigned int score = 0;
    while (i < count) {
        size_t ulen = validate_utf8_char(raw + i, count - i);
        if (ulen == 0) break;
        if (raw[i] == '\n') {
            score += 4;
            i += 1;
            break;
        }
        if (ulen == 1) {
            score += cat2_preference[raw[i]];
        } else {
            score += (unsigned int)(ulen * 4); /* High quality for valid multi-byte UTF-8 */
        }
        i += ulen;
    }
    if (score_out) *score_out = score;
    return i;
}

/* Measure printable span in transformed buffer */
static size_t measure_table_span(const unsigned char *transformed, size_t count, unsigned int *score_out) {
    size_t len = 0;
    unsigned int score = 0;
    while (len < count && transformed[len] != 0) {
        unsigned char c = transformed[len];
        score += cat2_preference[c];
        len++;
        if (c == '\n') break;
    }
    if (score_out) *score_out = score;
    return len;
}

typedef struct {
    unsigned int offset;
    unsigned int count;
    unsigned char *raw;
    unsigned char *buffers[TAB_COUNT];
    unsigned char *storage;
    size_t capacity;
    int eof;
} slide_buffer_t;

static int slide_buffer_init(slide_buffer_t *sb, size_t capacity) {
    sb->capacity = capacity > 0 ? capacity : SLIDE_WIDTH;
    sb->offset = 0;
    sb->count = 0;
    sb->eof = 0;
    sb->storage = (unsigned char*)malloc((TAB_COUNT + 1) * (sb->capacity + 16));
    if (!sb->storage) return -1;
    
    unsigned char *p = sb->storage;
    sb->raw = p;
    p += (sb->capacity + 16);
    for (int i = 0; i < TAB_COUNT; i++) {
        sb->buffers[i] = p;
        p += (sb->capacity + 16);
    }
    return 0;
}

static void slide_buffer_free(slide_buffer_t *sb) {
    if (sb->storage) {
        free(sb->storage);
        sb->storage = NULL;
    }
}

static void slide_buffer_refill(int fd, const unsigned char *mem_src, size_t mem_len, size_t *mem_pos,
                                slide_buffer_t *sb, int raw_fd) {
    if (sb->offset > 0) {
        if (sb->count > 0) {
            memmove(sb->raw, sb->raw + sb->offset, sb->count);
            for (int i = 0; i < TAB_COUNT; i++) {
                memmove(sb->buffers[i], sb->buffers[i] + sb->offset, sb->count);
                sb->buffers[i][sb->count] = 0;
            }
        }
        sb->offset = 0;
    }

    if (sb->eof) return;

    if (sb->count < sb->capacity) {
        size_t needed = sb->capacity - sb->count;
        ssize_t rc = 0;

        if (fd >= 0) {
            if (sb->count == 0) block(fd);
            rc = read(fd, sb->raw + sb->count, needed);
            if (sb->count == 0) unblock(fd);

            if (rc > 0) {
                if (raw_fd >= 0) {
                    ssize_t wrc = write(raw_fd, sb->raw + sb->count, (size_t)rc);
                    (void)wrc;
                }
            } else if (rc == 0) {
                if (errno != EAGAIN) sb->eof = 1;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) sb->eof = 1;
                rc = 0;
            }
        } else if (mem_src && mem_pos) {
            if (*mem_pos >= mem_len) {
                sb->eof = 1;
                return;
            }
            size_t avail = mem_len - *mem_pos;
            rc = (ssize_t)(avail < needed ? avail : needed);
            memcpy(sb->raw + sb->count, mem_src + *mem_pos, (size_t)rc);
            *mem_pos += (size_t)rc;
            if (*mem_pos >= mem_len) sb->eof = 1;
        }

        if (rc > 0) {
            size_t start = sb->count;
            size_t n = (size_t)rc;
            for (int t = 0; t < TAB_COUNT; t++) {
                const unsigned char *tbl = g_tables[t];
                for (size_t k = 0; k < n; k++) {
                    sb->buffers[t][start + k] = tbl[sb->raw[start + k]];
                }
                sb->buffers[t][start + n] = 0;
            }
            sb->count += n;
        }
    }
}

/* Decide the best candidate slice from buffer */
static void select_candidate(slide_buffer_t *sb, int last_tab,
                             int *best_tab_out, size_t *best_len_out, int *is_utf8_out) {
    size_t max_len = 0;
    unsigned int max_score = 0;
    int best_tab = TAB_INVALID;
    int is_utf8 = 0;

    /* Check table conversions first (ASCII, EBCDIC, double-converted) */
    for (int t = 0; t < TAB_COUNT; t++) {
        unsigned int sc = 0;
        size_t len = measure_table_span(sb->buffers[t], sb->count, &sc);

        int is_better = 0;
        if (len > max_len) {
            is_better = 1;
        } else if (len == max_len && len > 0) {
            if (sc > max_score) {
                is_better = 1;
            } else if (sc == max_score && t == last_tab) {
                is_better = 1; /* Favor continuity */
            }
        }

        if (is_better) {
            max_len = len;
            max_score = sc;
            best_tab = t;
            is_utf8 = 0;
        }
    }

    /* Check UTF-8 directly on raw if raw contains multibyte UTF-8 characters */
    unsigned int utf8_score = 0;
    size_t utf8_len = measure_utf8_run(sb->raw, sb->count, &utf8_score);
    if (utf8_len > max_len || (utf8_len == max_len && utf8_score > max_score)) {
        /* Check if there are actual non-ASCII multi-byte bytes */
        int has_multibyte = 0;
        for (size_t i = 0; i < utf8_len; i++) {
            if (sb->raw[i] >= 0x80) {
                has_multibyte = 1;
                break;
            }
        }
        if (has_multibyte) {
            max_len = utf8_len;
            max_score = utf8_score;
            best_tab = TAB_INVALID;
            is_utf8 = 1;
        }
    }

    /* Fallback if no candidate made progress */
    if (max_len == 0 && sb->count > 0) {
        max_len = 1;
        best_tab = TAB_INVALID;
        is_utf8 = 0;
    }

    *best_tab_out = best_tab;
    *best_len_out = max_len;
    *is_utf8_out = is_utf8;
}

/* Write formatted output chunk */
static int write_output_chunk(const unsigned char *data, size_t len,
                              cat2_output_mode_t mode, int out_fd,
                              unsigned char **mem_out, size_t *mem_out_len, size_t *mem_out_cap) {
    if (len == 0) return 0;

    unsigned char tr[SLIDE_WIDTH];
    const unsigned char *src = data;

    if (mode == CAT2_OUT_EBCDIC) {
        while (len > 0) {
            size_t chunk = len < sizeof(tr) ? len : sizeof(tr);
            for (size_t i = 0; i < chunk; i++) {
                tr[i] = cat2_a2e[src[i]];
            }
            if (out_fd >= 0) {
                if (write(out_fd, tr, chunk) < 0) return -1;
            }
            if (mem_out && mem_out_len && mem_out_cap) {
                while (*mem_out_len + chunk + 1 > *mem_out_cap) {
                    *mem_out_cap = (*mem_out_cap == 0) ? 4096 : (*mem_out_cap * 2);
                    *mem_out = (unsigned char*)realloc(*mem_out, *mem_out_cap);
                    if (!*mem_out) return -1;
                }
                memcpy(*mem_out + *mem_out_len, tr, chunk);
                *mem_out_len += chunk;
                (*mem_out)[*mem_out_len] = 0;
            }
            src += chunk;
            len -= chunk;
        }
    } else {
        /* ASCII out */
        if (out_fd >= 0) {
            if (write(out_fd, src, len) < 0) return -1;
        }
        if (mem_out && mem_out_len && mem_out_cap) {
            while (*mem_out_len + len + 1 > *mem_out_cap) {
                *mem_out_cap = (*mem_out_cap == 0) ? 4096 : (*mem_out_cap * 2);
                *mem_out = (unsigned char*)realloc(*mem_out, *mem_out_cap);
                if (!*mem_out) return -1;
            }
            memcpy(*mem_out + *mem_out_len, src, len);
            *mem_out_len += len;
            (*mem_out)[*mem_out_len] = 0;
        }
    }
    return 0;
}

static int process_core(int fd, const unsigned char *mem_src, size_t mem_src_len,
                        unsigned char **mem_out, size_t *mem_out_len,
                        const cat2_options_t *opts, cat2_detect_stats_t *stats) {
    cat2_options_t def_opts;
    if (!opts) {
        cat2_options_init(&def_opts);
        opts = &def_opts;
    }

    if (stats) memset(stats, 0, sizeof(cat2_detect_stats_t));

    slide_buffer_t sb;
    if (slide_buffer_init(&sb, opts->buffer_size) < 0) return -1;

    size_t mem_pos = 0;
    size_t mem_out_cap = 0;
    if (mem_out && mem_out_len) {
        *mem_out = NULL;
        *mem_out_len = 0;
    }

    if (fd >= 0) unblock(fd);

    slide_buffer_refill(fd, mem_src, mem_src_len, &mem_pos, &sb, opts->raw_fd);

    int last_tab = -1;
    while (sb.count > 0) {
        int best_tab = TAB_INVALID;
        size_t len = 0;
        int is_utf8 = 0;

        select_candidate(&sb, last_tab, &best_tab, &len, &is_utf8);

        const unsigned char *out_bytes = NULL;
        if (is_utf8) {
            out_bytes = sb.raw;
            if (stats) {
                stats->utf8_bytes += len;
                stats->total_bytes += len;
            }
        } else {
            out_bytes = sb.buffers[best_tab];
            if (stats) {
                stats->total_bytes += len;
                switch (best_tab) {
                    case TAB_ASCII: stats->ascii_bytes += len; break;
                    case TAB_EBCDIC: stats->ebcdic_bytes += len; break;
                    case TAB_ASCII2: stats->double_ascii_bytes += len; break;
                    case TAB_EBCDIC2: stats->double_ebcdic_bytes += len; break;
                    default: stats->invalid_bytes += len; break;
                }
            }
        }

        if (stats) {
            for (size_t i = 0; i < len; i++) {
                if (out_bytes[i] == '\n') stats->lines_processed++;
            }
        }

        if (write_output_chunk(out_bytes, len, opts->out_mode, opts->out_fd, mem_out, mem_out_len, &mem_out_cap) < 0) {
            slide_buffer_free(&sb);
            return -1;
        }

        sb.offset = len;
        sb.count -= len;
        last_tab = best_tab;

        slide_buffer_refill(fd, mem_src, mem_src_len, &mem_pos, &sb, opts->raw_fd);
    }

    slide_buffer_free(&sb);

    if (stats && stats->total_bytes > 0) {
        size_t max_b = stats->ascii_bytes;
        stats->dominant_encoding = CAT2_ENC_ASCII;

        if (stats->ebcdic_bytes > max_b) {
            max_b = stats->ebcdic_bytes;
            stats->dominant_encoding = CAT2_ENC_EBCDIC;
        }
        if (stats->utf8_bytes > max_b) {
            max_b = stats->utf8_bytes;
            stats->dominant_encoding = CAT2_ENC_UTF8;
        }
        if (stats->double_ascii_bytes > max_b) {
            max_b = stats->double_ascii_bytes;
            stats->dominant_encoding = CAT2_ENC_DOUBLE_ASCII;
        }
        if (stats->double_ebcdic_bytes > max_b) {
            max_b = stats->double_ebcdic_bytes;
            stats->dominant_encoding = CAT2_ENC_DOUBLE_EBCDIC;
        }
        if (stats->invalid_bytes > max_b) {
            max_b = stats->invalid_bytes;
            stats->dominant_encoding = CAT2_ENC_BINARY;
        }

        stats->confidence = ((double)max_b) / (double)stats->total_bytes;
    }

    return 0;
}

int cat2_process_fd(int fd, const cat2_options_t *opts, cat2_detect_stats_t *stats) {
    return process_core(fd, NULL, 0, NULL, NULL, opts, stats);
}

int cat2_process_file(const char *path, const cat2_options_t *opts, cat2_detect_stats_t *stats) {
    if (!path || strcmp(path, "-") == 0) {
        return cat2_process_fd(0, opts, stats);
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int rc = cat2_process_fd(fd, opts, stats);
    close(fd);
    return rc;
}

int cat2_convert_buffer(const unsigned char *src, size_t src_len,
                        unsigned char **out_buf, size_t *out_len,
                        const cat2_options_t *opts, cat2_detect_stats_t *stats) {
    if (!src || !out_buf || !out_len) return -1;
    cat2_options_t local_opts;
    if (opts) {
        local_opts = *opts;
    } else {
        cat2_options_init(&local_opts);
    }
    local_opts.out_fd = -1; /* Memory only */
    return process_core(-1, src, src_len, out_buf, out_len, &local_opts, stats);
}

int cat2_detect_buffer(const unsigned char *buf, size_t len, cat2_detect_stats_t *stats) {
    if (!buf || !stats) return -1;
    cat2_options_t opts;
    cat2_options_init(&opts);
    opts.out_fd = -1;
    return process_core(-1, buf, len, NULL, NULL, &opts, stats);
}

int cat2_detect_file(const char *path, cat2_detect_stats_t *stats) {
    if (!path || !stats) return -1;
    int fd = (strcmp(path, "-") == 0) ? 0 : open(path, O_RDONLY);
    if (fd < 0) return -1;
    cat2_options_t opts;
    cat2_options_init(&opts);
    opts.out_fd = -1;
    int rc = process_core(fd, NULL, 0, NULL, NULL, &opts, stats);
    if (fd != 0) close(fd);
    return rc;
}
