#include <ctype.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BUFFER_UNIT_SIZE 64
#define BUFFER_COUNT 16
#define L1_HASH_SIZE 8
#define L2_HASH_SIZE 8
#define MAX_TRACE_LINE 256

#define READ_SCORE_INC 2ULL
#define WRITE_SCORE_INC 4ULL
#define MISS_SCORE_INC 1ULL
#define DIRTY_EVICT_BONUS 1ULL

typedef struct Buffer {
    bool valid;
    bool dirty;
    uint64_t key;
    uint64_t score;
    uint64_t hits;
    uint64_t last_access_tick;
    unsigned char data[BUFFER_UNIT_SIZE];
    struct Buffer *hash_next;
} Buffer;

typedef struct {
    Buffer buffers[BUFFER_COUNT];
    Buffer *hash_table[L1_HASH_SIZE][L2_HASH_SIZE];
    uint64_t tick;
    uint64_t reads;
    uint64_t writes;
    uint64_t hits;
    uint64_t misses;
    uint64_t evictions;
    uint64_t dirty_flushes;
} BufferManager;

typedef struct {
    uint64_t key;
    unsigned char data[BUFFER_UNIT_SIZE];
} BackingRecord;

typedef struct {
    BackingRecord *records;
    size_t count;
    size_t capacity;
} BackingStore;

static uint64_t mix_u64(uint64_t x) {
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdULL;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ULL;
    x ^= x >> 33;
    return x;
}

static size_t l1_hash(uint64_t key) {
    return (size_t)(mix_u64(key) % L1_HASH_SIZE);
}

static size_t l2_hash(uint64_t key) {
    return (size_t)(mix_u64(key ^ (key >> 32) ^ 0x9e3779b97f4a7c15ULL) % L2_HASH_SIZE);
}

static void backing_store_init(BackingStore *store) {
    store->records = NULL;
    store->count = 0;
    store->capacity = 0;
}

static void backing_store_free(BackingStore *store) {
    free(store->records);
    store->records = NULL;
    store->count = 0;
    store->capacity = 0;
}

static BackingRecord *backing_store_find(BackingStore *store, uint64_t key) {
    for (size_t i = 0; i < store->count; ++i) {
        if (store->records[i].key == key) {
            return &store->records[i];
        }
    }
    return NULL;
}

static const BackingRecord *backing_store_find_const(const BackingStore *store, uint64_t key) {
    for (size_t i = 0; i < store->count; ++i) {
        if (store->records[i].key == key) {
            return &store->records[i];
        }
    }
    return NULL;
}

static bool backing_store_reserve(BackingStore *store, size_t needed) {
    if (needed <= store->capacity) {
        return true;
    }

    size_t new_capacity = store->capacity == 0 ? 32 : store->capacity * 2;
    while (new_capacity < needed) {
        new_capacity *= 2;
    }

    BackingRecord *new_records = realloc(store->records, new_capacity * sizeof(*new_records));
    if (new_records == NULL) {
        return false;
    }

    store->records = new_records;
    store->capacity = new_capacity;
    return true;
}

static bool backing_store_write(BackingStore *store, uint64_t key, const unsigned char data[BUFFER_UNIT_SIZE]) {
    BackingRecord *record = backing_store_find(store, key);
    if (record == NULL) {
        if (!backing_store_reserve(store, store->count + 1)) {
            return false;
        }
        record = &store->records[store->count++];
        record->key = key;
    }

    memcpy(record->data, data, BUFFER_UNIT_SIZE);
    return true;
}

static void backing_store_read(const BackingStore *store, uint64_t key, unsigned char data[BUFFER_UNIT_SIZE]) {
    const BackingRecord *record = backing_store_find_const(store, key);
    if (record == NULL) {
        memset(data, 0, BUFFER_UNIT_SIZE);
        return;
    }

    memcpy(data, record->data, BUFFER_UNIT_SIZE);
}

static void buffer_manager_init(BufferManager *manager) {
    memset(manager, 0, sizeof(*manager));
}

static Buffer *buffer_find(BufferManager *manager, uint64_t key) {
    size_t l1 = l1_hash(key);
    size_t l2 = l2_hash(key);
    for (Buffer *cursor = manager->hash_table[l1][l2]; cursor != NULL; cursor = cursor->hash_next) {
        if (cursor->valid && cursor->key == key) {
            return cursor;
        }
    }
    return NULL;
}

static void hash_insert(BufferManager *manager, Buffer *buffer) {
    size_t l1 = l1_hash(buffer->key);
    size_t l2 = l2_hash(buffer->key);
    buffer->hash_next = manager->hash_table[l1][l2];
    manager->hash_table[l1][l2] = buffer;
}

static void hash_remove(BufferManager *manager, Buffer *buffer) {
    size_t l1 = l1_hash(buffer->key);
    size_t l2 = l2_hash(buffer->key);
    Buffer **cursor = &manager->hash_table[l1][l2];

    while (*cursor != NULL) {
        if (*cursor == buffer) {
            *cursor = buffer->hash_next;
            buffer->hash_next = NULL;
            return;
        }
        cursor = &(*cursor)->hash_next;
    }
}

static uint64_t eviction_score(const Buffer *buffer) {
    uint64_t dirty_bonus = buffer->dirty ? DIRTY_EVICT_BONUS : 0;
    return buffer->score + dirty_bonus;
}

static Buffer *select_victim(BufferManager *manager) {
    Buffer *victim = NULL;
    for (size_t i = 0; i < BUFFER_COUNT; ++i) {
        Buffer *candidate = &manager->buffers[i];
        if (!candidate->valid) {
            return candidate;
        }
        if (victim == NULL || eviction_score(candidate) < eviction_score(victim) ||
            (eviction_score(candidate) == eviction_score(victim) &&
             candidate->last_access_tick < victim->last_access_tick)) {
            victim = candidate;
        }
    }
    return victim;
}

static bool flush_if_dirty(BufferManager *manager, BackingStore *store, Buffer *buffer) {
    if (!buffer->valid || !buffer->dirty) {
        return true;
    }

    if (!backing_store_write(store, buffer->key, buffer->data)) {
        return false;
    }

    buffer->dirty = false;
    manager->dirty_flushes++;
    return true;
}

static bool prepare_buffer_for_key(BufferManager *manager, BackingStore *store, uint64_t key, Buffer **out_buffer) {
    Buffer *buffer = select_victim(manager);
    if (buffer == NULL) {
        return false;
    }

    if (buffer->valid) {
        if (!flush_if_dirty(manager, store, buffer)) {
            return false;
        }
        hash_remove(manager, buffer);
        manager->evictions++;
    }

    backing_store_read(store, key, buffer->data);
    buffer->valid = true;
    buffer->dirty = false;
    buffer->key = key;
    buffer->score = MISS_SCORE_INC;
    buffer->hits = 0;
    buffer->last_access_tick = ++manager->tick;
    buffer->hash_next = NULL;
    hash_insert(manager, buffer);
    *out_buffer = buffer;
    return true;
}

static void touch_buffer(BufferManager *manager, Buffer *buffer, uint64_t score_increment) {
    buffer->hits++;
    buffer->score += score_increment;
    buffer->last_access_tick = ++manager->tick;
}

static bool buffer_read(BufferManager *manager, BackingStore *store, uint64_t key,
                        unsigned char out_data[BUFFER_UNIT_SIZE], bool *hit) {
    manager->reads++;
    Buffer *buffer = buffer_find(manager, key);
    if (buffer != NULL) {
        manager->hits++;
        *hit = true;
    } else {
        manager->misses++;
        *hit = false;
        if (!prepare_buffer_for_key(manager, store, key, &buffer)) {
            return false;
        }
    }

    touch_buffer(manager, buffer, READ_SCORE_INC);
    memcpy(out_data, buffer->data, BUFFER_UNIT_SIZE);
    return true;
}

static bool buffer_write(BufferManager *manager, BackingStore *store, uint64_t key,
                         const unsigned char in_data[BUFFER_UNIT_SIZE], bool *hit) {
    manager->writes++;
    Buffer *buffer = buffer_find(manager, key);
    if (buffer != NULL) {
        manager->hits++;
        *hit = true;
    } else {
        manager->misses++;
        *hit = false;
        if (!prepare_buffer_for_key(manager, store, key, &buffer)) {
            return false;
        }
    }

    memcpy(buffer->data, in_data, BUFFER_UNIT_SIZE);
    buffer->dirty = true;
    touch_buffer(manager, buffer, WRITE_SCORE_INC);
    return true;
}

static bool buffer_manager_flush_all(BufferManager *manager, BackingStore *store) {
    for (size_t i = 0; i < BUFFER_COUNT; ++i) {
        if (!flush_if_dirty(manager, store, &manager->buffers[i])) {
            return false;
        }
    }
    return true;
}

static void make_payload(const char *text, unsigned char data[BUFFER_UNIT_SIZE]) {
    memset(data, 0, BUFFER_UNIT_SIZE);
    if (text != NULL) {
        snprintf((char *)data, BUFFER_UNIT_SIZE, "%s", text);
    }
}

static void print_data_preview(const unsigned char data[BUFFER_UNIT_SIZE]) {
    char preview[BUFFER_UNIT_SIZE + 1];
    memcpy(preview, data, BUFFER_UNIT_SIZE);
    preview[BUFFER_UNIT_SIZE] = '\0';
    for (size_t i = 0; i < BUFFER_UNIT_SIZE; ++i) {
        if (preview[i] == '\0') {
            break;
        }
        if (!isprint((unsigned char)preview[i])) {
            preview[i] = '.';
        }
    }
    printf("%s", preview);
}

static bool parse_trace_line(char *line, char *op, uint64_t *key, char **payload) {
    char *cursor = line;
    while (isspace((unsigned char)*cursor)) {
        cursor++;
    }
    if (*cursor == '\0' || *cursor == '#') {
        return false;
    }

    char *op_token = strtok(cursor, " \t\r\n");
    char *key_token = strtok(NULL, " \t\r\n");
    char *payload_token = strtok(NULL, "\r\n");
    if (op_token == NULL || key_token == NULL) {
        fprintf(stderr, "invalid trace line: missing op/key\n");
        exit(EXIT_FAILURE);
    }

    *op = (char)toupper((unsigned char)op_token[0]);
    char *end = NULL;
    *key = strtoull(key_token, &end, 0);
    if (end == key_token || *end != '\0') {
        fprintf(stderr, "invalid trace line: bad key '%s'\n", key_token);
        exit(EXIT_FAILURE);
    }

    if (payload_token != NULL) {
        while (isspace((unsigned char)*payload_token)) {
            payload_token++;
        }
    }
    *payload = payload_token;
    return true;
}

static bool verify_read(const BackingStore *expected_store, uint64_t key,
                        const unsigned char actual[BUFFER_UNIT_SIZE]) {
    unsigned char expected[BUFFER_UNIT_SIZE];
    backing_store_read(expected_store, key, expected);
    return memcmp(expected, actual, BUFFER_UNIT_SIZE) == 0;
}

static void print_summary(const BufferManager *manager, uint64_t verified_reads) {
    uint64_t accesses = manager->reads + manager->writes;
    double hit_rate = accesses == 0 ? 0.0 : (double)manager->hits * 100.0 / (double)accesses;

    printf("\n=== DDR Buffer Manager Summary ===\n");
    printf("unit_size=%d bytes, buffer_count=%d, hash=%dx%d\n",
           BUFFER_UNIT_SIZE, BUFFER_COUNT, L1_HASH_SIZE, L2_HASH_SIZE);
    printf("accesses=%" PRIu64 ", reads=%" PRIu64 ", writes=%" PRIu64 "\n",
           accesses, manager->reads, manager->writes);
    printf("hits=%" PRIu64 ", misses=%" PRIu64 ", hit_rate=%.2f%%\n",
           manager->hits, manager->misses, hit_rate);
    printf("evictions=%" PRIu64 ", dirty_flushes=%" PRIu64 ", verified_reads=%" PRIu64 "\n",
           manager->evictions, manager->dirty_flushes, verified_reads);
}

int main(int argc, char **argv) {
    const char *trace_path = argc > 1 ? argv[1] : "trace.txt";
    FILE *trace = fopen(trace_path, "r");
    if (trace == NULL) {
        perror(trace_path);
        fprintf(stderr, "usage: %s <trace-file>\n", argv[0]);
        return EXIT_FAILURE;
    }

    BufferManager manager;
    BackingStore backing_store;
    BackingStore expected_store;
    buffer_manager_init(&manager);
    backing_store_init(&backing_store);
    backing_store_init(&expected_store);

    char line[MAX_TRACE_LINE];
    uint64_t line_no = 0;
    uint64_t verified_reads = 0;
    while (fgets(line, sizeof(line), trace) != NULL) {
        line_no++;
        char op = 0;
        uint64_t key = 0;
        char *payload = NULL;
        if (!parse_trace_line(line, &op, &key, &payload)) {
            continue;
        }

        bool hit = false;
        if (op == 'R') {
            unsigned char actual[BUFFER_UNIT_SIZE];
            if (!buffer_read(&manager, &backing_store, key, actual, &hit)) {
                fprintf(stderr, "line %" PRIu64 ": read failed\n", line_no);
                goto fail;
            }
            if (!verify_read(&expected_store, key, actual)) {
                fprintf(stderr, "line %" PRIu64 ": verification failed for key=%" PRIu64 "\n", line_no, key);
                goto fail;
            }
            verified_reads++;
            printf("line=%" PRIu64 " op=R key=%" PRIu64 " %s data=", line_no, key, hit ? "hit" : "miss");
            print_data_preview(actual);
            printf("\n");
        } else if (op == 'W') {
            unsigned char data[BUFFER_UNIT_SIZE];
            make_payload(payload, data);
            if (!buffer_write(&manager, &backing_store, key, data, &hit)) {
                fprintf(stderr, "line %" PRIu64 ": write failed\n", line_no);
                goto fail;
            }
            if (!backing_store_write(&expected_store, key, data)) {
                fprintf(stderr, "line %" PRIu64 ": expected-store update failed\n", line_no);
                goto fail;
            }
            printf("line=%" PRIu64 " op=W key=%" PRIu64 " %s data=", line_no, key, hit ? "hit" : "miss");
            print_data_preview(data);
            printf("\n");
        } else {
            fprintf(stderr, "line %" PRIu64 ": unsupported op '%c'\n", line_no, op);
            goto fail;
        }
    }

    if (!buffer_manager_flush_all(&manager, &backing_store)) {
        fprintf(stderr, "final flush failed\n");
        goto fail;
    }

    fclose(trace);
    print_summary(&manager, verified_reads);
    backing_store_free(&backing_store);
    backing_store_free(&expected_store);
    return EXIT_SUCCESS;

fail:
    fclose(trace);
    backing_store_free(&backing_store);
    backing_store_free(&expected_store);
    return EXIT_FAILURE;
}
