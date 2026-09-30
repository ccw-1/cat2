#define _POSIX_C_SOURCE 200809L
#include "cat2.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void print_help(const char *prog) {
    fprintf(stderr,
        "cat2 version 2.0.0\n"
        "Usage: %s [OPTION]... [FILE]...\n"
        "Concatenate FILE(s) to standard output in auto-detected human readable form.\n"
        "\n"
        "With no FILE, or when FILE is -, read standard input.\n"
        "\n"
        "Options:\n"
        "  -a               Output in ASCII (default on Unix/Linux)\n"
        "  -e               Output in EBCDIC (IBM-1047)\n"
        "  -2               Send output to standard error (fd 2)\n"
        "  -o <logfile>     Save raw unconverted input stream to <logfile>\n"
        "  -d, --detect     Show encoding detection summary instead of streaming\n"
        "  -h, --help       Show this help dialog\n"
        "  -v, --version    Show version information\n"
        "\n"
        "Examples:\n"
        "  %s myfile.txt\n"
        "  %s -o raw.bin f - g\n"
        "  ssh user@zos 'cat /path/to/logfile' | %s\n",
        prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    cat2_options_t opts;
    cat2_options_init(&opts);

    int detect_only = 0;
    int files_specified = 0;
    int raw_fd = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-a") == 0) {
            opts.out_mode = CAT2_OUT_ASCII;
        } else if (strcmp(argv[i], "-e") == 0) {
            opts.out_mode = CAT2_OUT_EBCDIC;
        } else if (strcmp(argv[i], "-2") == 0) {
            opts.out_fd = 2;
        } else if (strcmp(argv[i], "-d") == 0 || strcmp(argv[i], "--detect") == 0) {
            detect_only = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("cat2 version 2.0.0\n");
            return 0;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 < argc) {
                i++;
                raw_fd = open(argv[i], O_WRONLY | O_CREAT | O_APPEND, 0644);
                if (raw_fd < 0) {
                    fprintf(stderr, "Error opening raw logfile '%s': %s\n", argv[i], strerror(errno));
                    return 1;
                }
                opts.raw_fd = raw_fd;
            } else {
                fprintf(stderr, "-o requires a logfile path argument\n");
                return 1;
            }
        } else if (argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_help(argv[0]);
            return 1;
        }
    }

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && strcmp(argv[i], "-") != 0) {
            if (strcmp(argv[i], "-o") == 0) i++; /* Skip arg */
            continue;
        }

        files_specified++;
        const char *target = argv[i];

        if (detect_only) {
            cat2_detect_stats_t stats;
            if (cat2_detect_file(target, &stats) < 0) {
                fprintf(stderr, "Error detecting '%s': %s\n", target, strerror(errno));
                rc = 1;
                continue;
            }
            printf("File: %s\n", target);
            printf("  Dominant encoding: %s (%.1f%% confidence)\n",
                   cat2_encoding_name(stats.dominant_encoding), stats.confidence * 100.0);
            printf("  Total bytes      : %zu\n", stats.total_bytes);
            printf("  ASCII bytes      : %zu\n", stats.ascii_bytes);
            printf("  EBCDIC bytes     : %zu\n", stats.ebcdic_bytes);
            printf("  UTF-8 bytes      : %zu\n", stats.utf8_bytes);
            printf("  Double ASCII     : %zu\n", stats.double_ascii_bytes);
            printf("  Double EBCDIC    : %zu\n", stats.double_ebcdic_bytes);
            printf("  Invalid/Binary   : %zu\n", stats.invalid_bytes);
            printf("  Lines processed  : %zu\n\n", stats.lines_processed);
        } else {
            cat2_detect_stats_t stats;
            if (cat2_process_file(target, &opts, &stats) < 0) {
                fprintf(stderr, "Error processing '%s': %s\n", target, strerror(errno));
                rc = 1;
            }
        }
    }

    if (files_specified == 0) {
        if (detect_only) {
            cat2_detect_stats_t stats;
            if (cat2_detect_file("-", &stats) < 0) {
                fprintf(stderr, "Error detecting stdin: %s\n", strerror(errno));
                rc = 1;
            } else {
                printf("Stream: stdin\n");
                printf("  Dominant encoding: %s (%.1f%% confidence)\n",
                       cat2_encoding_name(stats.dominant_encoding), stats.confidence * 100.0);
                printf("  Total bytes      : %zu\n", stats.total_bytes);
                printf("  ASCII bytes      : %zu\n", stats.ascii_bytes);
                printf("  EBCDIC bytes     : %zu\n", stats.ebcdic_bytes);
                printf("  UTF-8 bytes      : %zu\n", stats.utf8_bytes);
                printf("  Lines processed  : %zu\n", stats.lines_processed);
            }
        } else {
            cat2_detect_stats_t stats;
            if (cat2_process_file("-", &opts, &stats) < 0) {
                fprintf(stderr, "Error processing stdin: %s\n", strerror(errno));
                rc = 1;
            }
        }
    }

    if (raw_fd >= 0) close(raw_fd);
    return rc;
}
