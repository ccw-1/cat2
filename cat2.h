#ifndef CAT2_H
#define CAT2_H

#include <stddef.h>
#include <stdio.h>

#define CAT2_VERSION "2.0.0"

#ifdef __cplusplus
extern "C" {
#endif

/* Translation tables */
extern const unsigned char cat2_a2e[256];
extern const unsigned char cat2_e2a[256];
extern const unsigned char cat2_print_ascii[256];
extern const unsigned char cat2_print_ebcdic[256];
extern const unsigned char cat2_print_ascii2[256];
extern const unsigned char cat2_print_ebcdic2[256];
extern const unsigned char cat2_print_invalid[256];
extern const unsigned char cat2_preference[256];

typedef enum {
    CAT2_OUT_ASCII = 0,
    CAT2_OUT_EBCDIC = 1
} cat2_output_mode_t;

typedef enum {
    CAT2_ENC_UNKNOWN = 0,
    CAT2_ENC_ASCII,
    CAT2_ENC_EBCDIC,
    CAT2_ENC_UTF8,
    CAT2_ENC_DOUBLE_ASCII,
    CAT2_ENC_DOUBLE_EBCDIC,
    CAT2_ENC_BINARY
} cat2_encoding_t;

typedef struct {
    size_t total_bytes;
    size_t ascii_bytes;
    size_t ebcdic_bytes;
    size_t utf8_bytes;
    size_t double_ascii_bytes;
    size_t double_ebcdic_bytes;
    size_t invalid_bytes;
    size_t lines_processed;
    cat2_encoding_t dominant_encoding;
    double confidence;
} cat2_detect_stats_t;

typedef struct {
    cat2_output_mode_t out_mode;
    int out_fd;
    int raw_fd;
    size_t buffer_size;
} cat2_options_t;

/* Initialize default options */
void cat2_options_init(cat2_options_t *opts);

/* Process an open file descriptor, streaming converted output to opts->out_fd */
int cat2_process_fd(int fd, const cat2_options_t *opts, cat2_detect_stats_t *stats);

/* Process a named file */
int cat2_process_file(const char *path, const cat2_options_t *opts, cat2_detect_stats_t *stats);

/* Convert in-memory buffer to dynamically allocated string buffer */
int cat2_convert_buffer(const unsigned char *src, size_t src_len,
                        unsigned char **out_buf, size_t *out_len,
                        const cat2_options_t *opts, cat2_detect_stats_t *stats);

/* Detect encoding distribution for a file */
int cat2_detect_file(const char *path, cat2_detect_stats_t *stats);

/* Detect encoding distribution for an in-memory buffer */
int cat2_detect_buffer(const unsigned char *buf, size_t len, cat2_detect_stats_t *stats);

/* Convert encoding enum to human readable string */
const char* cat2_encoding_name(cat2_encoding_t enc);

#ifdef __cplusplus
}
#endif

#endif /* CAT2_H */
