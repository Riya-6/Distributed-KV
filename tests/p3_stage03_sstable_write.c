/*
 * Phase 3, Stage 3: SSTable write path.
 * Contract under test: include/kv/sstable.h's kv_sstable_write() ->
 * src/sstable.c (you write this).
 * See docs/stages/phase3-lsm-tree.md, Stage 3.
 *
 * Deliberately does NOT use kv_sstable_open()/kv_sstable_get() -- those
 * are Stage 4. This stage verifies the written file directly with
 * kv_parse_frame()/kv_decode_command() from Phase 1, which is exactly
 * what Stage 4's read path will also do internally.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kv/command.h"
#include "kv/memtable.h"
#include "kv/protocol.h"
#include "kv/sstable.h"
#include "test_framework.h"

#define TEST_SSTABLE_PATH "build/p3_stage03_test.sst"

static uint8_t *read_whole_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc((size_t)size);
    if (buf == NULL) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (n != (size_t)size) {
        free(buf);
        return NULL;
    }
    *out_len = (size_t)size;
    return buf;
}

static void test_write_produces_sorted_frames_including_tombstone(void) {
    remove(TEST_SSTABLE_PATH);

    kv_memtable_t *mt = kv_memtable_create();
    kv_memtable_put(mt, (const uint8_t *)"charlie", 7, (const uint8_t *)"3", 1);
    kv_memtable_put(mt, (const uint8_t *)"alpha", 5, (const uint8_t *)"1", 1);
    kv_memtable_delete(mt, (const uint8_t *)"bravo", 5); /* tombstone */

    KV_ASSERT_EQ_INT(kv_sstable_write(TEST_SSTABLE_PATH, mt), 0);
    kv_memtable_destroy(mt);

    size_t file_len = 0;
    uint8_t *file_bytes = read_whole_file(TEST_SSTABLE_PATH, &file_len);
    KV_ASSERT(file_bytes != NULL);

    static const char *expected_keys[] = {"alpha", "bravo", "charlie"};
    static const size_t expected_key_lens[] = {5, 5, 7};
    static const int expected_is_delete[] = {0, 1, 0};

    size_t offset = 0;
    for (size_t i = 0; i < 3; i++) {
        kv_frame_t frame;
        int prc = kv_parse_frame(file_bytes + offset, file_len - offset, &frame);
        KV_ASSERT_EQ_INT(prc, 1);

        kv_command_t cmd;
        KV_ASSERT_EQ_INT(kv_decode_command(&frame, &cmd), 0);

        if (expected_is_delete[i]) {
            KV_ASSERT_EQ_INT(frame.opcode, KV_OP_DELETE);
        } else {
            KV_ASSERT_EQ_INT(frame.opcode, KV_OP_SET);
        }
        KV_ASSERT_EQ_BYTES(cmd.key, cmd.key_len, expected_keys[i],
                            expected_key_lens[i]);

        offset += frame.frame_len;
    }
    KV_ASSERT_EQ_INT(offset, file_len); /* exactly 3 records, nothing more */

    free(file_bytes);
    remove(TEST_SSTABLE_PATH);
}

int main(void) {
    KV_RUN(test_write_produces_sorted_frames_including_tombstone);
    KV_REPORT_AND_EXIT();
}
