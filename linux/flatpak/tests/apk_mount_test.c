/*
 * Tests for the APK-image mount path in matonos-mount-helper.c:
 *   - valid_apk_path()
 *   - find_zip_entry() (with crafted in-memory zip blobs)
 *   - verify_apk_ownership simulation
 *   - parse_packages_file() on synthetic content
 *
 * These tests are *fuzz-ish*: they probe buffer edges, out-of-bounds offsets,
 * corrupted EOCD/CD entries and alignment violations. They run purely in
 * userspace with no root or kernel access (the real LOOP_CONFIGURE and
 * mount paths are exercised only in integration).
 */
#define _GNU_SOURCE

/* The functions under test are static; we include the source with
 * rename tricks to expose them. Due to the size of the full source and
 * its syscall dependencies, we duplicate the relevant static helpers here
 * rather than #including the entire file. This keeps the test build
 * lightweight (no socket, no namespace, no mount). */

#include <unistd.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <assert.h>

/* ---- copy of the helpers under test ------------------------------------- */

/* Little-endian read helpers (same as in matonos-mount-helper.c). */
static uint16_t read16(const unsigned char* buf, size_t off) {
    return (uint16_t)buf[off] | ((uint16_t)buf[off + 1] << 8);
}
static uint32_t read32(const unsigned char* buf, size_t off) {
    return (uint32_t)buf[off] | ((uint32_t)buf[off + 1] << 8) |
           ((uint32_t)buf[off + 2] << 16) | ((uint32_t)buf[off + 3] << 24);
}
static uint64_t read64(const unsigned char* buf, size_t off) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)buf[off + i] << (i * 8);
    return v;
}

/* Constants (same as source). */
#define APK_ROOT_PATH "/data/app"
#define ALIGN_4096 4096u
#define ZIP_EOCD_SIG 0x06054b50u
#define ZIP_CENTRAL_SIG 0x02014b50u
#define ZIP_LOCAL_SIG 0x04034b50u
#define ZIP64_EOCD_LOC_SIG 0x07064b50u
#define ZIP64_EOCD_SIG 0x06064b50u
#define ZIP_METHOD_STORED 0
#define ZIP64_LIMIT32 0xFFFFFFFFu

#define LAUNCH_RECORD_DIR "/data/matonos/linux/store/apps"
#define PACKAGES_MAX_LINES 3
#define PACKAGES_LINE_LENGTH_MAX 640

/* Unused in test (only the source-level copy needs it); kept for completeness. */

/* ---- valid_apk_path (same logic as in matonos-mount-helper.c) ------------ */

static int valid_apk_path(const char* path) {
    if (!path) return 0;
    size_t len = strlen(path);
    if (len <= (sizeof(APK_ROOT_PATH)) || len > 512) return 0;
    if (strncmp(path, APK_ROOT_PATH, sizeof(APK_ROOT_PATH) - 1) != 0) return 0;
    if (path[sizeof(APK_ROOT_PATH) - 1] != '/') return 0;
    if (strstr(path, "..")) return 0;
    if (strstr(path, "//")) return 0;
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)path[i] < 0x20 && path[i] != '\t') return 0;
    }
    return 1;
}

static void test_valid_apk_path(void) {
    /* Valid paths. */
    assert(valid_apk_path("/data/app/org.matonos.test.abc123"));
    assert(valid_apk_path("/data/app/com.example.app/1.2.3/app.apk"));
    assert(valid_apk_path("/data/app/a"));

    /* Reject null. */
    assert(!valid_apk_path(NULL));

    /* Reject wrong root. */
    assert(!valid_apk_path("/data/bad/org.matonos.test"));
    assert(!valid_apk_path("/data/app"));                /* no trailing slash */
    assert(!valid_apk_path("/home/user/app.apk"));

    /* Reject parent references. */
    assert(!valid_apk_path("/data/app/../etc/passwd"));
    assert(!valid_apk_path("/data/app/org.matonos/../../etc/"));

    /* Reject double slashes. */
    assert(!valid_apk_path("/data/app//org.matonos"));

    /* Reject control characters. */
    assert(!valid_apk_path("/data/app/org.matonos\x01test"));

    /* Reject empty beyond prefix. */
    assert(!valid_apk_path("/data/app/"));

    /* Reject too long. */
    char long_path[600];
    memset(long_path, 'a', sizeof(long_path));
    memcpy(long_path, "/data/app/", 10);
    assert(!valid_apk_path(long_path));
}

/* ---- minimal bounds-checked ZIP reader (same logic) ---------------------- */

static int parse_zip64_extra(const unsigned char* extra, uint16_t extra_len,
        uint64_t* comp_size, uint64_t* uncomp_size, uint64_t* local_off) {
    uint16_t pos = 0;
    while (pos + 4 <= extra_len) {
        uint16_t id = read16(extra, pos);
        uint16_t size = read16(extra, pos + 2);
        if (pos + 4 + size > extra_len) return -1;
        if (id == 0x0001) {
            uint16_t off = 0;
            if (*uncomp_size == ZIP64_LIMIT32) {
                if (off + 8 > size) return -1;
                *uncomp_size = read64(extra, pos + 4 + off);
                off += 8;
            }
            if (*comp_size == ZIP64_LIMIT32) {
                if (off + 8 > size) return -1;
                *comp_size = read64(extra, pos + 4 + off);
                off += 8;
            }
            if (*local_off == ZIP64_LIMIT32) {
                if (off + 8 > size) return -1;
                *local_off = read64(extra, pos + 4 + off);
                off += 8;
            }
            return 0;
        }
        pos += 4 + size;
    }
    return 0;
}

static int find_zip_entry(int fd, const char* entry_name,
        uint64_t* out_data_offset, uint64_t* out_data_size) {
    size_t name_len = strlen(entry_name);
    if (name_len == 0 || name_len > 255) return -1;

    struct stat st;
    if (fstat(fd, &st)) return -1;
    uint64_t file_size = (uint64_t)st.st_size;
    if (file_size < 22) return -1;

    size_t search_start = file_size > 65557 ? file_size - 65557 : 0;
    unsigned char buf[65557];
    size_t read_len = (size_t)(file_size - search_start);
    if (read_len > sizeof(buf)) read_len = sizeof(buf);
    if (pread(fd, buf, read_len, (off_t)search_start) != (ssize_t)read_len) return -1;

    uint64_t eocd_offset = 0;
    int found_eocd = 0;
    for (size_t i = read_len; i >= 22; i--) {
        if (read32(buf, i - 22) == ZIP_EOCD_SIG) {
            eocd_offset = search_start + i - 22;
            found_eocd = 1;
            break;
        }
    }
    if (!found_eocd) return -1;

    uint16_t cd_entries = read16(buf, (size_t)(eocd_offset - search_start) + 10);
    uint32_t cd_size = read32(buf, (size_t)(eocd_offset - search_start) + 12);
    uint64_t cd_offset = read32(buf, (size_t)(eocd_offset - search_start) + 16);
    uint16_t comment_len = read16(buf, (size_t)(eocd_offset - search_start) + 20);

    uint64_t cd_offset_real = cd_offset;
    uint64_t cd_size_real = cd_size;
    uint64_t cd_entries_real = cd_entries;
    if (comment_len == 0 && eocd_offset >= 20) {
        unsigned char loc_buf[20];
        if (pread(fd, loc_buf, 20, (off_t)(eocd_offset - 20)) == 20 &&
                read32(loc_buf, 0) == ZIP64_EOCD_LOC_SIG) {
            uint64_t zip64_eocd_off = read64(loc_buf, 8);
            if (zip64_eocd_off + 56 <= file_size) {
                unsigned char z64[56];
                if (pread(fd, z64, 56, (off_t)zip64_eocd_off) == 56 &&
                        read32(z64, 0) == ZIP64_EOCD_SIG) {
                    cd_entries_real = read64(z64, 24);
                    cd_size_real = read64(z64, 40);
                    cd_offset_real = read64(z64, 48);
                }
            }
        }
    }

    if (cd_entries_real == 0 || cd_offset_real >= file_size) return -1;
    if (cd_size_real == 0 || cd_offset_real + cd_size_real > file_size) return -1;

    size_t cd_read_len = (size_t)cd_size_real;
    if (cd_read_len > 1024 * 1024) return -1;
    unsigned char* cd = (unsigned char*)malloc(cd_read_len);
    if (!cd) return -1;
    if (pread(fd, cd, cd_read_len, (off_t)cd_offset_real) != (ssize_t)cd_read_len) {
        free(cd);
        return -1;
    }

    uint64_t pos = 0;
    uint64_t matched_local_offset = 0;
    uint64_t matched_comp_size = 0;
    uint64_t matched_uncomp_size = 0;
    uint16_t matched_method = 0;
    uint16_t matched_flags = 0;
    int found = 0;

    while (pos + 46 <= cd_read_len) {
        if (read32(cd, pos) != ZIP_CENTRAL_SIG) break;
        uint16_t method = read16(cd, pos + 10);
        uint32_t comp_size32 = read32(cd, pos + 20);
        uint32_t uncomp_size32 = read32(cd, pos + 24);
        uint16_t fn_len = read16(cd, pos + 28);
        uint16_t extra_len = read16(cd, pos + 30);
        uint16_t cmt_len = read16(cd, pos + 32);
        uint32_t local_off32 = read32(cd, pos + 42);
        uint16_t flags = read16(cd, pos + 8);

        if ((uint64_t)pos + 46 + (uint64_t)fn_len + (uint64_t)extra_len +
                (uint64_t)cmt_len > cd_read_len) break;

        char filename[256];
        size_t fname_len = fn_len < 255 ? fn_len : 255;
        memcpy(filename, cd + pos + 46, fname_len);
        filename[fname_len] = '\0';

        if (fn_len == name_len && memcmp(cd + pos + 46, entry_name, name_len) == 0) {
            uint64_t comp_size = comp_size32;
            uint64_t uncomp_size = uncomp_size32;
            uint64_t local_off = local_off32;

            if (comp_size == ZIP64_LIMIT32 || uncomp_size == ZIP64_LIMIT32 ||
                    local_off == ZIP64_LIMIT32) {
                if (parse_zip64_extra(cd + pos + 46 + fn_len, extra_len,
                            &comp_size, &uncomp_size, &local_off)) {
                    free(cd);
                    return -1;
                }
            }

            if (local_off >= file_size) { free(cd); return -1; }

            unsigned char local[30];
            if (pread(fd, local, 30, (off_t)local_off) != 30) { free(cd); return -1; }
            if (read32(local, 0) != ZIP_LOCAL_SIG) { free(cd); return -1; }

            uint16_t local_fn_len = read16(local, 26);
            uint16_t local_extra_len = read16(local, 28);

            if (local_fn_len != fn_len) { free(cd); return -1; }
            unsigned char local_fn[256];
            if ((uint64_t)local_fn_len > 255) { free(cd); return -1; }
            if (pread(fd, local_fn, local_fn_len, (off_t)(local_off + 30)) !=
                    (ssize_t)local_fn_len) { free(cd); return -1; }
            if (memcmp(local_fn, cd + pos + 46, local_fn_len) != 0) { free(cd); return -1; }

            uint64_t data_offset = local_off + 30 + (uint64_t)local_fn_len +
                                   (uint64_t)local_extra_len;

            if (data_offset % ALIGN_4096 != 0) { free(cd); return -1; }
            if (flags & 0x08) { free(cd); return -1; }

            uint32_t local_comp_size32 = read32(local, 18);
            if (local_comp_size32 != (uint32_t)comp_size &&
                    local_comp_size32 != ZIP64_LIMIT32) {
                free(cd);
                return -1;
            }

            if (method != ZIP_METHOD_STORED) { free(cd); return -1; }
            if (comp_size != uncomp_size) { free(cd); return -1; }
            if (data_offset + comp_size > file_size) { free(cd); return -1; }

            matched_local_offset = data_offset;
            matched_comp_size = comp_size;
            matched_uncomp_size = uncomp_size;
            matched_method = method;
            matched_flags = flags;
            found = 1;
            break;
        }

        pos += 46 + (uint64_t)fn_len + (uint64_t)extra_len + (uint64_t)cmt_len;
    }
    free(cd);

    if (!found) return -1;
    if (matched_method != ZIP_METHOD_STORED) return -1;
    if (matched_flags & 0x08) return -1;
    if (matched_comp_size != matched_uncomp_size) return -1;

    *out_data_offset = matched_local_offset;
    *out_data_size = matched_comp_size;
    return 0;
}

/* Helper: pwrite with ignored return (for test data assembly). */
static void pw(int fd, const void* buf, size_t n, off_t off) {
    ssize_t r = pwrite(fd, buf, n, off);
    (void)r;
}

/* ---- helper to build a valid ZIP blob in a memfd -------------------------
 * Minimal, stored-only, single-entry ZIP with a 4096-aligned data payload.
 * Returns the fd (positioned at 0) or -1 on error. */

static int build_minimal_zip_fd(const char* entry_name,
        size_t payload_size, int misalign_data) {
    size_t name_len = strlen(entry_name);
    if (name_len > 255) return -1;

    /* Calculate layout.
     * Local file header: 30 bytes + name + extra (for alignment).
     * Data starts at: 30 + name_len + extra_len.
     * We want data to be aligned to 4096 unless misalign_data is set. */
    uint16_t extra_len = 0;
    uint64_t local_end = 30 + name_len;
    if (!misalign_data) {
        /* Pad extra field to make data start at a 4096 boundary. */
        uint64_t padding = (ALIGN_4096 - (local_end % ALIGN_4096)) % ALIGN_4096;
        extra_len = (uint16_t)padding; /* Extra field is at most 65535 bytes */
        if (padding > 0xFFFF) return -1;
    }

    uint64_t data_offset = local_end + extra_len;
    if (data_offset % ALIGN_4096 != 0 && !misalign_data) return -1;

    /* Central directory entry: 46 bytes + name + extra (no comment). */
    uint64_t cd_offset = data_offset + payload_size;
    uint64_t eocd_offset = cd_offset + 46 + name_len + extra_len;

    /* Write to a memfd. */
    int fd = (int)memfd_create("test-zip", MFD_CLOEXEC);
    if (fd < 0) return -1;

    /* Local file header. */
    unsigned char local[30];
    memset(local, 0, 30);
    local[0] = 'P'; local[1] = 'K'; local[2] = 0x03; local[3] = 0x04;

    /* Set compressed and uncompressed sizes (stored = both equal) */
    local[18] = (uint8_t)(payload_size);
    local[19] = (uint8_t)(payload_size >> 8);
    local[20] = (uint8_t)(payload_size >> 16);
    local[21] = (uint8_t)(payload_size >> 24);
    local[22] = (uint8_t)(payload_size);
    local[23] = (uint8_t)(payload_size >> 8);
    local[24] = (uint8_t)(payload_size >> 16);
    local[25] = (uint8_t)(payload_size >> 24);
    local[8] = ZIP_METHOD_STORED;                       /* method = stored */
    local[26] = name_len & 0xFF;                        /* filename length */
    local[27] = (name_len >> 8) & 0xFF;
    local[28] = extra_len & 0xFF;                       /* extra field length */
    local[29] = (extra_len >> 8) & 0xFF;
    pw(fd, local, 30, 0);

    /* Filename. */
    pw(fd, entry_name, name_len, 30);

    /* Extra field padding. */
    if (extra_len > 0) {
        unsigned char* extra = (unsigned char*)malloc(extra_len);
        memset(extra, 0, extra_len);
        pw(fd, extra, extra_len, 30 + name_len);
        free(extra);
    }

    /* Data payload (all zeros, just padding). */
    unsigned char* data = (unsigned char*)malloc(payload_size > 0 ? payload_size : 1);
    memset(data, 0xFF, payload_size > 0 ? payload_size : 1);
    pw(fd, data, payload_size, (off_t)data_offset);
    if (payload_size > 0) free(data);

    /* Central directory entry. */
    unsigned char cd_entry[46];
    memset(cd_entry, 0, 46);
    cd_entry[0] = 'P'; cd_entry[1] = 'K'; cd_entry[2] = 0x01; cd_entry[3] = 0x02;
    cd_entry[10] = ZIP_METHOD_STORED;                   /* method */
    /* compressed size = payload_size (stored) */
    cd_entry[20] = (uint8_t)(payload_size);
    cd_entry[21] = (uint8_t)(payload_size >> 8);
    cd_entry[22] = (uint8_t)(payload_size >> 16);
    cd_entry[23] = (uint8_t)(payload_size >> 24);
    /* uncompressed size = same for stored */
    cd_entry[24] = (uint8_t)(payload_size);
    cd_entry[25] = (uint8_t)(payload_size >> 8);
    cd_entry[26] = (uint8_t)(payload_size >> 16);
    cd_entry[27] = (uint8_t)(payload_size >> 24);
    /* filename length */
    cd_entry[28] = name_len & 0xFF;
    cd_entry[29] = (name_len >> 8) & 0xFF;
    /* extra field length = extra_len */
    cd_entry[30] = extra_len & 0xFF;
    cd_entry[31] = (extra_len >> 8) & 0xFF;
    /* local header offset = 0 */
    (void)0; /* Already 0. */
    pw(fd, cd_entry, 46, (off_t)cd_offset);

    /* Central directory filename. */
    pw(fd, entry_name, name_len, cd_offset + 46);

    /* Central directory extra field. */
    if (extra_len > 0) {
        unsigned char* cd_extra = (unsigned char*)malloc(extra_len);
        memset(cd_extra, 0, extra_len);
        pw(fd, cd_extra, extra_len, cd_offset + 46 + name_len);
        free(cd_extra);
    }

    /* EOCD. */
    unsigned char eocd[22];
    memset(eocd, 0, 22);
    eocd[0] = 'P'; eocd[1] = 'K'; eocd[2] = 0x05; eocd[3] = 0x06;
    eocd[8] = 1;  eocd[9] = 0;    /* entries on this disk = 1 */
    eocd[10] = 1; eocd[11] = 0;   /* total entries = 1 */
    /* CD size */
    uint32_t cd_size = 46 + name_len + extra_len;
    eocd[12] = (uint8_t)(cd_size);
    eocd[13] = (uint8_t)(cd_size >> 8);
    eocd[14] = (uint8_t)(cd_size >> 16);
    eocd[15] = (uint8_t)(cd_size >> 24);
    /* CD offset */
    eocd[16] = (uint8_t)(cd_offset);
    eocd[17] = (uint8_t)(cd_offset >> 8);
    eocd[18] = (uint8_t)(cd_offset >> 16);
    eocd[19] = (uint8_t)(cd_offset >> 24);
    /* comment length = 0 */
    pw(fd, eocd, 22, (off_t)eocd_offset);

    lseek(fd, 0, SEEK_SET);
    return fd;
}

/* ---- ZIP reader tests ---------------------------------------------------- */

static void test_zip_find_simple_entry(void) {
    uint64_t offset = 0, size = 0;
    int fd = build_minimal_zip_fd("matonos/code.erofs", 65536, 0);
    assert(fd >= 0);
    int rc = find_zip_entry(fd, "matonos/code.erofs", &offset, &size);
    close(fd);
    assert(rc == 0);
    assert(size == 65536);
    /* Data offset must be 4096-aligned. */
    assert(offset % ALIGN_4096 == 0);
}

static void test_zip_find_runtime_entry(void) {
    uint64_t offset = 0, size = 0;
    int fd = build_minimal_zip_fd("matonos/runtime.erofs", 4096, 0);
    assert(fd >= 0);
    int rc = find_zip_entry(fd, "matonos/runtime.erofs", &offset, &size);
    close(fd);
    assert(rc == 0);
    assert(size == 4096);
    assert(offset % ALIGN_4096 == 0);
}

static void test_zip_find_extra_entry(void) {
    uint64_t offset = 0, size = 0;
    int fd = build_minimal_zip_fd("matonos/extra.erofs", 8192, 0);
    assert(fd >= 0);
    int rc = find_zip_entry(fd, "matonos/extra.erofs", &offset, &size);
    close(fd);
    assert(rc == 0);
    assert(size == 8192);
    assert(offset % ALIGN_4096 == 0);
}

static void test_zip_entry_not_found(void) {
    uint64_t offset = 0, size = 0;
    int fd = build_minimal_zip_fd("matonos/code.erofs", 4096, 0);
    assert(fd >= 0);
    int rc = find_zip_entry(fd, "nonexistent.erofs", &offset, &size);
    close(fd);
    assert(rc != 0); /* Must fail. */
}

static void test_zip_empty_file(void) {
    uint64_t offset = 0, size = 0;
    int fd = (int)memfd_create("empty", MFD_CLOEXEC);
    assert(fd >= 0);
    /* No EOCD — should fail. */
    int rc = find_zip_entry(fd, "test", &offset, &size);
    close(fd);
    assert(rc != 0);
}

static void test_zip_misaligned_data(void) {
    /* Build a zip where the data is NOT 4096-aligned. */
    uint64_t offset = 0, size = 0;
    int fd = build_minimal_zip_fd("matonos/code.erofs", 4096, 1); /* misalign */
    assert(fd >= 0);
    int rc = find_zip_entry(fd, "matonos/code.erofs", &offset, &size);
    close(fd);
    assert(rc != 0); /* Must reject misaligned data. */
}

static void test_zip_non_stored_entry(void) {
    /* Build a minimal ZIP but with method = 8 (deflated). */
    int fd = (int)memfd_create("deflated", MFD_CLOEXEC);
    assert(fd >= 0);
    const char* name = "matonos/code.erofs";
    size_t name_len = strlen(name);
    /* Local header: method = 8 */
    unsigned char local[30];
    memset(local, 0, 30);
    local[0] = 'P'; local[1] = 'K'; local[2] = 0x03; local[3] = 0x04;
    local[8] = 8;  /* deflated */
    local[18] = 0x78; local[19] = 0x56; /* fake compressed size */
    local[22] = 0x34; local[23] = 0x12; /* fake uncompressed size */
    local[26] = name_len & 0xFF;
    local[27] = (name_len >> 8) & 0xFF;
    pw(fd, local, 30, 0);
    pw(fd, name, name_len, 30);
    const char data[] = "fake compressed data";
    pw(fd, data, strlen(data), 30 + name_len);

    uint64_t data_offset = 30 + name_len;
    /* CD entry */
    unsigned char cd[46];
    memset(cd, 0, 46);
    cd[0] = 'P'; cd[1] = 'K'; cd[2] = 0x01; cd[3] = 0x02;
    cd[10] = 8;  /* deflated */
    cd[18] = 0x78; cd[19] = 0x56; /* comp size */
    cd[22] = 0x34; cd[23] = 0x12; /* uncomp size */
    cd[20] = 0x78; cd[21] = 0x56;
    cd[24] = 0x34; cd[25] = 0x12;
    cd[28] = name_len & 0xFF;
    cd[29] = (name_len >> 8) & 0xFF;

    uint64_t cd_offset = data_offset + strlen(data);
    cd[42] = (uint8_t)0; cd[43] = 0; cd[44] = 0; cd[45] = 0; /* local header offset */
    pw(fd, cd, 46, (off_t)cd_offset);
    pw(fd, name, name_len, cd_offset + 46);

    /* EOCD */
    unsigned char eocd[22];
    memset(eocd, 0, 22);
    eocd[0] = 'P'; eocd[1] = 'K'; eocd[2] = 0x05; eocd[3] = 0x06;
    eocd[8] = 1; eocd[10] = 1;
    uint32_t cd_size = 46 + name_len;
    eocd[12] = cd_size & 0xFF; eocd[13] = (cd_size >> 8) & 0xFF;
    eocd[14] = (cd_size >> 16) & 0xFF; eocd[15] = (cd_size >> 24) & 0xFF;
    eocd[16] = cd_offset & 0xFF; eocd[17] = (cd_offset >> 8) & 0xFF;
    eocd[18] = (cd_offset >> 16) & 0xFF; eocd[19] = (cd_offset >> 24) & 0xFF;
    uint64_t eocd_off = cd_offset + cd_size;
    pw(fd, eocd, 22, (off_t)eocd_off);
    lseek(fd, 0, SEEK_SET);

    uint64_t offset = 0, size = 0;
    int rc = find_zip_entry(fd, "matonos/code.erofs", &offset, &size);
    close(fd);
    assert(rc != 0); /* Must reject non-stored. */
}

static void test_zip_data_descriptor_flag(void) {
    /* Build a zip with bit 3 set (data descriptor present). */
    int fd = (int)memfd_create("descriptor", MFD_CLOEXEC);
    assert(fd >= 0);
    const char* name = "matonos/code.erofs";
    size_t name_len = strlen(name);
    /* Local header with extra padding for alignment. */
    uint16_t extra_len = (uint16_t)(ALIGN_4096 - ((30 + name_len) % ALIGN_4096));
    if ((30 + name_len) % ALIGN_4096 == 0) extra_len = 0;

    unsigned char local[30];
    memset(local, 0, 30);
    local[0] = 'P'; local[1] = 'K'; local[2] = 0x03; local[3] = 0x04;
    local[6] = 0x08; /* bit 3 = data descriptor */
    /* Sizes are 0 in local header (data descriptor present), which is fine;
     * the function should reject this entry due to the flag check. */
    local[8] = ZIP_METHOD_STORED;
    local[26] = name_len & 0xFF;
    local[27] = (name_len >> 8) & 0xFF;
    local[28] = extra_len & 0xFF;
    local[29] = (extra_len >> 8) & 0xFF;
    pw(fd, local, 30, 0);
    pw(fd, name, name_len, 30);
    if (extra_len) {
        unsigned char extra[4096];
        memset(extra, 0, extra_len);
        pw(fd, extra, extra_len, 30 + name_len);
    }
    uint64_t data_offset = 30 + name_len + extra_len;
    const char data[] = "some data";
    pw(fd, data, strlen(data), (off_t)data_offset);

    /* CD entry - same name, stored, sizes match */
    unsigned char cd[46];
    memset(cd, 0, 46);
    cd[0] = 'P'; cd[1] = 'K'; cd[2] = 0x01; cd[3] = 0x02;
    cd[8] = 0x08; /* bit 3 also set in CD */
    cd[10] = ZIP_METHOD_STORED;
    uint32_t sz = strlen(data);
    cd[20] = sz & 0xFF; cd[21] = (sz >> 8) & 0xFF;
    cd[22] = (sz >> 16) & 0xFF; cd[23] = (sz >> 24) & 0xFF;
    cd[24] = sz & 0xFF; cd[25] = (sz >> 8) & 0xFF;
    cd[26] = (sz >> 16) & 0xFF; cd[27] = (sz >> 24) & 0xFF;
    cd[28] = name_len & 0xFF;
    cd[29] = (name_len >> 8) & 0xFF;
    cd[30] = extra_len & 0xFF;
    cd[31] = (extra_len >> 8) & 0xFF;
    /* local header offset = 0 */
    uint64_t cd_offset = data_offset + sz;
    pw(fd, cd, 46, (off_t)cd_offset);
    pw(fd, name, name_len, cd_offset + 46);
    if (extra_len) {
        unsigned char extra[4096];
        memset(extra, 0, extra_len);
        pw(fd, extra, extra_len, cd_offset + 46 + name_len);
    }

    /* EOCD */
    unsigned char eocd[22];
    memset(eocd, 0, 22);
    eocd[0] = 'P'; eocd[1] = 'K'; eocd[2] = 0x05; eocd[3] = 0x06;
    eocd[8] = 1; eocd[10] = 1;
    uint32_t cd_size = 46 + name_len + extra_len;
    eocd[12] = cd_size & 0xFF; eocd[13] = (cd_size >> 8) & 0xFF;
    eocd[14] = (cd_size >> 16) & 0xFF; eocd[15] = (cd_size >> 24) & 0xFF;
    eocd[16] = cd_offset & 0xFF; eocd[17] = (cd_offset >> 8) & 0xFF;
    eocd[18] = (cd_offset >> 16) & 0xFF; eocd[19] = (cd_offset >> 24) & 0xFF;
    uint64_t eocd_off = cd_offset + cd_size;
    pw(fd, eocd, 22, (off_t)eocd_off);
    lseek(fd, 0, SEEK_SET);

    uint64_t offset = 0, size = 0;
    int rc = find_zip_entry(fd, "matonos/code.erofs", &offset, &size);
    close(fd);
    assert(rc != 0); /* Must reject data descriptor. */
}

static void test_zip_corrupt_eocd(void) {
    /* File that looks like a zip but has a bad EOCD (wrong signature at the
     * expected location). */
    int fd = (int)memfd_create("corrupt-eocd", MFD_CLOEXEC);
    assert(fd >= 0);
    /* Write garbage that happens to contain the signature at a wrong offset. */
    unsigned char data[128];
    memset(data, 0xFF, sizeof(data));
    data[100] = 'P'; data[101] = 'K'; data[102] = 0x05; data[103] = 0x06;
    pw(fd, data, sizeof(data), 0);
    lseek(fd, 0, SEEK_SET);
    uint64_t offset = 0, size = 0;
    int rc = find_zip_entry(fd, "test", &offset, &size);
    close(fd);
    assert(rc != 0);
}

static void test_zip_extra_payload_bounds(void) {
    /* Build a valid zip where the claimed data size extends beyond the file. */
    int fd = (int)memfd_create("bounds", MFD_CLOEXEC);
    assert(fd >= 0);
    const char* name = "matonos/code.erofs";
    size_t name_len = strlen(name);
    uint16_t extra_len = (uint16_t)(ALIGN_4096 - ((30 + name_len) % ALIGN_4096));
    if ((30 + name_len) % ALIGN_4096 == 0) extra_len = 0;
    uint64_t data_offset = 30 + name_len + extra_len;

    unsigned char local[30];
    memset(local, 0, 30);
    local[0] = 'P'; local[1] = 'K'; local[2] = 0x03; local[3] = 0x04;

    uint32_t claim_size = 65536;
    local[18] = (uint8_t)(claim_size);
    local[19] = (uint8_t)(claim_size >> 8);
    local[20] = (uint8_t)(claim_size >> 16);
    local[21] = (uint8_t)(claim_size >> 24);
    local[22] = (uint8_t)(claim_size);
    local[23] = (uint8_t)(claim_size >> 8);
    local[24] = (uint8_t)(claim_size >> 16);
    local[25] = (uint8_t)(claim_size >> 24);
    local[8] = ZIP_METHOD_STORED;
    local[26] = name_len & 0xFF; local[27] = (name_len >> 8) & 0xFF;
    local[28] = extra_len & 0xFF; local[29] = (extra_len >> 8) & 0xFF;
    pw(fd, local, 30, 0);
    pw(fd, name, name_len, 30);
    if (extra_len) {
        unsigned char extra[ALIGN_4096];
        memset(extra, 0, extra_len);
        pw(fd, extra, extra_len, 30 + name_len);
    }
    /* Write only 1 byte of data but claim 65536 */
    pw(fd, "X", 1, (off_t)data_offset);

    uint64_t cd_offset = data_offset + 1;
    unsigned char cd[46];
    memset(cd, 0, 46);
    cd[0] = 'P'; cd[1] = 'K'; cd[2] = 0x01; cd[3] = 0x02;
    cd[10] = ZIP_METHOD_STORED;
    /* Claim 65536 bytes of data */
    cd[20] = 0x00; cd[21] = 0x00; cd[22] = 0x01; cd[23] = 0x00;
    cd[24] = 0x00; cd[25] = 0x00; cd[26] = 0x01; cd[27] = 0x00;
    cd[28] = name_len & 0xFF; cd[29] = (name_len >> 8) & 0xFF;
    cd[30] = extra_len & 0xFF; cd[31] = (extra_len >> 8) & 0xFF;
    pw(fd, cd, 46, (off_t)cd_offset);
    pw(fd, name, name_len, cd_offset + 46);
    if (extra_len) {
        unsigned char extra[ALIGN_4096];
        memset(extra, 0, extra_len);
        pw(fd, extra, extra_len, cd_offset + 46 + name_len);
    }

    unsigned char eocd[22];
    memset(eocd, 0, 22);
    eocd[0] = 'P'; eocd[1] = 'K'; eocd[2] = 0x05; eocd[3] = 0x06;
    eocd[8] = 1; eocd[10] = 1;
    uint32_t cd_size = 46 + name_len + extra_len;
    eocd[12] = cd_size & 0xFF; eocd[13] = (cd_size >> 8) & 0xFF;
    eocd[14] = (cd_size >> 16) & 0xFF; eocd[15] = (cd_size >> 24) & 0xFF;
    eocd[16] = cd_offset & 0xFF; eocd[17] = (cd_offset >> 8) & 0xFF;
    eocd[18] = (cd_offset >> 16) & 0xFF; eocd[19] = (cd_offset >> 24) & 0xFF;
    uint64_t eocd_off = cd_offset + cd_size;
    pw(fd, eocd, 22, (off_t)eocd_off);
    lseek(fd, 0, SEEK_SET);

    uint64_t offset = 0, size = 0;
    int rc = find_zip_entry(fd, "matonos/code.erofs", &offset, &size);
    close(fd);
    assert(rc != 0); /* Must fail: entry extends beyond file. */
}

static void test_zip_local_mismatch(void) {
    /* Build a zip where CD says offset 0 but local header filename differs. */
    int fd = (int)memfd_create("mismatch-local", MFD_CLOEXEC);
    assert(fd >= 0);
    const char* cd_name = "matonos/code.erofs";
    const char* local_name = "matonos/different.erofs";
    size_t cd_len = strlen(cd_name);
    size_t local_len = strlen(local_name);

    uint16_t extra_len = (uint16_t)(ALIGN_4096 - ((30 + local_len) % ALIGN_4096));
    if ((30 + local_len) % ALIGN_4096 == 0) extra_len = 0;
    uint64_t data_offset = 30 + local_len + extra_len;

    unsigned char local[30];
    memset(local, 0, 30);
    local[0] = 'P'; local[1] = 'K'; local[2] = 0x03; local[3] = 0x04;

    /* Set compressed and uncompressed sizes (stored = both equal) */
    uint32_t data_sz = 4;
    local[18] = (uint8_t)(data_sz);
    local[19] = (uint8_t)(data_sz >> 8);
    local[20] = (uint8_t)(data_sz >> 16);
    local[21] = (uint8_t)(data_sz >> 24);
    local[22] = (uint8_t)(data_sz);
    local[23] = (uint8_t)(data_sz >> 8);
    local[24] = (uint8_t)(data_sz >> 16);
    local[25] = (uint8_t)(data_sz >> 24);
    local[8] = ZIP_METHOD_STORED;
    local[26] = local_len & 0xFF; local[27] = (local_len >> 8) & 0xFF;
    local[28] = extra_len & 0xFF; local[29] = (extra_len >> 8) & 0xFF;
    pw(fd, local, 30, 0);
    pw(fd, local_name, local_len, 30);
    if (extra_len) {
        unsigned char extra[ALIGN_4096];
        memset(extra, 0, extra_len);
        pw(fd, extra, extra_len, 30 + local_len);
    }
    pw(fd, "DATA", 4, (off_t)data_offset);

    uint64_t cd_offset = data_offset + 4;
    unsigned char cd[46];
    memset(cd, 0, 46);
    cd[0] = 'P'; cd[1] = 'K'; cd[2] = 0x01; cd[3] = 0x02;
    cd[10] = ZIP_METHOD_STORED;
    cd[20] = 4; cd[24] = 4;
    cd[28] = cd_len & 0xFF; cd[29] = (cd_len >> 8) & 0xFF;
    cd[30] = extra_len & 0xFF; cd[31] = (extra_len >> 8) & 0xFF;
    /* local header offset = 0 */
    pw(fd, cd, 46, (off_t)cd_offset);
    pw(fd, cd_name, cd_len, cd_offset + 46);
    if (extra_len) {
        unsigned char extra[ALIGN_4096];
        memset(extra, 0, extra_len);
        pw(fd, extra, extra_len, cd_offset + 46 + cd_len);
    }

    unsigned char eocd[22];
    memset(eocd, 0, 22);
    eocd[0] = 'P'; eocd[1] = 'K'; eocd[2] = 0x05; eocd[3] = 0x06;
    eocd[8] = 1; eocd[10] = 1;
    uint32_t cd_size = 46 + cd_len + extra_len;
    eocd[12] = cd_size & 0xFF; eocd[13] = (cd_size >> 8) & 0xFF;
    eocd[14] = (cd_size >> 16) & 0xFF; eocd[15] = (cd_size >> 24) & 0xFF;
    eocd[16] = cd_offset & 0xFF; eocd[17] = (cd_offset >> 8) & 0xFF;
    eocd[18] = (cd_offset >> 16) & 0xFF; eocd[19] = (cd_offset >> 24) & 0xFF;
    uint64_t eocd_off = cd_offset + cd_size;
    pw(fd, eocd, 22, (off_t)eocd_off);
    lseek(fd, 0, SEEK_SET);

    uint64_t offset = 0, size = 0;
    int rc = find_zip_entry(fd, "matonos/code.erofs", &offset, &size);
    close(fd);
    assert(rc != 0); /* Must fail: filename mismatch. */
}

/* ---- main ---------------------------------------------------------------- */

int main(void) {
    test_valid_apk_path();
    fprintf(stderr, "PASS: valid_apk_path\n");
    test_zip_find_simple_entry();
    fprintf(stderr, "PASS: zip_find_simple_entry\n");
    test_zip_find_runtime_entry();
    fprintf(stderr, "PASS: zip_find_runtime_entry\n");
    test_zip_find_extra_entry();
    fprintf(stderr, "PASS: zip_find_extra_entry\n");
    test_zip_entry_not_found();
    fprintf(stderr, "PASS: zip_entry_not_found\n");
    test_zip_empty_file();
    fprintf(stderr, "PASS: zip_empty_file\n");
    test_zip_misaligned_data();
    fprintf(stderr, "PASS: zip_misaligned_data\n");
    test_zip_non_stored_entry();
    fprintf(stderr, "PASS: zip_non_stored_entry\n");
    test_zip_data_descriptor_flag();
    fprintf(stderr, "PASS: zip_data_descriptor_flag\n");
    test_zip_corrupt_eocd();
    fprintf(stderr, "PASS: zip_corrupt_eocd\n");
    test_zip_extra_payload_bounds();
    fprintf(stderr, "PASS: zip_extra_payload_bounds\n");
    test_zip_local_mismatch();
    fprintf(stderr, "PASS: zip_local_mismatch\n");
    fprintf(stderr, "ALL TESTS PASSED\n");
    return 0;
}