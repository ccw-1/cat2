#include "cat2.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void test_ascii_conversion(void) {
    const char *ascii_input = "Hello, World!\nThis is a standard ASCII text string.\n";
    unsigned char *out = NULL;
    size_t out_len = 0;
    cat2_detect_stats_t stats;

    int rc = cat2_convert_buffer((const unsigned char*)ascii_input, strlen(ascii_input),
                                &out, &out_len, NULL, &stats);
    assert(rc == 0);
    assert(out != NULL);
    assert(strcmp((char*)out, ascii_input) == 0);
    assert(stats.dominant_encoding == CAT2_ENC_ASCII);
    assert(stats.ascii_bytes > 0);
    free(out);
    printf("✓ test_ascii_conversion passed\n");
}

static void test_ebcdic_conversion(void) {
    /* "Hello z/OS!" in EBCDIC-1047:
       H = 0xC8, e = 0x85, l = 0x93, l = 0x93, o = 0x96, ' ' = 0x40,
       z = 0xA9, / = 0x61, O = 0xD6, S = 0xE2, ! = 0x5A, \n = 0x15 */
    const unsigned char ebcdic_input[] = {
        0xC8, 0x85, 0x93, 0x93, 0x96, 0x40, 0xA9, 0x61, 0xD6, 0xE2, 0x5A, 0x15
    };

    unsigned char *out = NULL;
    size_t out_len = 0;
    cat2_detect_stats_t stats;

    int rc = cat2_convert_buffer(ebcdic_input, sizeof(ebcdic_input),
                                &out, &out_len, NULL, &stats);
    assert(rc == 0);
    assert(out != NULL);
    assert(strcmp((char*)out, "Hello z/OS!\n") == 0);
    assert(stats.dominant_encoding == CAT2_ENC_EBCDIC);
    assert(stats.ebcdic_bytes > 0);
    free(out);
    printf("✓ test_ebcdic_conversion passed\n");
}

static void test_utf8_conversion(void) {
    const char *utf8_input = "UTF-8 test: café, 🚀 rocket, and 你好世界!\n";
    unsigned char *out = NULL;
    size_t out_len = 0;
    cat2_detect_stats_t stats;

    int rc = cat2_convert_buffer((const unsigned char*)utf8_input, strlen(utf8_input),
                                &out, &out_len, NULL, &stats);
    assert(rc == 0);
    assert(out != NULL);
    assert(strcmp((char*)out, utf8_input) == 0);
    assert(stats.dominant_encoding == CAT2_ENC_UTF8);
    assert(stats.utf8_bytes > 0);
    free(out);
    printf("✓ test_utf8_conversion passed\n");
}

static void test_mixed_conversion(void) {
    /* Line 1: ASCII "Part 1: ASCII\n"
       Line 2: EBCDIC "Part 2: EBCDIC\n" */
    const unsigned char line1[] = "Part 1: ASCII\n";
    const unsigned char line2_ebcdic[] = {
        0xD7, 0x81, 0x99, 0xA3, 0x40, 0xF2, 0x7A, 0x40,
        0xC5, 0xC2, 0xC3, 0xC4, 0xC9, 0xC3, 0x15
    };

    unsigned char mixed[256];
    memcpy(mixed, line1, sizeof(line1) - 1);
    memcpy(mixed + sizeof(line1) - 1, line2_ebcdic, sizeof(line2_ebcdic));
    size_t total_len = (sizeof(line1) - 1) + sizeof(line2_ebcdic);

    unsigned char *out = NULL;
    size_t out_len = 0;
    cat2_detect_stats_t stats;

    int rc = cat2_convert_buffer(mixed, total_len, &out, &out_len, NULL, &stats);
    assert(rc == 0);
    assert(out != NULL);
    assert(strstr((char*)out, "Part 1: ASCII\n") != NULL);
    assert(strstr((char*)out, "Part 2: EBCDIC\n") != NULL);
    assert(stats.lines_processed >= 2);
    free(out);
    printf("✓ test_mixed_conversion passed\n");
}

int main(void) {
    printf("Running cat2 unit tests...\n");
    test_ascii_conversion();
    test_ebcdic_conversion();
    test_utf8_conversion();
    test_mixed_conversion();
    printf("All cat2 unit tests passed!\n");
    return 0;
}
