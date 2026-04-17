// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6...  1699900000 42 README.md
//   100644 f7e8d9c0b1a2...  1699900100 128 src/main.c
//
// This is intentionally a simple text format. No magic numbers, no
// binary parsing. The focus is on the staging area CONCEPT (tracking
// what will go into the next commit) and ATOMIC WRITES (temp+rename).
//
// PROVIDED functions: index_find, index_remove, index_status
// TODO functions:     index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <inttypes.h>

// Forward declaration for object_write (implemented in object.c)
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// Helper for qsort to ensure the index is always saved in alphabetical order
static int compare_index(const void *a, const void *b) {
    return strcmp(((IndexEntry*)a)->path, ((IndexEntry*)b)->path);
}

// ─── PROVIDED ────────────────────────────────────────────────────────────────

// Find an index entry by path (linear scan).
IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

// Remove a file from the index.
// Returns 0 on success, -1 if path not in index.
int index_remove(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0) {
            int remaining = index->count - i - 1;
            if (remaining > 0)
                memmove(&index->entries[i], &index->entries[i + 1],
                        remaining * sizeof(IndexEntry));
            index->count--;
            return index_save(index);
        }
    }
    fprintf(stderr, "error: '%s' is not in the index\n", path);
    return -1;
}

// Print the status of the working directory.
int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("  staged:     %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("  deleted:    %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec || st.st_size != (off_t)index->entries[i].size) {
                printf("  modified:   %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("  (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0 || strcmp(ent->d_name, "pes") == 0) continue;
            if (strstr(ent->d_name, ".o") != NULL) continue;

            int is_tracked = 0;
            for (int i = 0; i < index->count; i++) {
                if (strcmp(index->entries[i].path, ent->d_name) == 0) {
                    is_tracked = 1; 
                    break;
                }
            }
            if (!is_tracked) {
                struct stat st;
                stat(ent->d_name, &st);
                if (S_ISREG(st.st_mode)) {
                    printf("  untracked:  %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("  (nothing to show)\n");
    printf("\n");
    return 0;
}

// ─── TODO: IMPLEMENTED ───────────────────────────────────────────────────────

// Load the index from .pes/index.
int index_load(Index *index) {
    // CRITICAL: Initialize count to 0 to avoid segmentation faults
    index->count = 0;
    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0; // Not an error; repo is just empty

    char hash_hex[HASH_HEX_SIZE + 1];
    // Read the space-separated fields: <mode> <hash> <mtime> <size> <path>
    while (index->count < MAX_INDEX_ENTRIES && 
           fscanf(f, "%o %64s %" SCNu64 " %u %511s\n", 
                  &index->entries[index->count].mode, 
                  hash_hex, 
                  &index->entries[index->count].mtime_sec, 
                  &index->entries[index->count].size, 
                  index->entries[index->count].path) == 5) {
        // Convert hex string to binary ObjectID
        hex_to_hash(hash_hex, &index->entries[index->count].hash);
        index->count++;
    }

    fclose(f);
    return 0;
}

// Save the index to .pes/index atomically using temp-file and rename pattern.
int index_save(const Index *index) {
    // Sort entries alphabetically before saving to ensure consistency
    Index temp = *index;
    qsort(temp.entries, temp.count, sizeof(IndexEntry), compare_index);

    char tmp_path[512];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);
    FILE *f = fopen(tmp_path, "w");
    if (!f) return -1;

    for (int i = 0; i < temp.count; i++) {
        char hex[HASH_HEX_SIZE + 1];
        hash_to_hex(&temp.entries[i].hash, hex);
        fprintf(f, "%o %s %" PRIu64 " %u %s\n", 
                temp.entries[i].mode, hex, 
                temp.entries[i].mtime_sec, 
                temp.entries[i].size, 
                temp.entries[i].path);
    }

    // Atomic write pattern: flush, sync to disk, then rename
    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return rename(tmp_path, INDEX_FILE);
}

// Stage a file: store as blob, update index entry with metadata.
int index_add(Index *index, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return -1;

    // 1. Read file and store content as a OBJ_BLOB
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    
    void *data = malloc(st.st_size ? st.st_size : 1);
    if (!data) { fclose(f); return -1; }
    fread(data, 1, st.st_size, f);
    fclose(f);

    ObjectID blob_id;
    if (object_write(OBJ_BLOB, data, st.st_size, &blob_id) != 0) {
        free(data);
