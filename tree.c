// tree.c — Tree object serialization and construction
//
// PROVIDED functions: get_file_mode, tree_parse, tree_serialize
// TODO functions: tree_from_index
//
// Binary tree format (per entry, concatenated with no separators):
//   "<mode-as-ascii-octal> <name>\0<32-byte-binary-hash>"

#include "tree.h"
#include "index.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

// ─── Mode Constants ──────────────────────────────────────────────────────────
#define MODE_FILE 0100644
#define MODE_EXEC 0100755
#define MODE_DIR  0040000

// Forward declarations from object.c
int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out);

// ─── PROVIDED ────────────────────────────────────────────────────────────────

uint32_t get_file_mode(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return 0;
    if (S_ISDIR(st.st_mode)) return MODE_DIR;
    if (st.st_mode & S_IXUSR) return MODE_EXEC;
    return MODE_FILE;
}

int tree_parse(const void *data, size_t len, Tree *tree_out) {
    tree_out->count = 0;
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + len;

    while (ptr < end && tree_out->count < MAX_TREE_ENTRIES) {
        TreeEntry *entry = &tree_out->entries[tree_out->count];

        const uint8_t *space = memchr(ptr, ' ', end - ptr);
        if (!space) return -1;

        char mode_str[16] = {0};
        size_t mode_len = space - ptr;
        if (mode_len >= sizeof(mode_str)) return -1;
        memcpy(mode_str, ptr, mode_len);
        entry->mode = (uint32_t)strtol(mode_str, NULL, 8);
        ptr = space + 1;

        const uint8_t *null_byte = memchr(ptr, '\0', end - ptr);
        if (!null_byte) return -1;
        size_t name_len = null_byte - ptr;
        if (name_len >= sizeof(entry->name)) return -1;
        memcpy(entry->name, ptr, name_len);
        entry->name[name_len] = '\0';
        ptr = null_byte + 1;

        if (ptr + HASH_SIZE > end) return -1;
        memcpy(entry->hash.hash, ptr, HASH_SIZE);
        ptr += HASH_SIZE;

        tree_out->count++;
    }
    return 0;
}

static int compare_tree_entries(const void *a, const void *b) {
    return strcmp(((const TreeEntry *)a)->name, ((const TreeEntry *)b)->name);
}

int tree_serialize(const Tree *tree, void **data_out, size_t *len_out) {
    size_t max_size = (size_t)tree->count * 296;
    uint8_t *buffer = malloc(max_size);
    if (!buffer) return -1;

    Tree sorted_tree = *tree;
    qsort(sorted_tree.entries, (size_t)sorted_tree.count,
          sizeof(TreeEntry), compare_tree_entries);

    size_t offset = 0;
    for (int i = 0; i < sorted_tree.count; i++) {
        const TreeEntry *entry = &sorted_tree.entries[i];
        int written = sprintf((char *)buffer + offset, "%o %s",
                              (unsigned int)entry->mode, entry->name);
        offset += (size_t)written + 1; // +1 for the null byte sprintf writes
        memcpy(buffer + offset, entry->hash.hash, HASH_SIZE);
        offset += HASH_SIZE;
    }

    *data_out = buffer;
    *len_out  = offset;
    return 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

// Recursive helper: build a tree from a slice of index entries that all share
// the same directory prefix at the given depth level.
//
// entries  - pointer into the index entries array
// count    - number of entries in this slice
// prefix   - the directory prefix these entries are under (e.g. "src/")
//            empty string "" for the root level
// id_out   - receives the ObjectID of the written tree object
//
// Returns 0 on success, -1 on error.
static int write_tree_level(const IndexEntry *entries, int count,
                             const char *prefix, ObjectID *id_out) {
    Tree tree;
    memset(&tree, 0, sizeof(tree));

    int i = 0;
    while (i < count) {
        const char *path = entries[i].path;
        size_t prefix_len = strlen(prefix);

        // Strip the shared prefix to get the relative name
        const char *rel = path + prefix_len;

        // Does this entry go into a subdirectory?
        const char *slash = strchr(rel, '/');
        if (slash != NULL) {
            // It's in a subdirectory. Collect all entries with the same subdir.
            size_t dir_name_len = (size_t)(slash - rel);
            char dir_name[256];
            if (dir_name_len >= sizeof(dir_name)) return -1;
            memcpy(dir_name, rel, dir_name_len);
            dir_name[dir_name_len] = '\0';

            // Build the full prefix for this subdirectory
            char sub_prefix[512];
            snprintf(sub_prefix, sizeof(sub_prefix), "%s%s/", prefix, dir_name);

            // Find all entries that belong to this subdirectory
            int j = i;
            while (j < count && strncmp(entries[j].path, sub_prefix, strlen(sub_prefix)) == 0) {
                j++;
            }

            // Recursively build the subtree for this subdirectory
            ObjectID sub_tree_id;
            if (write_tree_level(entries + i, j - i, sub_prefix, &sub_tree_id) != 0)
                return -1;

            // Add a tree entry for this subdirectory
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count];
            te->mode = MODE_DIR;
            te->hash = sub_tree_id;
            strncpy(te->name, dir_name, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            tree.count++;

            i = j; // Skip past all entries consumed by the subtree
        } else {
            // It's a regular file directly at this level
            if (tree.count >= MAX_TREE_ENTRIES) return -1;
            TreeEntry *te = &tree.entries[tree.count];
            te->mode = entries[i].mode;
            te->hash = entries[i].hash;
            strncpy(te->name, rel, sizeof(te->name) - 1);
            te->name[sizeof(te->name) - 1] = '\0';
            tree.count++;
            i++;
        }
    }

    // Serialize and write this tree object
    void *raw = NULL;
    size_t raw_len = 0;
    if (tree_serialize(&tree, &raw, &raw_len) != 0) return -1;

    int rc = object_write(OBJ_TREE, raw, raw_len, id_out);
    free(raw);
    return rc;
}

// Build a tree hierarchy from the current index and write all tree objects
// to the object store. Returns the root tree's ObjectID in *id_out.
int tree_from_index(ObjectID *id_out) {
    // Load the current index
    Index index;
    if (index_load(&index) != 0) {
        fprintf(stderr, "error: tree_from_index: failed to load index\n");
        return -1;
    }

    if (index.count == 0) {
        fprintf(stderr, "error: tree_from_index: index is empty\n");
        return -1;
    }

    // Build the root tree from all index entries (prefix = "" = root)
    return write_tree_level(index.entries, index.count, "", id_out);
}
