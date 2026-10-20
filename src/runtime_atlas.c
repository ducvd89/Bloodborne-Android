/* SPDX-License-Identifier: GPL-2.0-or-later
 * Player position and event-flag publisher for the Android app's map (read only).
 * Ported from Hunter's Atlas by MikeTat1988 (native/src/runtime_map.c, runtime_atlas_state.c and
 * map_build_guard.inc): the image offsets, the executable fingerprints, the pointer chains and the
 * range checks are theirs, for the 1.09 eboot (sha256 d65f0b4f...). The travel feature is not ported.
 *
 * BB_ATLAS_FILE: an 8192-byte shared file (layout below, seqlock) rewritten 20 times a second.
 * BB_ATLAS_FLAGS_FILE: the app's request, "gen <N>" then one decimal flag id per line.
 * Nothing is written to the game and no game function is called. Every read of the game's memory is
 * safe: a write() of the range into a pipe (an unmapped range gives EFAULT, never a fault). No
 * signal handler is touched: FEX owns this process's signals.
 *
 * File layout (little endian):
 *    0 u32 magic 0x54414242 ("BBAT")      4 u32 version = 1
 *    8 u32 seq (odd while being written)  12 u32 valid (position valid)
 *   16 f32 x   20 f32 y   24 f32 z   28 f32 yaw   32 u32 block
 *   36 u32 menu_open   40 u32 tick   44 u32 flags_gen   48 u32 flag_count   52..63 reserved
 *   64.. i8 flags[min(flag_count, 8128)]: -1 unreadable, 0 off, 1 on, in request order */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#ifndef _WIN32
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>

enum { FileSize = 8192, FlagsAt = 64, MaxFlags = FileSize - FlagsAt, FlagBytes = 125, NoGroup = 0xffffffffu };
enum { PublishNs = 50 * 1000 * 1000, FlagEvery = 5, ReloadEvery = 20 };

/* Reads of the game's memory. */
static int probe_pipe[2] = {-1, -1};
static int pipe_read(uintptr_t at, void *out, size_t size) {
    if (size > 4096) return 0;
    const ssize_t put = write(probe_pipe[1], (const void *)at, size);
    if (put < 0) return 0;
    size_t got = 0;
    while (got < (size_t)put) { /* drain what went in, even a short write */
        const ssize_t n = read(probe_pipe[0], (unsigned char *)out + got, (size_t)put - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    return got == size;
}
static int read_mem(uintptr_t at, void *out, size_t size) {
    if (at < 0x10000 || size > 4096 || at + size < at) return 0;
    return pipe_read(at, out, size);
}
static uintptr_t ptr(uintptr_t at) {
    uintptr_t p = 0;
    return read_mem(at, &p, sizeof(p)) ? p : 0;
}
static uint32_t le32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t le64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* The image: eboot offsets of the 1.09 build. */
static uintptr_t base;
static uintptr_t world_slot, menu_slot, flag_slot;
static int flags_supported;

static int guard(uint64_t size, uint64_t at, const unsigned char *bytes, size_t n) {
    unsigned char got[64];
    return n <= sizeof(got) && at <= size && n <= size - at && read_mem(base + at, got, n) && !memcmp(got, bytes, n);
}
/* map_build_guard.inc: an image byte fingerprint of the executable (its offset 0x18db3c0). */
static const unsigned char build_guard[] = {0x55,0x48,0x89,0xe5,0x41,0x57,0x41,0x56,0x41,0x55,0x41,0x54,0x53,0x48,0x83,0xec,0x38,0x89,0x55,0xd4,0x41,0x89,0xf7,0x48,0x89,0xfb,0x4c,0x8d,0x35,0x97,0x34,0xc6,0x03,0x49,0x8b,0x0e,0x48,0x85,0xc9,0x75,0x24,0x48,0x8d,0x3d,0x79,0xff,0x05,0x03,0x48,0x8d,0x15,0xc6,0xff,0x05,0x03,0x48,0x8d,0x0d,0x6d,0xfe,0x05,0x03,0xbe,0xb1};
/* The game's IsShowMenu predicate, the grid's selected-index read (runtime_map.c), and the event
 * flag lookup (runtime_atlas_state.c). */
static const unsigned char menu_guard[] = {0x48,0x8b,0x05,0xd6,0x6f,0xdf,0x03,0x83,0x78,0x3c,0x00};
static const unsigned char focus_guard[] = {0x8b,0x8b,0xe8,0x0a,0,0,0x85,0xc9,0x79,0x06,0x8b,0x8b,0xe4,0x0a,0,0};
static const unsigned char flag_guard[] = {0x53,0x49,0x89,0xd0,0x8b,0x4f,0x1c,0x31,0xd2,0x89,0xf0,0xf7,0xf1,0x41,0x89,0xc2,0x44,0x0f,0xaf,0xd1,0x48,0x8b,0x5f,0x38};

/* 1 when the position reader is usable (the image is the supported executable). */
static int bind_image(void *image, uint64_t size) {
    base = (uintptr_t)image;
    world_slot = menu_slot = flag_slot = 0;
    flags_supported = 0;
    if (!image || size < 0x553e880) return 0;
    if (!guard(size, 0x18db3c0, build_guard, sizeof(build_guard))) return 0;
    world_slot = base + 0x553e878;
    if (size > 0x5562880 && guard(size, 0x176b89b, menu_guard, sizeof(menu_guard)) &&
        guard(size, 0x1bdf835, focus_guard, sizeof(focus_guard)))
        menu_slot = base + 0x5562878;
    if (guard(size, 0x13cfc00, flag_guard, sizeof(flag_guard))) {
        flags_supported = 1;
        flag_slot = base + 0x553b100;
    }
    return 1;
}

typedef struct { float x, y, z, yaw; uint32_t block; int valid; } Position;
/* runtime_map_location: the chain world -> player -> modules -> transform, each link re-validated. */
static void read_position(Position *out) {
    memset(out, 0, sizeof(*out));
    uintptr_t world = ptr(world_slot), chr = world ? ptr(world + 0x60) : 0;
    unsigned char c[0x174]; /* chr+0x288 .. chr+0x3fc: the +0x288 pointer, modules at +0x3b0, block at +0x3f8 */
    if (!chr || !read_mem(chr + 0x288, c, sizeof(c))) return;
    const uintptr_t modules = (uintptr_t)le64(c + 0x3b0 - 0x288);
    const uint32_t block = le32(c + 0x3f8 - 0x288);
    uintptr_t transform = modules ? ptr(modules + 0x68) : 0;
    unsigned char t[24]; /* transform+0x1d4: yaw, then +0x1e0: x y z */
    if (!transform || !le64(c) || !read_mem(transform + 0x1d4, t, sizeof(t))) return;
    if (chr != ptr(world + 0x60) || transform != ptr(modules + 0x68)) return;
    float v[3], yaw;
    memcpy(&yaw, t, 4);
    memcpy(v, t + 12, 12);
    const unsigned area = block >> 24;
    if (area < 21 || area > 36 || block == 0xffffffffu || !isfinite(yaw) ||
        !isfinite(v[0]) || !isfinite(v[1]) || !isfinite(v[2]) ||
        fabsf(v[0]) > 20000 || fabsf(v[1]) > 20000 || fabsf(v[2]) > 20000) return;
    out->x = v[0]; out->y = v[1]; out->z = v[2]; out->yaw = yaw; out->block = block; out->valid = 1;
}
/* runtime_map_menu_open: the quick menu manager's shown state (2). */
static int read_menu_open(void) {
    uintptr_t menu = menu_slot ? ptr(menu_slot) : 0;
    uint32_t value = 0;
    if (!menu || !read_mem(menu + 0x54, &value, sizeof(value))) return 0;
    return value == 2;
}

/* The app's flag request. */
static uint32_t request_gen, flag_count;
static uint32_t flag_ids[MaxFlags];
static uint16_t flag_order[MaxFlags]; /* indices sorted by (group, index): one lookup per group */
static int8_t flag_values[MaxFlags];
static uint32_t group_of(uint32_t id) { return id > 99999999u ? NoGroup : id / 1000; }
static int flag_cmp(const void *a, const void *b) {
    const uint16_t x = *(const uint16_t *)a, y = *(const uint16_t *)b;
    const uint32_t gx = group_of(flag_ids[x]), gy = group_of(flag_ids[y]);
    return gx != gy ? (gx < gy ? -1 : 1) : (x < y ? -1 : x > y);
}
/* Parses the request text; 0 when the header line is not "gen <N>". */
static int parse_request(char *text, uint32_t *gen, uint32_t *count) {
    char *save = NULL, *line = strtok_r(text, "\n", &save);
    if (!line || strncmp(line, "gen", 3) || (line[3] != ' ' && line[3] != '\t')) return 0;
    char *end;
    errno = 0;
    const unsigned long long g = strtoull(line + 3, &end, 10);
    if (end == line + 3 || errno) return 0;
    *gen = (uint32_t)g;
    uint32_t n = 0;
    while (n < MaxFlags && (line = strtok_r(NULL, "\n", &save))) {
        while (*line == ' ' || *line == '\t') ++line;
        if (!*line || *line == '\r') continue;
        const unsigned long long id = strtoull(line, &end, 10);
        flag_ids[n++] = (end == line || id > 99999999ull) ? NoGroup : (uint32_t)id; /* a bad line stays in its slot, unreadable */
    }
    *count = n;
    return 1;
}
typedef struct { int exists; long long sec, nsec, size; } FileSig;
static FileSig sig_of(const char *path) {
    struct stat st;
    FileSig s = {0, 0, 0, 0};
    if (path && !stat(path, &st)) { s.exists = 1; s.sec = st.st_mtim.tv_sec; s.nsec = st.st_mtim.tv_nsec; s.size = st.st_size; }
    return s;
}
static int sig_same(FileSig a, FileSig b) { return a.exists == b.exists && a.sec == b.sec && a.nsec == b.nsec && a.size == b.size; }
/* Loads the request when the file changed (or at the first call). Returns 1 when the request changed. */
static int reload_request(const char *path, FileSig *seen, int first) {
    const FileSig now = sig_of(path);
    if (!first && sig_same(now, *seen)) return 0;
    static char text[MaxFlags * 12 + 256];
    uint32_t gen = 0, count = 0;
    if (now.exists) {
        FILE *f = path ? fopen(path, "r") : NULL;
        if (!f) return 0;
        const size_t n = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        text[n] = 0;
        if (!parse_request(text, &gen, &count)) return 0; /* half-written: look again next second */
    }
    *seen = now;
    request_gen = gen;
    flag_count = count;
    for (uint32_t i = 0; i < count; ++i) flag_order[i] = (uint16_t)i;
    qsort(flag_order, count, sizeof(flag_order[0]), flag_cmp);
    memset(flag_values, 0xff, sizeof(flag_values));
    printf("Atlas: flag request gen %u, %u flags\n", gen, count);
    fflush(stdout);
    return 1;
}

/* runtime_atlas_flag, for a whole request: the manager's tree (a std::map of groups of 1000 flags,
 * MSVC node layout: left, parent, right, color, isnil, key at +0x20, kind +0x28, data +0x30) is walked
 * once per group, the group's bytes are read once, and every flag of the group is taken from them. */
static int group_data(uintptr_t head, uint32_t group, uint32_t stride, uintptr_t pool, uintptr_t *data) {
    unsigned char node_bytes[0x38], found_bytes[0x38];
    uintptr_t node = ptr(head + 8);
    int stopped = 0, have_found = 0;
    for (int depth = 0; depth < 64; ++depth) {
        if (!node) return 0;
        /* The sentinel may be a short allocation: its isnil byte is enough. */
        if (!read_mem(node, node_bytes, sizeof(node_bytes)) && (!read_mem(node, node_bytes, 0x1a) || !node_bytes[0x19])) return 0;
        if (node_bytes[0x19]) { stopped = 1; break; }
        if (le32(node_bytes + 0x20) < group) node = (uintptr_t)le64(node_bytes + 0x10);
        else { memcpy(found_bytes, node_bytes, sizeof(found_bytes)); have_found = 1; node = (uintptr_t)le64(node_bytes); }
    }
    if (!stopped || !have_found || le32(found_bytes + 0x20) != group) return 0;
    const uint32_t kind = le32(found_bytes + 0x28);
    if (kind == 2) *data = (uintptr_t)le64(found_bytes + 0x30);
    else if (kind == 1) {
        if (!pool || stride < 125 || stride > 4096) return 0;
        const uint64_t offset = (uint64_t)stride * le32(found_bytes + 0x30);
        if (offset > UINT32_MAX) return 0;
        *data = pool + (uintptr_t)offset;
    } else return 0;
    return *data != 0;
}
static void read_flags(void) {
    memset(flag_values, 0xff, flag_count);
    if (!flags_supported || !flag_count) return;
    const uintptr_t man = ptr(flag_slot);
    unsigned char m[0x24]; /* man+0x1c: bits per group, +0x20 stride, +0x28 pool, +0x38 tree head */
    if (!man || !read_mem(man + 0x1c, m, sizeof(m)) || le32(m) != 1000) return;
    const uint32_t stride = le32(m + 4);
    const uintptr_t pool = (uintptr_t)le64(m + 0xc), head = (uintptr_t)le64(m + 0x1c);
    if (!head) return;
    for (uint32_t at = 0; at < flag_count;) {
        const uint32_t group = group_of(flag_ids[flag_order[at]]);
        uint32_t end = at;
        while (end < flag_count && group_of(flag_ids[flag_order[end]]) == group) ++end;
        uintptr_t data = 0;
        unsigned char bytes[FlagBytes];
        if (group != NoGroup && group_data(head, group, stride, pool, &data)) {
            const int whole = read_mem(data, bytes, sizeof(bytes));
            for (uint32_t i = at; i < end; ++i) {
                const uint32_t bit = flag_ids[flag_order[i]] % 1000;
                unsigned char value;
                if (whole) value = bytes[bit >> 3];
                else if (!read_mem(data + (bit >> 3), &value, 1)) continue;
                flag_values[flag_order[i]] = (value & (0x80u >> (bit & 7))) ? 1 : 0;
            }
            if (ptr(flag_slot) != man) /* the manager moved while reading: this group is unreadable */
                for (uint32_t i = at; i < end; ++i) flag_values[flag_order[i]] = -1;
        }
        at = end;
    }
}

/* The shared file. */
static unsigned char *map;
static uint32_t file_seq, publish_tick;
static void put32(unsigned at, uint32_t v) { __atomic_store_n((uint32_t *)(map + at), v, __ATOMIC_RELAXED); }
static void putf(unsigned at, float v) { uint32_t u; memcpy(&u, &v, 4); put32(at, u); }
static void publish(const Position *p, int menu_open) {
    const uint32_t n = flag_count < MaxFlags ? flag_count : MaxFlags;
    __atomic_store_n((uint32_t *)(map + 8), file_seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    put32(12, (uint32_t)p->valid);
    putf(16, p->x); putf(20, p->y); putf(24, p->z); putf(28, p->yaw);
    put32(32, p->block);
    put32(36, (uint32_t)menu_open);
    put32(40, ++publish_tick);
    put32(44, request_gen);
    put32(48, n);
    memcpy(map + FlagsAt, flag_values, n);
    __atomic_store_n((uint32_t *)(map + 8), file_seq + 2, __ATOMIC_RELEASE);
    file_seq += 2;
}
static unsigned char *open_file(const char *path) {
    const int fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return NULL;
    struct stat st;
    if (fstat(fd, &st) || (st.st_size < FileSize && ftruncate(fd, FileSize))) { close(fd); return NULL; }
    void *m = mmap(NULL, FileSize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return m == MAP_FAILED ? NULL : m;
}
static void init_file(void) {
    uint32_t old;
    memcpy(&old, map + 8, 4);
    file_seq = old & ~1u; /* a file left by an earlier run keeps its sequence moving forward */
    memset(map, 0, FileSize);
    memcpy(map + 8, &file_seq, 4);
    put32(0, 0x54414242u);
    put32(4, 1);
}
static void next_tick(struct timespec *t) {
    t->tv_nsec += PublishNs;
    if (t->tv_nsec >= 1000000000L) { t->tv_nsec -= 1000000000L; ++t->tv_sec; }
}

typedef struct { void *image; uint64_t size; const char *file, *flags_file; } AtlasArgs;
static void *atlas_thread(void *argument) {
    AtlasArgs args = *(AtlasArgs *)argument;
    free(argument);
    prctl(PR_SET_NAME, "bb-atlas");
    FileSig seen = {0, 0, 0, 0};
    reload_request(args.flags_file, &seen, 1);
    if (!bind_image(args.image, args.size)) {
        /* The fingerprint does not match: no position, every flag unreadable, and the thread ends. */
        Position none;
        memset(&none, 0, sizeof(none));
        memset(flag_values, 0xff, sizeof(flag_values));
        publish(&none, 0);
        puts("Atlas: executable layout mismatch; position and flags disabled");
        fflush(stdout);
        return NULL;
    }
    printf("Atlas: position%s%s published to %s\n", menu_slot ? ", quick menu" : "", flags_supported ? " and event flags" : "",
           args.file);
    if (!flags_supported) { puts("Atlas: event flag lookup does not match; flags unreadable"); }
    fflush(stdout);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (uint32_t tick = 0;; ++tick) {
        Position p;
        read_position(&p);
        int refresh = tick % FlagEvery == 0;
        if (tick % ReloadEvery == 0 && tick && reload_request(args.flags_file, &seen, 0)) refresh = 1;
        if (refresh) read_flags();
        publish(&p, read_menu_open());
        next_tick(&next);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > next.tv_sec + 1) next = now; /* far behind (suspended): do not burst */
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
    }
    return NULL;
}

void runtime_atlas_start(void *image, uint64_t size) {
    const char *file = getenv("BB_ATLAS_FILE"), *flags_file = getenv("BB_ATLAS_FLAGS_FILE");
    if (!file || !*file) return;
    map = open_file(file);
    if (!map) { printf("Atlas: cannot open %s\n", file); fflush(stdout); return; }
    init_file();
    if (pipe2(probe_pipe, O_NONBLOCK | O_CLOEXEC)) { puts("Atlas: no safe way to read game memory"); fflush(stdout); return; }
    AtlasArgs *args = malloc(sizeof(*args));
    if (!args) return;
    *args = (AtlasArgs){image, size, file, flags_file && *flags_file ? flags_file : NULL};
    pthread_t thread;
    if (pthread_create(&thread, NULL, atlas_thread, args) == 0) pthread_detach(thread);
    else free(args);
}
#else
void runtime_atlas_start(void *image, uint64_t size) { (void)image; (void)size; }
#endif
