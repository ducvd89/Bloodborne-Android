/* SPDX-License-Identifier: GPL-2.0-or-later
 * Atlas publisher (src/runtime_atlas.c): a synthetic image with the 1.09 fingerprints, a player chain,
 * a quick-menu manager and an event-flag tree in the MSVC std::map layout, read through the
 * pipe; then the whole thread against a temporary shared file. */
#define _GNU_SOURCE
#include "../src/runtime_atlas.c"
#include <assert.h>
#include <stdint.h>

#ifndef _WIN32
enum { ImageSize = 0x5562900, Groups = 400, Stride = 128 };
static unsigned char *image;
static unsigned char *chr_obj, *transform_obj, *menu_obj, *manager;

static void poke64(uintptr_t at, uint64_t v) { memcpy((void *)at, &v, 8); }
static void poke32(uintptr_t at, uint32_t v) { memcpy((void *)at, &v, 4); }
static void pokef(uintptr_t at, float v) { memcpy((void *)at, &v, 4); }

/* Flag state of the synthetic game: a function of the id. */
static int truth(uint32_t id) { return (int)(((id * 2654435761u) >> 13) & 1); }
static unsigned char *group_bytes[Groups];
static unsigned char *pool;

static unsigned char *new_node(uint32_t key, unsigned char *sentinel) {
    unsigned char *n = calloc(1, 0x40);
    poke64((uintptr_t)n, (uintptr_t)sentinel);
    poke64((uintptr_t)n + 8, (uintptr_t)sentinel);
    poke64((uintptr_t)n + 0x10, (uintptr_t)sentinel);
    n[0x19] = 0;
    poke32((uintptr_t)n + 0x20, key);
    return n;
}
/* Balanced tree over the groups with an even number (odd ones are missing); the group g's bits follow truth(). */
static unsigned char *build(int lo, int hi, unsigned char *sentinel, unsigned char *parent) {
    while (lo <= hi && (lo & 1)) ++lo;
    if (lo > hi) return sentinel;
    int mid = (lo + hi) / 2;
    if (mid & 1) --mid;
    if (mid < lo) return sentinel;
    unsigned char *n = new_node((uint32_t)mid, sentinel);
    if (mid % 4 == 0) { /* kind 2: its own bytes */
        group_bytes[mid] = calloc(1, Stride);
        poke32((uintptr_t)n + 0x28, 2);
        poke64((uintptr_t)n + 0x30, (uintptr_t)group_bytes[mid]);
    } else { /* kind 1: a slot of the pool */
        group_bytes[mid] = pool + (size_t)Stride * mid;
        poke32((uintptr_t)n + 0x28, 1);
        poke32((uintptr_t)n + 0x30, (uint32_t)mid);
    }
    for (uint32_t b = 0; b < 1000; ++b)
        if (truth((uint32_t)mid * 1000 + b)) group_bytes[mid][b >> 3] |= (unsigned char)(0x80u >> (b & 7));
    poke64((uintptr_t)n + 8, (uintptr_t)(parent ? parent : sentinel));
    unsigned char *l = build(lo, mid - 1, sentinel, n), *r = build(mid + 1, hi, sentinel, n);
    poke64((uintptr_t)n, (uintptr_t)l);
    poke64((uintptr_t)n + 0x10, (uintptr_t)r);
    return n;
}
static void make_image(int short_sentinel) {
    image = mmap(NULL, ImageSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    assert(image != MAP_FAILED);
    memcpy(image + 0x18db3c0, build_guard, sizeof(build_guard));
    memcpy(image + 0x176b89b, menu_guard, sizeof(menu_guard));
    memcpy(image + 0x1bdf835, focus_guard, sizeof(focus_guard));
    memcpy(image + 0x13cfc00, flag_guard, sizeof(flag_guard));
    unsigned char *world = calloc(1, 0x100);
    chr_obj = calloc(1, 0x400);
    unsigned char *modules = calloc(1, 0x100);
    transform_obj = calloc(1, 0x200);
    poke64((uintptr_t)image + 0x553e878, (uintptr_t)world);
    poke64((uintptr_t)world + 0x60, (uintptr_t)chr_obj);
    poke64((uintptr_t)chr_obj + 0x288, (uintptr_t)world); /* any nonzero pointer */
    poke64((uintptr_t)chr_obj + 0x3b0, (uintptr_t)modules);
    poke64((uintptr_t)modules + 0x68, (uintptr_t)transform_obj);
    pokef((uintptr_t)transform_obj + 0x1d4, 1.5f);
    pokef((uintptr_t)transform_obj + 0x1e0, -12.25f);
    pokef((uintptr_t)transform_obj + 0x1e4, 3.0f);
    pokef((uintptr_t)transform_obj + 0x1e8, 100.5f);
    poke32((uintptr_t)chr_obj + 0x3f8, 0x18000000u | 0x00030000u); /* area 24 */
    menu_obj = calloc(1, 0x100);
    poke64((uintptr_t)image + 0x5562878, (uintptr_t)menu_obj);
    poke32((uintptr_t)menu_obj + 0x54, 2);
    manager = calloc(1, 0x100);
    pool = calloc(Groups, Stride);
    poke32((uintptr_t)manager + 0x1c, 1000);
    poke32((uintptr_t)manager + 0x20, Stride);
    poke64((uintptr_t)manager + 0x28, (uintptr_t)pool);
    unsigned char *sentinel;
    if (short_sentinel) { /* 0x20 bytes before an inaccessible page: only the isnil byte can be read */
        unsigned char *page = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        assert(page != MAP_FAILED && !mprotect(page + 4096, 4096, PROT_NONE));
        sentinel = page + 4096 - 0x20;
    } else sentinel = calloc(1, 0x40);
    sentinel[0x19] = 1;
    unsigned char *root = build(0, Groups - 1, sentinel, NULL);
    poke64((uintptr_t)sentinel + 8, (uintptr_t)root);
    poke64((uintptr_t)manager + 0x38, (uintptr_t)sentinel);
    poke64((uintptr_t)image + 0x553b100, (uintptr_t)manager);
}
static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e3 + (double)t.tv_nsec / 1e6;
}
static void check_position(void) {
    Position p;
    read_position(&p);
    assert(p.valid && p.x == -12.25f && p.y == 3.0f && p.z == 100.5f && p.yaw == 1.5f && p.block == 0x18030000u);
    assert(read_menu_open() == 1);
    poke32((uintptr_t)menu_obj + 0x54, 1);
    assert(read_menu_open() == 0);
    poke32((uintptr_t)menu_obj + 0x54, 2);
    poke32((uintptr_t)chr_obj + 0x3f8, 0x05000000u);
    read_position(&p);
    assert(!p.valid && p.x == 0);
    poke32((uintptr_t)chr_obj + 0x3f8, 0x18030000u);
    pokef((uintptr_t)transform_obj + 0x1e0, 30000.0f);
    read_position(&p);
    assert(!p.valid);
    pokef((uintptr_t)transform_obj + 0x1e0, -12.25f);
    read_position(&p);
    assert(p.valid);
}
static void check_flags(const char *label) {
    char text[MaxFlags * 12];
    int n = snprintf(text, sizeof(text), "gen 7\n");
    uint32_t ids[900], count = 0;
    for (uint32_t g = 0; g < Groups && count < 600; ++g) /* two flags in most groups, some in missing ones */
        for (uint32_t b = 0; b < 2; ++b) ids[count++] = g * 1000 + (b ? 999 : (g * 37) % 1000);
    ids[count++] = 400 * 1000 + 5;   /* past the last group: no entry */
    ids[count++] = 100000000u;       /* out of range */
    ids[count++] = 3000;             /* a repeat in request order */
    ids[count++] = 3000;
    for (uint32_t i = 0; i < count; ++i) n += snprintf(text + n, sizeof(text) - (size_t)n, "%u\n", ids[i]);
    n += snprintf(text + n, sizeof(text) - (size_t)n, "junk\n");
    uint32_t gen = 0, parsed = 0;
    assert(parse_request(text, &gen, &parsed) && gen == 7 && parsed == count + 1);
    request_gen = gen;
    flag_count = parsed;
    memcpy(flag_ids, ids, count * sizeof(ids[0]));
    flag_ids[count] = NoGroup;
    for (uint32_t i = 0; i < flag_count; ++i) flag_order[i] = (uint16_t)i;
    qsort(flag_order, flag_count, sizeof(flag_order[0]), flag_cmp);
    read_flags();
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t g = ids[i] / 1000;
        const int expect = ids[i] > 99999999u ? -1 : (g >= Groups || (g & 1)) ? -1 : truth(ids[i]);
        if (flag_values[i] != expect) { printf("flag %u: got %d expected %d\n", ids[i], flag_values[i], expect); abort(); }
    }
    assert(flag_values[count] == -1);
    /* A flag that changes in the game shows up on the next read. */
    const uint32_t id = 4 * 1000 + 17; /* group 4: kind 2 */
    group_bytes[4][17 >> 3] ^= (unsigned char)(0x80u >> (17 & 7));
    uint32_t at = 0;
    while (at < count && ids[at] != id) ++at;
    read_flags();
    if (at < count) assert(flag_values[at] == (truth(id) ^ 1));
    group_bytes[4][17 >> 3] ^= (unsigned char)(0x80u >> (17 & 7));
    /* A manager that moves: every flag reads as unreadable. */
    poke64((uintptr_t)image + 0x553b100, 0);
    read_flags();
    for (uint32_t i = 0; i < flag_count; ++i) assert(flag_values[i] == -1);
    poke64((uintptr_t)image + 0x553b100, (uintptr_t)manager);
    /* Cost of one pass over these 600 flags. */
    enum { Rounds = 200 };
    const double t0 = now_ms();
    for (int i = 0; i < Rounds; ++i) read_flags();
    const double per = (now_ms() - t0) / Rounds;
    Position p;
    const double t1 = now_ms();
    for (int i = 0; i < Rounds * 10; ++i) { read_position(&p); (void)read_menu_open(); }
    printf("atlas %s: %u flags in %.3f ms per pass; position and menu %.4f ms per publish\n", label, count + 1, per,
           (now_ms() - t1) / (Rounds * 10));
}
static void check_file(void) {
    char path[] = "/tmp/bb-atlas-test-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    map = open_file(path);
    assert(map);
    init_file();
    Position p = {1, 2, 3, 4, 0x18000000u, 1};
    flag_count = 3;
    request_gen = 9;
    flag_values[0] = 1; flag_values[1] = 0; flag_values[2] = -1;
    publish(&p, 1);
    publish(&p, 0);
    uint32_t w[16];
    memcpy(w, map, sizeof(w));
    assert(w[0] == 0x54414242u && w[1] == 1 && w[2] == 4 && (w[2] & 1) == 0 && w[3] == 1 && w[8] == 0x18000000u);
    assert(w[9] == 0 && w[10] == 2 && w[11] == 9 && w[12] == 3 && w[13] == 0 && w[14] == 0 && w[15] == 0);
    float x;
    memcpy(&x, &w[4], 4);
    assert(x == 1.0f && (int8_t)map[64] == 1 && (int8_t)map[65] == 0 && (int8_t)map[66] == -1);
    struct stat st;
    assert(!stat(path, &st) && st.st_size == FileSize && (st.st_mode & 0777) == 0600);
    munmap(map, FileSize);
    map = NULL;
    unlink(path);
}
/* The thread end to end: it reads the request file, then the shared file shows the game's values. */
static void check_thread(void) {
    char path[] = "/tmp/bb-atlas-file-XXXXXX", request[] = "/tmp/bb-atlas-req-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    close(fd);
    fd = mkstemp(request);
    assert(fd >= 0);
    assert(write(fd, "gen 3\n3001\n4004\n4005\n", 21) == 21);
    close(fd);
    setenv("BB_ATLAS_FILE", path, 1);
    setenv("BB_ATLAS_FLAGS_FILE", request, 1);
    runtime_atlas_start(image, ImageSize);
    struct timespec pause = {0, 400 * 1000 * 1000};
    nanosleep(&pause, NULL);
    unsigned char *m = open_file(path);
    assert(m);
    uint32_t w[16], s1, s2;
    do { s1 = __atomic_load_n((uint32_t *)(m + 8), __ATOMIC_ACQUIRE); memcpy(w, m, 64); __atomic_thread_fence(__ATOMIC_ACQUIRE); s2 = *(volatile uint32_t *)(m + 8); } while (s1 != s2 || (s1 & 1));
    assert(w[0] == 0x54414242u && w[3] == 1 && w[9] == 1 && w[10] >= 6 && w[10] <= 12 && w[11] == 3 && w[12] == 3);
    assert((int8_t)m[64] == -1 /* group 3 is missing */ && (int8_t)m[65] == truth(4004) && (int8_t)m[66] == truth(4005));
    /* A new request is picked up within about a second. */
    fd = open(request, O_WRONLY | O_TRUNC);
    assert(fd >= 0 && write(fd, "gen 4\n4004\n", 11) == 11);
    close(fd);
    pause.tv_sec = 1; pause.tv_nsec = 400 * 1000 * 1000;
    nanosleep(&pause, NULL);
    assert(*(volatile uint32_t *)(m + 44) == 4 && *(volatile uint32_t *)(m + 48) == 1 && (int8_t)m[64] == truth(4004));
    unlink(path);
    unlink(request);
}
int main(void) {
    for (int variant = 0; variant < 2; ++variant) {
        make_image(variant);
        assert(!pipe2(probe_pipe, O_NONBLOCK | O_CLOEXEC));
        assert(bind_image(image, ImageSize) && menu_slot && flags_supported);
        check_position();
        check_flags("pipe");
        /* Unmapped memory is refused, not faulted on. */
        unsigned char b;
        assert(!read_mem(0x1000, &b, 1) && !read_mem(0x10000, &b, 1) && !read_mem(UINTPTR_MAX - 2, &b, 4));
        /* A changed fingerprint disables everything. */
        image[0x18db3c0 + 20] ^= 1;
        assert(!bind_image(image, ImageSize) && !world_slot);
        image[0x18db3c0 + 20] ^= 1;
        assert(!bind_image(image, 0x553e000));
        image[0x13cfc00] ^= 1; /* only the flag lookup differs: position still works, flags do not */
        assert(bind_image(image, ImageSize) && !flags_supported && menu_slot);
        read_flags();
        image[0x13cfc00] ^= 1;
        if (variant == 0) {
            close(probe_pipe[0]); close(probe_pipe[1]);
            probe_pipe[0] = probe_pipe[1] = -1;
        }
    }
    check_file();
    check_thread();
    puts("atlas tests passed");
    return 0;
}
#else
int main(void) { return 0; }
#endif
