#include "update.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine.h"
#include "net.h"

#define API_LATEST "https://api.github.com/repos/" JD_REPO "/releases/latest"
#define BINARY "jaffedrone"
#define MAX_ZIP (64L << 20)
#define MAX_FILES 512
#define MAX_DIRS 64
#define MAX_PATH 300

static UpdateStatus us;
static char install_dir[1024];
/* "/" and the release file's name, so the name can be matched at the end of a URL */
static const char *const asset_path = "/" JD_ASSET, *const zip_dir = JD_ZIP_DIR;

const char *update_asset(void) { return asset_path + 1; }

UpdateStatus *update_status(void) { return &us; }

static void set_state(int s)
{
    __sync_synchronize(); /* the strings are written before the UI thread sees the new state */
    us.state = s;
}

int version_newer(const char *a, const char *b)
{
    if (*a == 'v' || *a == 'V')
        a++;
    if (*b == 'v' || *b == 'V')
        b++;
    while (*a || *b) {
        char *ea, *eb;
        long x = strtol(a, &ea, 10), y = strtol(b, &eb, 10);
        if (x != y)
            return x > y;
        a = *ea == '.' ? ea + 1 : ea + strlen(ea); /* anything after the numbers ("-rc1") is ignored */
        b = *eb == '.' ? eb + 1 : eb + strlen(eb);
    }
    return 0;
}

/* ------------------------------------------------------- the release */

/* the string value after "key": in a JSON text, from p on; returns where it ends, or NULL */
static const char *json_string(const char *p, const char *key, char *dst, size_t n)
{
    char pat[64];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    dst[0] = 0;
    p = strstr(p, pat);
    if (!p)
        return NULL;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == ':')
        p++;
    if (*p != '"')
        return p;
    size_t j = 0;
    for (p++; *p && *p != '"'; p++) {
        if (*p == '\\' && p[1])
            p++;
        if (j + 1 < n)
            dst[j++] = *p;
    }
    dst[j] = 0;
    return p;
}

int update_check(void)
{
    const char *api = getenv("JD_UPDATE_API"); /* for testing against a local server */
    char *body = NULL, tag[sizeof us.latest] = "", url[sizeof us.url], err[128];
    set_state(UP_CHECKING);
    if (http_fetch(api ? api : API_LATEST, &body, 1L << 20, err, sizeof err) < 0) {
        /* GitHub answers 404 for a private repo as for a missing one */
        snprintf(us.err, sizeof us.err, "update check: %s%s", err,
                 strstr(err, " 404") ? " (" JD_REPO " private, or not there?)" : "");
        set_state(UP_CHECK_FAILED);
        return -1;
    }
    json_string(body, "tag_name", tag, sizeof tag);
    us.url[0] = 0;
    for (const char *p = body; (p = json_string(p, "browser_download_url", url, sizeof url));) {
        size_t l = strlen(url), a = strlen(asset_path);
        if (l >= a && !strcmp(url + l - a, asset_path)) {
            snprintf(us.url, sizeof us.url, "%s", url);
            break;
        }
    }
    free(body);
    if (!tag[0] || !us.url[0]) {
        snprintf(us.err, sizeof us.err, "update check: no %s in the latest release", update_asset());
        set_state(UP_CHECK_FAILED);
        return -1;
    }
    snprintf(us.latest, sizeof us.latest, "%s", tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag);
    set_state(version_newer(us.latest, JD_VERSION) ? UP_AVAILABLE : UP_CURRENT);
    return 0;
}

static long download(const char *url, unsigned char **out, char *err, size_t errlen)
{
    HttpConn *c = calloc(1, sizeof *c);
    *out = NULL;
    if (!c) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    if (http_open(c, url, err, errlen)) {
        free(c);
        return -1;
    }
    long want = c->content_length, cap = want > 0 ? want : 1L << 20, len = 0;
    unsigned char *b = want <= MAX_ZIP ? malloc(cap) : NULL;
    if (!b)
        snprintf(err, errlen, "the download is too big");
    while (b) {
        if (len == cap) {
            unsigned char *nb = cap < MAX_ZIP ? realloc(b, cap * 2) : NULL;
            if (!nb) {
                snprintf(err, errlen, "the download is too big");
                len = -1;
                break;
            }
            b = nb;
            cap *= 2;
        }
        int r = http_read(c, b + len, (int)(cap - len));
        if (r < 0) {
            snprintf(err, errlen, "the download broke off");
            len = -1;
            break;
        }
        if (r == 0)
            break;
        len += r;
        if (want > 0)
            us.progress = (int)(len * 100 / want);
    }
    http_close(c);
    free(c);
    if (b && len >= 0 && want > 0 && len != want) {
        snprintf(err, errlen, "the download broke off");
        len = -1;
    }
    if (!b || len < 0) {
        free(b);
        return -1;
    }
    *out = b;
    return len;
}

int update_install(const char *app_dir)
{
    unsigned char *zip = NULL;
    char err[160];
    us.progress = 0;
    set_state(UP_INSTALLING);
    long n = download(us.url, &zip, err, sizeof err);
    if (n < 0 || update_unzip(zip, n, app_dir, err, sizeof err)) {
        free(zip);
        snprintf(us.err, sizeof us.err, "%s", err);
        set_state(UP_FAILED);
        return -1;
    }
    free(zip);
    sync();
    us.progress = 100;
    set_state(UP_INSTALLED);
    return 0;
}

static void *check_thread(void *arg)
{
    (void)arg;
    update_check();
    return NULL;
}

static void *install_thread(void *arg)
{
    (void)arg;
    update_install(install_dir);
    return NULL;
}

static void start(void *(*fn)(void *), int fail_state)
{
    pthread_t t;
    if (pthread_create(&t, NULL, fn, NULL) == 0)
        pthread_detach(t);
    else {
        snprintf(us.err, sizeof us.err, "couldn't start the updater");
        set_state(fail_state);
    }
}

void update_check_start(void)
{
    if (us.state == UP_CHECKING || us.state == UP_INSTALLING)
        return;
    us.state = UP_CHECKING;
    start(check_thread, UP_CHECK_FAILED);
}

void update_install_start(const char *app_dir)
{
    if (us.state != UP_AVAILABLE && us.state != UP_FAILED)
        return;
    snprintf(install_dir, sizeof install_dir, "%s", app_dir);
    us.progress = 0;
    us.state = UP_INSTALLING;
    start(install_thread, UP_FAILED);
}

/* ----------------------------------------------- inflate (RFC 1951) */

typedef struct {
    const unsigned char *in;
    long inlen, inpos;
    unsigned bitbuf;
    int bitcnt, bad;
    unsigned char *out;
    long outlen, outpos;
} Inflate;

typedef struct {
    short count[16], symbol[288];
} Huffman;

static int bits(Inflate *s, int need)
{
    unsigned long val = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->inpos >= s->inlen) {
            s->bad = 1;
            return 0;
        }
        val |= (unsigned long)s->in[s->inpos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = (unsigned)(val >> need);
    s->bitcnt -= need;
    return (int)(val & ((1UL << need) - 1));
}

static int decode(Inflate *s, const Huffman *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= bits(s, 1);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

/* a canonical Huffman table from code lengths: 0 if complete, >0 if incomplete, <0 if over-subscribed */
static int construct(Huffman *h, const short *length, int n)
{
    short offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++)
        h->count[length[i]]++;
    if (h->count[0] == n)
        return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left = (left << 1) - h->count[len];
        if (left < 0)
            return left;
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++)
        offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; i++)
        if (length[i])
            h->symbol[offs[length[i]]++] = (short)i;
    return left;
}

static int codes(Inflate *s, const Huffman *lencode, const Huffman *distcode)
{
    static const short lbase[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dbase[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                    193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
        int sym = decode(s, lencode);
        if (sym < 0 || s->bad)
            return -1;
        if (sym == 256)
            return 0;
        if (sym < 256) {
            if (s->outpos >= s->outlen)
                return -1;
            s->out[s->outpos++] = (unsigned char)sym;
            continue;
        }
        sym -= 257;
        if (sym >= 29)
            return -1;
        long len = lbase[sym] + bits(s, lext[sym]);
        sym = decode(s, distcode);
        if (sym < 0 || sym >= 30)
            return -1;
        long dist = dbase[sym] + bits(s, dext[sym]);
        if (s->bad || dist > s->outpos || s->outpos + len > s->outlen)
            return -1;
        for (; len > 0; len--, s->outpos++)
            s->out[s->outpos] = s->out[s->outpos - dist];
    }
}

static int stored(Inflate *s)
{
    s->bitbuf = 0;
    s->bitcnt = 0;
    if (s->inpos + 4 > s->inlen)
        return -1;
    const unsigned char *p = s->in + s->inpos;
    long len = p[0] | p[1] << 8;
    if ((p[2] | p[3] << 8) != (~len & 0xffff))
        return -1;
    s->inpos += 4;
    if (s->inpos + len > s->inlen || s->outpos + len > s->outlen)
        return -1;
    memcpy(s->out + s->outpos, s->in + s->inpos, len);
    s->inpos += len;
    s->outpos += len;
    return 0;
}

static int fixed(Inflate *s)
{
    Huffman lencode, distcode;
    short lengths[288];
    int i = 0;
    for (; i < 144; i++)
        lengths[i] = 8;
    for (; i < 256; i++)
        lengths[i] = 9;
    for (; i < 280; i++)
        lengths[i] = 7;
    for (; i < 288; i++)
        lengths[i] = 8;
    construct(&lencode, lengths, 288);
    for (i = 0; i < 30; i++)
        lengths[i] = 5;
    construct(&distcode, lengths, 30);
    return codes(s, &lencode, &distcode);
}

static int dynamic(Inflate *s)
{
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320];
    Huffman lencode, distcode;
    int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
    if (s->bad || nlen > 286 || ndist > 30)
        return -1;
    int i = 0;
    for (; i < ncode; i++)
        lengths[order[i]] = (short)bits(s, 3);
    for (; i < 19; i++)
        lengths[order[i]] = 0;
    if (construct(&lencode, lengths, 19) != 0)
        return -1;
    for (i = 0; i < nlen + ndist;) {
        int sym = decode(s, &lencode);
        if (sym < 0 || s->bad)
            return -1;
        if (sym < 16) {
            lengths[i++] = (short)sym;
            continue;
        }
        short len = 0;
        int rep;
        if (sym == 16) {
            if (i == 0)
                return -1;
            len = lengths[i - 1];
            rep = 3 + bits(s, 2);
        }
        else if (sym == 17)
            rep = 3 + bits(s, 3);
        else
            rep = 11 + bits(s, 7);
        if (i + rep > nlen + ndist)
            return -1;
        while (rep--)
            lengths[i++] = len;
    }
    if (lengths[256] == 0)
        return -1;
    int e = construct(&lencode, lengths, nlen);
    if (e < 0 || (e > 0 && nlen - lencode.count[0] != 1))
        return -1;
    e = construct(&distcode, lengths + nlen, ndist);
    if (e < 0 || (e > 0 && ndist - distcode.count[0] != 1))
        return -1;
    return codes(s, &lencode, &distcode);
}

static int inflate_raw(const unsigned char *in, long inlen, unsigned char *out, long outlen)
{
    Inflate s = {in, inlen, 0, 0, 0, 0, out, outlen, 0};
    int last;
    do {
        last = bits(&s, 1);
        int type = bits(&s, 2);
        if (s.bad)
            return -1;
        int rc = type == 0 ? stored(&s) : type == 1 ? fixed(&s) : type == 2 ? dynamic(&s) : -1;
        if (rc)
            return -1;
    } while (!last);
    return s.outpos == outlen ? 0 : -1;
}

static unsigned long crc32_update(unsigned long c, const unsigned char *p, long n)
{
    static unsigned long table[256];
    if (!table[1])
        for (unsigned long i = 0; i < 256; i++) {
            unsigned long t = i;
            for (int k = 0; k < 8; k++)
                t = t & 1 ? 0xedb88320UL ^ (t >> 1) : t >> 1;
            table[i] = t;
        }
    c ^= 0xffffffffUL;
    while (n--)
        c = table[(c ^ *p++) & 255] ^ (c >> 8);
    return c ^ 0xffffffffUL;
}

/* -------------------------------------------------------------- zip */

static unsigned rd16(const unsigned char *p) { return p[0] | p[1] << 8; }
static unsigned long rd32(const unsigned char *p)
{
    return p[0] | p[1] << 8 | p[2] << 16 | (unsigned long)p[3] << 24;
}

/* files the user owns: an update never writes them */
static int keep_users(const char *rel)
{
    size_t n = strlen(rel);
    return strstr(rel, ".sav") || (n > 4 && (!strcmp(rel + n - 4, ".jds") || !strcmp(rel + n - 4, ".cfg"))) ||
           !strcmp(rel, "log.txt") || !strncmp(rel, "jaffedrone_log.txt", 18) || !strcmp(rel, "sysinfo.txt");
}

/* a path inside the app folder: relative, no "..", no empty parts, at most three deep */
static int safe_rel(const char *rel)
{
    int depth = 0;
    if (!*rel || *rel == '/' || strchr(rel, '\\') || strstr(rel, "//"))
        return 0;
    for (const char *p = rel; *p; p++)
        depth += *p == '/';
    return depth < 3 && strcmp(rel, "..") && strncmp(rel, "../", 3) && !strstr(rel, "/../") &&
           !(strlen(rel) >= 3 && !strcmp(rel + strlen(rel) - 3, "/.."));
}

/* the file at path already holds exactly these bytes (size and CRC-32) */
static int same_file(const char *path, unsigned long size, unsigned long crc)
{
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode) || (unsigned long)st.st_size != size)
        return 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    unsigned char b[16384];
    unsigned long c = 0;
    size_t n;
    while ((n = fread(b, 1, sizeof b, f)) > 0)
        c = crc32_update(c, b, (long)n);
    fclose(f);
    return c == crc;
}

/* the folders above dir/rel; the ones it makes go into made[], to remove if the update fails */
static int make_parents(const char *dir, const char *rel, char (*made)[MAX_PATH], int *nmade)
{
    char path[MAX_PATH + 1100];
    int base = snprintf(path, sizeof path, "%s/", dir);
    snprintf(path + base, sizeof path - base, "%s", rel);
    for (char *p = path + base; (p = strchr(p, '/')); p++) {
        *p = 0;
        int rc = mkdir(path, 0755);
        if (!rc && *nmade < MAX_DIRS)
            snprintf(made[(*nmade)++], MAX_PATH, "%s", path + base);
        *p = '/';
        if (rc && errno != EEXIST)
            return -1;
    }
    return 0;
}

static int write_file(const char *path, const unsigned char *data, long n)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;
    int ok = fwrite(data, 1, n, f) == (size_t)n;
    ok &= fflush(f) == 0;
    ok &= fsync(fileno(f)) == 0;
    ok &= fclose(f) == 0;
    return ok ? 0 : -1;
}

/* Every changed file is written next to its old one as NAME.new and checked; only once all
 * of them are there do they replace the old files, so a failed update leaves the old version
 * whole. Renaming over the running binary or launch.sh is fine: they keep their own copies. */
int update_unzip(const unsigned char *zip, long len, const char *dir, char *err, size_t errlen)
{
    char (*names)[MAX_PATH] = malloc((MAX_FILES + MAX_DIRS) * sizeof *names), (*made)[MAX_PATH] = names + MAX_FILES;
    char path[MAX_PATH + 1100], tmp[MAX_PATH + 1110];
    int nfiles = 0, nmade = 0, rc = -1, have_bin = 0;
    long eocd = -1;
    if (!names) {
        snprintf(err, errlen, "out of memory");
        return -1;
    }
    for (long i = len - 22; i >= 0 && i >= len - 22 - 65535; i--)
        if (rd32(zip + i) == 0x06054b50UL) {
            eocd = i;
            break;
        }
    if (eocd < 0) {
        snprintf(err, errlen, "the download isn't a zip");
        goto out;
    }
    /* offsets are 64-bit so that a bad one can't wrap past the checks on a 32-bit long (the Miyoo's) */
    typedef unsigned long long u64;
    unsigned entries = rd16(zip + eocd + 10);
    u64 cd = rd32(zip + eocd + 16);
    for (unsigned e = 0; e < entries; e++) {
        if (cd + 46 > (u64)len || rd32(zip + cd) != 0x02014b50UL)
            goto damaged;
        const unsigned char *h = zip + cd;
        unsigned method = rd16(h + 10), nlen = rd16(h + 28);
        unsigned long crc = rd32(h + 16), usize = rd32(h + 24);
        u64 csize = rd32(h + 20), lho = rd32(h + 42);
        char name[MAX_PATH + 32];
        if (cd + 46 + nlen > (u64)len || nlen >= sizeof name)
            goto damaged;
        memcpy(name, h + 46, nlen);
        name[nlen] = 0;
        cd += 46 + nlen + rd16(h + 30) + rd16(h + 32);

        const char *rel = name + strlen(zip_dir);
        if (strncmp(name, zip_dir, strlen(zip_dir)) || !*rel || name[nlen - 1] == '/') /* folders */
            continue;
        if (!safe_rel(rel) || strlen(rel) >= MAX_PATH || keep_users(rel))
            continue;
        if (!strcmp(rel, BINARY))
            have_bin = 1;
        snprintf(path, sizeof path, "%s/%s", dir, rel);
        if (same_file(path, usize, crc))
            continue; /* the samples, mostly */
        if (nfiles == MAX_FILES) {
            snprintf(err, errlen, "the zip has too many files");
            goto out;
        }

        if (lho + 30 > (u64)len || rd32(zip + lho) != 0x04034b50UL)
            goto damaged;
        u64 data = lho + 30 + rd16(zip + lho + 26) + rd16(zip + lho + 28);
        if (data + csize > (u64)len || usize > (unsigned long)MAX_ZIP)
            goto damaged;
        unsigned char *buf = malloc(usize ? usize : 1);
        if (!buf) {
            snprintf(err, errlen, "out of memory");
            goto out;
        }
        int bad = method == 0   ? (csize != usize ? -1 : (memcpy(buf, zip + data, usize), 0))
                  : method == 8 ? inflate_raw(zip + data, (long)csize, buf, (long)usize)
                                : -1;
        if (!bad && crc32_update(0, buf, (long)usize) != crc)
            bad = -1;
        if (bad) {
            free(buf);
            snprintf(err, errlen, "couldn't unpack %s", rel);
            goto out;
        }
        snprintf(tmp, sizeof tmp, "%s.new", path);
        if (make_parents(dir, rel, made, &nmade) || write_file(tmp, buf, (long)usize)) {
            free(buf);
            unlink(tmp);
            snprintf(err, errlen, "couldn't write %s (%s)", rel, strerror(errno));
            goto out;
        }
        free(buf);
        snprintf(names[nfiles++], MAX_PATH, "%s", rel);
    }
    if (!have_bin) {
        snprintf(err, errlen, "the zip has no %s" BINARY, zip_dir);
        goto out;
    }
    for (int i = 0; i < nfiles; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, names[i]);
        snprintf(tmp, sizeof tmp, "%s.new", path);
        chmod(tmp, !strcmp(names[i], BINARY) || strstr(names[i], ".sh") ? 0755 : 0644);
        if (rename(tmp, path)) {
            snprintf(err, errlen, "couldn't replace %s (%s)", names[i], strerror(errno));
            goto out; /* rare (the card filled up?): the files already renamed are new, the rest still old */
        }
        names[i][0] = 0;
    }
    rc = 0;
    goto out;
damaged:
    snprintf(err, errlen, "the zip is damaged");
out:
    for (int i = 0; i < nfiles; i++)
        if (names[i][0]) {
            snprintf(tmp, sizeof tmp, "%s/%s.new", dir, names[i]);
            unlink(tmp);
        }
    for (int i = nmade - 1; i >= 0 && rc; i--) { /* the new folders, deepest first (only empty ones go) */
        snprintf(path, sizeof path, "%s/%s", dir, made[i]);
        rmdir(path);
    }
    free(names);
    return rc;
}
