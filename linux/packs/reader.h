#ifndef MATON_OSTREE_PACK_READER_H
#define MATON_OSTREE_PACK_READER_H
#include <stddef.h>
#include <stdint.h>
typedef struct mtp_index mtp_index;
typedef struct {
    const char *path, *checksum, *pack, *kind, *target;
    uint64_t offset, length;
    uint32_t uid, gid, mode;
    const unsigned char *xattrs;
    uint32_t xattrs_length;
} mtp_object;
int mtp_index_open(const char *filename, mtp_index **out);
void mtp_index_close(mtp_index *index);
size_t mtp_index_count(const mtp_index *index);
int mtp_lookup(const mtp_index *index, const char *path, mtp_object *out);
/* Stat-style path lookup; fills kind, mode, uid, gid and payload/link fields. */
int mtp_stat(const mtp_index *index, const char *path, mtp_object *out);
/* Return the ordinal of the next immediate child; cursor starts at zero. */
int mtp_readdir(const mtp_index *index, const char *directory, size_t *cursor,
                const char **name, mtp_object *object);
/* Validate and normalize an index path. Accepts an absolute virtual path or
 * a path relative to the pack root; returns 0 on success. */
int mtp_normalize_path(const char *input, char *output, size_t output_size);
#endif
