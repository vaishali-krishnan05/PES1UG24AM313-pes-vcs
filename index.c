// index.c — Staging area implementation
//
// Text format of .pes/index (one entry per line, sorted by path):
//
//   <mode-octal> <64-char-hex-hash> <mtime-seconds> <size> <path>
//
// Example:
//   100644 a1b2c3d4e5f6... 1699900000 42 README.md
//   100644 f7e8d9c0b1a2... 1699900100 128 src/main.c
//
// PROVIDED functions: index_find, index_remove, index_status
// IMPLEMENTED functions: index_load, index_save, index_add

#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <inttypes.h>

// Forward declaration from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

IndexEntry* index_find(Index *index, const char *path) {
    for (int i = 0; i < index->count; i++) {
        if (strcmp(index->entries[i].path, path) == 0)
            return &index->entries[i];
    }
    return NULL;
}

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

int index_status(const Index *index) {
    printf("Staged changes:\n");
    int staged_count = 0;
    for (int i = 0; i < index->count; i++) {
        printf("    staged: %s\n", index->entries[i].path);
        staged_count++;
    }
    if (staged_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Unstaged changes:\n");
    int unstaged_count = 0;
    for (int i = 0; i < index->count; i++) {
        struct stat st;
        if (stat(index->entries[i].path, &st) != 0) {
            printf("    deleted: %s\n", index->entries[i].path);
            unstaged_count++;
        } else {
            if (st.st_mtime != (time_t)index->entries[i].mtime_sec ||
                st.st_size  != (off_t)index->entries[i].size) {
                printf("    modified: %s\n", index->entries[i].path);
                unstaged_count++;
            }
        }
    }
    if (unstaged_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    printf("Untracked files:\n");
    int untracked_count = 0;
    DIR *dir = opendir(".");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir)) != NULL) {
            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
            if (strcmp(ent->d_name, ".pes") == 0) continue;
            if (strcmp(ent->d_name, "pes") == 0) continue;
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
                if (stat(ent->d_name, &st) == 0 && S_ISREG(st.st_mode)) {
                    printf("    untracked: %s\n", ent->d_name);
                    untracked_count++;
                }
            }
        }
        closedir(dir);
    }
    if (untracked_count == 0) printf("    (nothing to show)\n");
    printf("\n");

    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Load the index from .pes/index.
// Always zeroes the index first. Missing file = empty index (not an error).
int index_load(Index *index) {
    memset(index, 0, sizeof(Index));

    FILE *f = fopen(INDEX_FILE, "r");
    if (!f) return 0;  // No index file yet — that's fine

    char hex[HASH_HEX_SIZE + 2];
    unsigned int mode;
    unsigned long mtime;
    unsigned int size;
    char path[512];

    while (fscanf(f, "%o %64s %lu %u %511s\n",
                  &mode, hex, &mtime, &size, path) == 5) {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index full\n");
            fclose(f);
            return -1;
        }
        IndexEntry *e = &index->entries[index->count];
        e->mode = (uint32_t)mode;
        if (hex_to_hash(hex, &e->hash) != 0) {
            fprintf(stderr, "error: invalid hash in index: %s\n", hex);
            fclose(f);
            return -1;
        }
        e->mtime_sec = (uint64_t)mtime;
        e->size      = (uint32_t)size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        e->path[sizeof(e->path) - 1] = '\0';
        index->count++;
    }

    fclose(f);
    return 0;
}

// Comparator for qsort
static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const IndexEntry *)a)->path,
                  ((const IndexEntry *)b)->path);
}

// Save the index to .pes/index atomically (temp file + rename).
// KEY FIX: sorted entries are heap-allocated, not put on the stack.
// sizeof(Index) ~ 5.6 MB — two copies on the stack = stack overflow.
int index_save(const Index *index) {
    // Heap-allocate a sorted copy of just the entries array
    IndexEntry *sorted = malloc((size_t)index->count * sizeof(IndexEntry));
    if (!sorted && index->count > 0) return -1;

    memcpy(sorted, index->entries, (size_t)index->count * sizeof(IndexEntry));
    qsort(sorted, (size_t)index->count, sizeof(IndexEntry), entry_cmp);

    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", INDEX_FILE);

    FILE *f = fopen(tmp_path, "w");
    if (!f) { free(sorted); return -1; }

    char hex[HASH_HEX_SIZE + 1];
    for (int i = 0; i < index->count; i++) {
        hash_to_hex(&sorted[i].hash, hex);
        fprintf(f, "%o %s %lu %u %s\n",
                (unsigned int)sorted[i].mode,
                hex,
                (unsigned long)sorted[i].mtime_sec,
                (unsigned int)sorted[i].size,
                sorted[i].path);
    }
    free(sorted);

    if (fflush(f) != 0)     { fclose(f); unlink(tmp_path); return -1; }
    if (fsync(fileno(f)) != 0) { fclose(f); unlink(tmp_path); return -1; }
    fclose(f);

    if (rename(tmp_path, INDEX_FILE) != 0) { unlink(tmp_path); return -1; }
    return 0;
}

// Stage a file for the next commit.
int index_add(Index *index, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        fprintf(stderr, "error: cannot stat '%s': ", path);
        perror("");
        return -1;
    }

    void *contents = NULL;
    size_t file_size = (size_t)st.st_size;

    if (file_size > 0) {
        FILE *f = fopen(path, "rb");
        if (!f) { fprintf(stderr, "error: cannot open '%s'\n", path); return -1; }
        contents = malloc(file_size);
        if (!contents) { fclose(f); return -1; }
        if (fread(contents, 1, file_size, f) != file_size) {
            fprintf(stderr, "error: short read on '%s'\n", path);
            free(contents); fclose(f); return -1;
        }
        fclose(f);
    }

    ObjectID blob_id;
    if (object_write(OBJ_BLOB, contents ? contents : "", file_size, &blob_id) != 0) {
        free(contents);
        fprintf(stderr, "error: failed to write blob for '%s'\n", path);
        return -1;
    }
    free(contents);

    uint32_t mode = (st.st_mode & S_IXUSR) ? 0100755 : 0100644;

    IndexEntry *existing = index_find(index, path);
    if (existing) {
        existing->hash      = blob_id;
        existing->mode      = mode;
        existing->mtime_sec = (uint64_t)st.st_mtime;
        existing->size      = (uint32_t)st.st_size;
    } else {
        if (index->count >= MAX_INDEX_ENTRIES) {
            fprintf(stderr, "error: index full\n");
            return -1;
        }
        IndexEntry *e = &index->entries[index->count];
        memset(e, 0, sizeof(IndexEntry));
        e->hash      = blob_id;
        e->mode      = mode;
        e->mtime_sec = (uint64_t)st.st_mtime;
        e->size      = (uint32_t)st.st_size;
        strncpy(e->path, path, sizeof(e->path) - 1);
        index->count++;
    }

    return index_save(index);
}
