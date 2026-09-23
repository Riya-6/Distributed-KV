# Build/test infra. Run this from inside WSL2 Ubuntu (or any real Linux
# box) -- see README.md's Environment section. Native Windows has no
# sys/socket.h and no valgrind; this Makefile assumes a POSIX toolchain.

CC      := gcc
CFLAGS  := -std=c11 -Wall -Wextra -Werror -g -pthread -Iinclude -Itests
LDFLAGS := -pthread

VALGRIND      := valgrind
VALGRIND_OPTS := --leak-check=full --show-leak-kinds=all --error-exitcode=1 --quiet

BUILD := build

.PHONY: all clean test-p1 valgrind-p1 test-p2 valgrind-p2 test-p3 valgrind-p3 \
        test-p1-s1 test-p1-s2 test-p1-s3 test-p1-s4 test-p1-s5 \
        test-p2-s1 test-p2-s2 test-p2-s3 \
        test-p3-s1 test-p3-s2 test-p3-s3 test-p3-s4 test-p3-s5 test-p3-s6

all: test-p1

$(BUILD):
	mkdir -p $(BUILD)

# --- Phase 1 --------------------------------------------------------------
# Each stage links only the src/ files its own contract needs (see
# docs/stages/phase1-tcp-server.md). None of these src/*.c files are
# scaffolded -- you create them. Until a stage's file exists, `make`
# fails loudly here with "No rule to make target 'src/....c'", naming
# exactly what's missing.

$(BUILD)/p1_stage01: tests/p1_stage01_listener.c src/net.c tests/helpers/test_client.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/p1_stage02: tests/p1_stage02_frame_parser.c src/protocol.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/p1_stage03: tests/p1_stage03_command_codec.c src/protocol.c src/command.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

# src/store.c is required here since server.c's dispatch calls
# kv_store_*() unconditionally -- and, as of Phase 3 Stage 5, store.c
# itself is now the LSM engine, so memtable.c/wal.c/sstable.c come
# along with it at link time too.
$(BUILD)/p1_stage04: tests/p1_stage04_server_loop.c src/net.c src/protocol.c src/command.c src/server.c src/store.c src/memtable.c src/wal.c src/sstable.c tests/helpers/test_client.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/p1_stage05: tests/p1_stage05_server_lifecycle.c src/net.c src/protocol.c src/command.c src/server.c src/store.c src/memtable.c src/wal.c src/sstable.c tests/helpers/test_client.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

P1_BINS := $(BUILD)/p1_stage01 $(BUILD)/p1_stage02 $(BUILD)/p1_stage03 \
           $(BUILD)/p1_stage04 $(BUILD)/p1_stage05

test-p1-s1: $(BUILD)/p1_stage01
	./$(BUILD)/p1_stage01

test-p1-s2: $(BUILD)/p1_stage02
	./$(BUILD)/p1_stage02

test-p1-s3: $(BUILD)/p1_stage03
	./$(BUILD)/p1_stage03

test-p1-s4: $(BUILD)/p1_stage04
	./$(BUILD)/p1_stage04

test-p1-s5: $(BUILD)/p1_stage05
	./$(BUILD)/p1_stage05

test-p1: $(P1_BINS)
	@status=0; \
	for bin in $(P1_BINS); do \
		echo "== $$bin =="; \
		./$$bin || status=1; \
	done; \
	exit $$status

valgrind-p1: $(P1_BINS)
	@status=0; \
	for bin in $(P1_BINS); do \
		echo "== valgrind $$bin =="; \
		$(VALGRIND) $(VALGRIND_OPTS) ./$$bin || status=1; \
	done; \
	exit $$status

# --- Phase 2 ---------------------------------------------------------------
# See docs/stages/phase2-storage.md. store.c pulls in memtable.c/wal.c/
# sstable.c too, same as the Phase 1 stage 4/5 rules above -- store.c
# became the LSM engine's orchestrator in Phase 3 Stage 5.

$(BUILD)/p2_stage01: tests/p2_stage01_store_set_get.c src/store.c src/memtable.c src/wal.c src/sstable.c src/protocol.c src/command.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/p2_stage02: tests/p2_stage02_store_delete_destroy.c src/store.c src/memtable.c src/wal.c src/sstable.c src/protocol.c src/command.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/p2_stage03: tests/p2_stage03_store_over_wire.c src/net.c src/protocol.c src/command.c src/server.c src/store.c src/memtable.c src/wal.c src/sstable.c tests/helpers/test_client.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

P2_BINS := $(BUILD)/p2_stage01 $(BUILD)/p2_stage02 $(BUILD)/p2_stage03

test-p2-s1: $(BUILD)/p2_stage01
	./$(BUILD)/p2_stage01

test-p2-s2: $(BUILD)/p2_stage02
	./$(BUILD)/p2_stage02

test-p2-s3: $(BUILD)/p2_stage03
	./$(BUILD)/p2_stage03

test-p2: $(P2_BINS)
	@status=0; \
	for bin in $(P2_BINS); do \
		echo "== $$bin =="; \
		./$$bin || status=1; \
	done; \
	exit $$status

valgrind-p2: $(P2_BINS)
	@status=0; \
	for bin in $(P2_BINS); do \
		echo "== valgrind $$bin =="; \
		$(VALGRIND) $(VALGRIND_OPTS) ./$$bin || status=1; \
	done; \
	exit $$status

# --- Phase 3 ---------------------------------------------------------------
# See docs/stages/phase3-lsm-tree.md. wal.c and sstable.c reuse
# protocol.c/command.c's frame encode/decode for their on-disk record
# format (see that doc's "idea that makes several stages simpler"),
# hence those two showing up as link deps here despite this being
# storage, not networking, code.

$(BUILD)/p3_stage01: tests/p3_stage01_memtable.c src/memtable.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/p3_stage02: tests/p3_stage02_wal.c src/wal.c src/protocol.c src/command.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/p3_stage03: tests/p3_stage03_sstable_write.c src/sstable.c src/memtable.c src/protocol.c src/command.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/p3_stage04: tests/p3_stage04_sstable_read.c src/sstable.c src/memtable.c src/protocol.c src/command.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/p3_stage05: tests/p3_stage05_engine_integration.c src/store.c src/memtable.c src/wal.c src/sstable.c src/protocol.c src/command.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD)/p3_stage06: tests/p3_stage06_compaction.c src/sstable.c src/memtable.c src/protocol.c src/command.c | $(BUILD)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

P3_BINS := $(BUILD)/p3_stage01 $(BUILD)/p3_stage02 $(BUILD)/p3_stage03 \
           $(BUILD)/p3_stage04 $(BUILD)/p3_stage05 $(BUILD)/p3_stage06

test-p3-s1: $(BUILD)/p3_stage01
	./$(BUILD)/p3_stage01

test-p3-s2: $(BUILD)/p3_stage02
	./$(BUILD)/p3_stage02

test-p3-s3: $(BUILD)/p3_stage03
	./$(BUILD)/p3_stage03

test-p3-s4: $(BUILD)/p3_stage04
	./$(BUILD)/p3_stage04

test-p3-s5: $(BUILD)/p3_stage05
	./$(BUILD)/p3_stage05

test-p3-s6: $(BUILD)/p3_stage06
	./$(BUILD)/p3_stage06

test-p3: $(P3_BINS)
	@status=0; \
	for bin in $(P3_BINS); do \
		echo "== $$bin =="; \
		./$$bin || status=1; \
	done; \
	exit $$status

valgrind-p3: $(P3_BINS)
	@status=0; \
	for bin in $(P3_BINS); do \
		echo "== valgrind $$bin =="; \
		$(VALGRIND) $(VALGRIND_OPTS) ./$$bin || status=1; \
	done; \
	exit $$status

# ---------------------------------------------------------------------------
# Later phases (Phase 4 onward) add their own src/test groups and
# test-pN / valgrind-pN targets here, following the same pattern.

clean:
	rm -rf $(BUILD)
