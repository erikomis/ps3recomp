/*
 * trophy_files -- a title's PS3 trophy files on the host. See trophy_files.h
 * for the formats. Written from the public format documentation and the
 * readers of the trackers that use the folder (Playnite's SuccessStory and
 * Playnite Achievements); nothing here comes from an emulator's code.
 */
#include "trophy_files.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <direct.h>
#  define TF_MKDIR(p)  _mkdir(p)
#  define TF_STAT      _stat64
#  define TF_STAT_T    struct __stat64
#  define TF_IFMT      _S_IFMT
#  define TF_IFDIR     _S_IFDIR
#  define TF_IFREG     _S_IFREG
#else
#  include <unistd.h>
#  include <dirent.h>
#  include <sys/types.h>
#  define TF_MKDIR(p)  mkdir(p, 0755)
#  define TF_STAT      stat
#  define TF_STAT_T    struct stat
#  define TF_IFMT      S_IFMT
#  define TF_IFDIR     S_IFDIR
#  define TF_IFREG     S_IFREG
#endif

/* ---- big-endian helpers ---- */

static uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t rd64(const uint8_t* p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }
static void wr32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void wr64(uint8_t* p, uint64_t v) { wr32(p, (uint32_t)(v >> 32)); wr32(p + 4, (uint32_t)v); }

/* ---- TROPHY.TRP ---- */

#define TRP_MAGIC 0xDCA24D00u

int trp_open(const char* path, trp_index* out)
{
    memset(out, 0, sizeof *out);
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    uint8_t h[0x40];
    int ok = fread(h, 1, sizeof h, f) == sizeof h && rd32(h) == TRP_MAGIC;
    const uint64_t fsize = ok ? rd64(h + 8) : 0;
    const uint32_t count = ok ? rd32(h + 16) : 0, esz = ok ? rd32(h + 20) : 0;
    if (ok && (esz < 48 || count > TROPHY_TRP_MAX_FILES)) ok = 0;
    for (uint32_t i = 0; ok && i < count; i++) {
        uint8_t e[48];
        if (fseek(f, (long)(0x40 + (uint64_t)i * esz), SEEK_SET) || fread(e, 1, 48, f) != 48) { ok = 0; break; }
        trp_entry* d = &out->e[out->n];
        memcpy(d->name, e, 32);
        d->name[32] = 0;
        d->offset = rd64(e + 32);
        d->size = rd64(e + 40);
        if (!d->name[0] || d->offset + d->size > fsize || d->size > (64u << 20)) continue;   /* skip a bad one */
        out->n++;
    }
    fclose(f);
    if (!ok) { out->n = 0; return 0; }
    snprintf(out->path, sizeof out->path, "%s", path);
    out->file_size = fsize;
    out->entry_count = count;
    out->entry_size = esz;
    return 1;
}

const trp_entry* trp_find_entry(const trp_index* t, const char* name)
{
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->e[i].name, name)) return &t->e[i];
    return NULL;
}

char* trp_read_entry_data(const trp_index* t, const char* name, size_t* size)
{
    const trp_entry* e = trp_find_entry(t, name);
    if (!e) return NULL;
    FILE* f = fopen(t->path, "rb");
    if (!f) return NULL;
    char* d = (char*)malloc((size_t)e->size + 1);
    if (d && (fseek(f, (long)e->offset, SEEK_SET) || fread(d, 1, (size_t)e->size, f) != (size_t)e->size)) { free(d); d = NULL; }
    fclose(f);
    if (!d) return NULL;
    d[e->size] = 0;
    if (size) *size = (size_t)e->size;
    return d;
}

/* ---- TROP*.SFM ---- */

static void utf8_put(char** o, char* end, unsigned c)
{
    char b[4];
    int n;
    if (c < 0x80) { b[0] = (char)c; n = 1; }
    else if (c < 0x800) { b[0] = (char)(0xC0 | c >> 6); b[1] = (char)(0x80 | (c & 0x3F)); n = 2; }
    else if (c < 0x10000) { b[0] = (char)(0xE0 | c >> 12); b[1] = (char)(0x80 | (c >> 6 & 0x3F)); b[2] = (char)(0x80 | (c & 0x3F)); n = 3; }
    else { b[0] = (char)(0xF0 | c >> 18); b[1] = (char)(0x80 | (c >> 12 & 0x3F)); b[2] = (char)(0x80 | (c >> 6 & 0x3F)); b[3] = (char)(0x80 | (c & 0x3F)); n = 4; }
    if (*o + n >= end) return;
    memcpy(*o, b, (size_t)n);
    *o += n;
}

/* Text between a and b, entities decoded, whitespace trimmed. */
static void xml_text(const char* a, const char* b, char* out, size_t cap)
{
    char* o = out;
    char* end = out + cap;
    while (a < b && (*a == ' ' || *a == '\n' || *a == '\r' || *a == '\t')) a++;
    while (b > a && (b[-1] == ' ' || b[-1] == '\n' || b[-1] == '\r' || b[-1] == '\t')) b--;
    while (a < b && o + 1 < end) {
        if (*a == '&') {
            const char* semi = memchr(a, ';', (size_t)(b - a));
            if (semi && semi - a <= 10) {
                const size_t k = (size_t)(semi - a - 1);
                unsigned c = 0;
                if (k == 3 && !strncmp(a + 1, "amp", 3)) c = '&';
                else if (k == 2 && !strncmp(a + 1, "lt", 2)) c = '<';
                else if (k == 2 && !strncmp(a + 1, "gt", 2)) c = '>';
                else if (k == 4 && !strncmp(a + 1, "quot", 4)) c = '"';
                else if (k == 4 && !strncmp(a + 1, "apos", 4)) c = '\'';
                else if (k >= 2 && a[1] == '#') c = (unsigned)strtoul(a + (a[2] == 'x' || a[2] == 'X' ? 3 : 2), NULL, a[2] == 'x' || a[2] == 'X' ? 16 : 10);
                if (c) { utf8_put(&o, end, c); a = semi + 1; continue; }
            }
        }
        *o++ = *a++;
    }
    *o = 0;
}

/* <tag>text</tag> inside [p, end): 1 when found. */
static int xml_elem(const char* p, const char* end, const char* tag, char* out, size_t cap)
{
    char open[40], close[40];
    snprintf(open, sizeof open, "<%s>", tag);
    snprintf(close, sizeof close, "</%s>", tag);
    const size_t ol = strlen(open);
    for (const char* s = p; s + ol <= end; s++) {
        if (memcmp(s, open, ol)) continue;
        const char* a = s + ol;
        for (const char* e = a; e + strlen(close) <= end; e++)
            if (!memcmp(e, close, strlen(close))) { xml_text(a, e, out, cap); return 1; }
        return 0;
    }
    return 0;
}

/* attr="value" inside the tag [p, end). */
static int xml_attr(const char* p, const char* end, const char* name, char* out, size_t cap)
{
    const size_t nl = strlen(name);
    for (const char* s = p; s + nl + 2 <= end; s++) {
        if (memcmp(s, name, nl) || s[nl] != '=' || (s[nl + 1] != '"' && s[nl + 1] != '\'')) continue;
        if (s > p && s[-1] != ' ' && s[-1] != '\t' && s[-1] != '\n' && s[-1] != '\r') continue;
        const char q = s[nl + 1];
        const char* a = s + nl + 2;
        const char* e = memchr(a, q, (size_t)(end - a));
        if (!e) return 0;
        xml_text(a, e, out, cap);
        return 1;
    }
    return 0;
}

static trophy_def* set_get(trophy_set* s, int id)
{
    for (int i = 0; i < s->n; i++)
        if (s->t[i].id == id) return &s->t[i];
    if (s->n >= TROPHY_FILES_MAX) return NULL;
    trophy_def* d = &s->t[s->n++];
    memset(d, 0, sizeof *d);
    d->id = id;
    d->grade = 'B';
    d->pid = -1;
    return d;
}

int trophy_sfm_parse(const char* xml, trophy_set* into)
{
    if (!xml) return into->n;
    const char* end = xml + strlen(xml);
    char buf[256];
    const char* conf = strstr(xml, "<trophyconf");
    if (!conf) return into->n;
    const char* first = strstr(conf, "<trophy ");
    const char* head_end = first ? first : end;
    if (xml_elem(conf, head_end, "npcommid", buf, sizeof buf) && buf[0])
        snprintf(into->comm_id, sizeof into->comm_id, "%s", buf);
    if (xml_elem(conf, head_end, "title-name", buf, sizeof buf) && buf[0])
        snprintf(into->title, sizeof into->title, "%s", buf);
    {
        char detail[sizeof into->title_detail];
        if (xml_elem(conf, head_end, "title-detail", detail, sizeof detail) && detail[0])
            snprintf(into->title_detail, sizeof into->title_detail, "%s", detail);
    }
    for (const char* p = first; p; p = strstr(p + 1, "<trophy ")) {
        const char* gt = strchr(p, '>');
        if (!gt) break;
        if (!xml_attr(p, gt, "id", buf, sizeof buf)) continue;
        trophy_def* d = set_get(into, atoi(buf));
        if (!d) break;
        if (xml_attr(p, gt, "ttype", buf, sizeof buf) && buf[0]) d->grade = buf[0];
        if (xml_attr(p, gt, "pid", buf, sizeof buf) && buf[0]) d->pid = atoi(buf);
        if (xml_attr(p, gt, "hidden", buf, sizeof buf)) d->hidden = !strcmp(buf, "yes");
        if (gt[-1] == '/') continue;                           /* <trophy .../>: attributes only */
        const char* close = strstr(gt, "</trophy>");
        if (!close) close = end;
        xml_elem(gt, close, "name", d->name, sizeof d->name);
        xml_elem(gt, close, "detail", d->detail, sizeof d->detail);
    }
    return into->n;
}

int trophy_set_load(trophy_set* s, const trp_index* t, const char* lang_file)
{
    memset(s, 0, sizeof *s);
    char* conf = trp_read_entry_data(t, "TROPCONF.SFM", NULL);
    trophy_sfm_parse(conf, s);
    free(conf);
    char* text = lang_file ? trp_read_entry_data(t, lang_file, NULL) : NULL;
    if (!text) text = trp_read_entry_data(t, "TROP.SFM", NULL);
    trophy_sfm_parse(text, s);
    free(text);
    return s->n;
}

const trophy_def* trophy_set_find(const trophy_set* s, int id)
{
    for (int i = 0; i < s->n; i++)
        if (s->t[i].id == id) return &s->t[i];
    return NULL;
}

void trophy_lang_file(int ps3_lang, char* out, size_t n)
{
    snprintf(out, n, "TROP_%02d.SFM", ps3_lang);
}

/* ---- TROPUSR.DAT ---- */

#define TROPUSR_MAGIC   0x818F54ADu
#define TROPUSR_HEAD    0x30
#define TROPUSR_TABLE   0x20
#define TROPUSR_ENTRY   0x10       /* each entry's own header */
#define TROPUSR_SIZE4   0x50       /* type 4 contents */
#define TROPUSR_SIZE6   0x60       /* type 6 contents */
/* CellRtc ticks (us since 0001-01-01) at the Unix epoch */
#define TROPHY_TICK_UNIX_EPOCH 62135596800000000ull

uint64_t trophy_tick_from_unix(uint64_t s) { return s ? s * 1000000ull + TROPHY_TICK_UNIX_EPOCH : 0; }
uint64_t trophy_unix_from_tick(uint64_t t) { return t > TROPHY_TICK_UNIX_EPOCH ? (t - TROPHY_TICK_UNIX_EPOCH) / 1000000ull : 0; }

size_t tropusr_size(int n)
{
    return TROPUSR_HEAD + 2 * TROPUSR_TABLE + (size_t)n * (TROPUSR_ENTRY + TROPUSR_SIZE4)
                                            + (size_t)n * (TROPUSR_ENTRY + TROPUSR_SIZE6);
}

static uint32_t grade_num(char g)
{
    switch (g) {
    case 'P': return 1;
    case 'G': return 2;
    case 'S': return 3;
    default:  return 4;
    }
}

size_t tropusr_build(const trophy_set* s, const uint8_t* unlocked, const uint64_t* unix_secs,
                     uint8_t* out, size_t cap)
{
    const int n = s->n;
    const size_t need = tropusr_size(n);
    if (!out || cap < need || n <= 0) return 0;
    memset(out, 0, need);
    wr32(out + 0x00, TROPUSR_MAGIC);
    wr32(out + 0x04, 0x00010000u);
    wr32(out + 0x08, 2);
    const size_t off4 = TROPUSR_HEAD + 2 * TROPUSR_TABLE;
    const size_t off6 = off4 + (size_t)n * (TROPUSR_ENTRY + TROPUSR_SIZE4);
    uint8_t* th = out + TROPUSR_HEAD;
    wr32(th + 0, 4); wr32(th + 4, TROPUSR_SIZE4); wr32(th + 8, 1); wr32(th + 12, (uint32_t)n); wr64(th + 16, off4);
    th += TROPUSR_TABLE;
    wr32(th + 0, 6); wr32(th + 4, TROPUSR_SIZE6); wr32(th + 8, 1); wr32(th + 12, (uint32_t)n); wr64(th + 16, off6);
    for (int i = 0; i < n; i++) {
        const trophy_def* d = &s->t[i];
        const int id = d->id;
        const int on = id >= 0 && id < TROPHY_FILES_MAX && unlocked && unlocked[id];
        const uint64_t tick = on && unix_secs ? trophy_tick_from_unix(unix_secs[id]) : 0;
        uint8_t* e4 = out + off4 + (size_t)i * (TROPUSR_ENTRY + TROPUSR_SIZE4);
        wr32(e4 + 0, 4); wr32(e4 + 4, TROPUSR_SIZE4); wr32(e4 + 8, (uint32_t)id);
        wr32(e4 + 16, (uint32_t)id); wr32(e4 + 20, grade_num(d->grade)); wr32(e4 + 24, (uint32_t)d->pid);
        uint8_t* e6 = out + off6 + (size_t)i * (TROPUSR_ENTRY + TROPUSR_SIZE6);
        wr32(e6 + 0, 6); wr32(e6 + 4, TROPUSR_SIZE6); wr32(e6 + 8, (uint32_t)id);
        wr32(e6 + 16, (uint32_t)id); wr32(e6 + 20, on ? 1u : 0u);
        wr64(e6 + 32, tick); wr64(e6 + 40, tick);
    }
    return need;
}

int tropusr_parse(const uint8_t* b, size_t len, tropusr_state* out, int max)
{
    if (!b || len < TROPUSR_HEAD || rd32(b) != TROPUSR_MAGIC) return -1;
    const uint32_t tables = rd32(b + 8);
    if (!tables || tables > (len - TROPUSR_HEAD) / TROPUSR_TABLE) return -1;
    int found = 0, n = 0;
    for (uint32_t t = 0; t < tables; t++) {
        const uint8_t* th = b + TROPUSR_HEAD + (size_t)t * TROPUSR_TABLE;
        const uint32_t type = rd32(th), esz = rd32(th + 4), count = rd32(th + 12);
        const uint64_t off = rd64(th + 16);
        const uint64_t stride = (uint64_t)esz + TROPUSR_ENTRY;
        if (off > len || (count && (stride > len || count > (len - off) / stride))) return -1;
        if (type != 6) continue;
        if (found || esz != TROPUSR_SIZE6) return -1;
        found = 1;
        for (uint32_t i = 0; i < count; i++) {
            const uint8_t* e = b + off + (size_t)i * stride;
            if (rd32(e) != 6 || rd32(e + 4) != TROPUSR_SIZE6) return -1;
            const uint32_t id = rd32(e + 16);
            for (int k = 0; k < n && k < max; k++)
                if (out[k].id == id) return -1;                     /* a duplicate id */
            if (n < max) {
                out[n].id = id;
                out[n].state = rd32(e + 20);
                out[n].time1 = rd64(e + 32);
                out[n].time2 = rd64(e + 40);
                out[n].offset = (size_t)(off + (size_t)i * stride);
            }
            n++;
        }
    }
    return found ? n : -1;
}

int tropusr_merge(uint8_t* b, size_t len, const uint8_t* unlocked, const uint64_t* unix_secs)
{
    tropusr_state st[TROPHY_FILES_MAX * 2];
    const int n = tropusr_parse(b, len, st, (int)(sizeof st / sizeof st[0]));
    if (n < 0 || n > (int)(sizeof st / sizeof st[0])) return -1;
    int changed = 0;
    for (int i = 0; i < n; i++) {
        const uint32_t id = st[i].id;
        if (id >= TROPHY_FILES_MAX || !unlocked || !unlocked[id]) continue;
        const uint64_t ours = unix_secs ? trophy_tick_from_unix(unix_secs[id]) : 0;
        uint8_t* e = b + st[i].offset;
        if (!st[i].state) {
            wr32(e + 20, 1);
            wr64(e + 32, ours);
            wr64(e + 40, ours);
            changed++;
        } else if (ours && (!st[i].time2 || ours < st[i].time2)) {
            wr64(e + 32, ours);
            wr64(e + 40, ours);
            changed++;
        }
    }
    return changed;
}

/* ---- the folder ---- */

static int is_dir(const char* p)
{
    TF_STAT_T st;
    return TF_STAT(p, &st) == 0 && (st.st_mode & TF_IFMT) == TF_IFDIR;
}

static long long file_size(const char* p)
{
    TF_STAT_T st;
    if (TF_STAT(p, &st) != 0 || (st.st_mode & TF_IFMT) != TF_IFREG) return -1;
    return (long long)st.st_size;
}

int trophy_mkdirs(const char* path)
{
    char tmp[1024];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char* p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        if (p[-1] == ':') continue;                       /* C:\ */
        const char c = *p;
        *p = 0;
        if (!is_dir(tmp)) TF_MKDIR(tmp);
        *p = c;
    }
    if (!is_dir(tmp)) TF_MKDIR(tmp);
    return is_dir(tmp) ? 0 : -1;
}

uint8_t* trophy_read_file(const char* path, size_t* n)
{
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    uint8_t* d = NULL;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long sz = ftell(f);
        if (sz >= 0 && sz < (64l << 20) && fseek(f, 0, SEEK_SET) == 0) {
            d = (uint8_t*)malloc((size_t)sz + 1);
            if (d && fread(d, 1, (size_t)sz, f) != (size_t)sz) { free(d); d = NULL; }
            if (d) { d[sz] = 0; if (n) *n = (size_t)sz; }
        }
    }
    fclose(f);
    return d;
}

int trophy_write_atomic(const char* path, const void* data, size_t n)
{
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = fopen(tmp, "wb");
    int ok = f && fwrite(data, 1, n, f) == n;
    if (f && fclose(f) != 0) ok = 0;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) remove(tmp);
    return ok ? 0 : -1;
}

int trophy_folder_install(const char* dir, const trp_index* t, const char* lang_file)
{
    if (trophy_mkdirs(dir) != 0) return -1;
    char path[1200];
    int written = 0, errors = 0;
    /* TROPCONF.SFM: one file with the structure and the text, in the chosen language */
    const char* conf_src = lang_file && trp_find_entry(t, lang_file) ? lang_file
                         : trp_find_entry(t, "TROP.SFM") ? "TROP.SFM" : "TROPCONF.SFM";
    size_t csz = 0;
    char* conf = trp_read_entry_data(t, conf_src, &csz);
    if (conf) {
        snprintf(path, sizeof path, "%s/TROPCONF.SFM", dir);
        size_t have_n = 0;
        uint8_t* have = trophy_read_file(path, &have_n);
        if (!have || have_n != csz || memcmp(have, conf, csz)) {
            if (trophy_write_atomic(path, conf, csz) == 0) written++; else errors++;
        }
        free(have);
        free(conf);
    } else errors++;
    for (int i = 0; i < t->n; i++) {
        const trp_entry* e = &t->e[i];
        if (!strcmp(e->name, "TROPCONF.SFM") || strchr(e->name, '/') || strchr(e->name, '\\') || strstr(e->name, ".."))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->name);
        if (file_size(path) == (long long)e->size) continue;
        size_t sz = 0;
        char* d = trp_read_entry_data(t, e->name, &sz);
        if (d && trophy_write_atomic(path, d, sz) == 0) written++; else errors++;
        free(d);
    }
    return errors ? -1 : written;
}

int trophy_folder_write_usr(const char* dir, const trophy_set* s, const uint8_t* unlocked,
                            const uint64_t* unix_secs, int merge)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/TROPUSR.DAT", dir);
    size_t have_n = 0;
    uint8_t* have = trophy_read_file(path, &have_n);
    if (merge && have) {
        const int changed = tropusr_merge(have, have_n, unlocked, unix_secs);
        int r = changed < 0 ? -1 : 0;
        if (changed > 0) {
            char bak[1300];
            snprintf(bak, sizeof bak, "%s.port-backup", path);
            if (file_size(bak) < 0) {
                size_t on = 0;
                uint8_t* orig = trophy_read_file(path, &on);
                if (orig) trophy_write_atomic(bak, orig, on);
                free(orig);
            }
            r = trophy_write_atomic(path, have, have_n) == 0 ? 1 : -1;
        }
        free(have);
        return r;
    }
    const size_t need = tropusr_size(s->n);
    uint8_t* b = (uint8_t*)malloc(need);
    int r = -1;
    if (b && tropusr_build(s, unlocked, unix_secs, b, need) == need) {
        if (have && have_n == need && !memcmp(have, b, need)) r = 0;
        else r = trophy_write_atomic(path, b, need) == 0 ? 1 : -1;
    }
    free(b);
    free(have);
    return r;
}

int trophy_rpcs3_home(const char* given, char* out, size_t n)
{
    if (!given || !*given) return 0;
    char base[1024], p[1200];
    snprintf(base, sizeof base, "%s", given);
    size_t l = strlen(base);
    while (l > 1 && (base[l - 1] == '/' || base[l - 1] == '\\')) base[--l] = 0;
    /* <home>/<user>/trophy given: its parent */
    if (l > 7 && (base[l - 7] == '/' || base[l - 7] == '\\') && !strcmp(base + l - 6, "trophy")) {
        base[l - 7] = 0;
        if (is_dir(base)) { snprintf(out, n, "%s", base); return 1; }
        return 0;
    }
    snprintf(p, sizeof p, "%s/trophy", base);
    if (is_dir(p)) { snprintf(out, n, "%s", base); return 1; }    /* home/<user> */
    if (strlen(base) >= 8 && strspn(base + strlen(base) - 8, "0123456789") == 8 && is_dir(base)) {
        snprintf(out, n, "%s", base);                                 /* a home/<user> with no trophies yet */
        return 1;
    }
    /* RPCS3's folder (or its portable/ one): its active user, else 00000001 */
    static const char* const roots[] = { "", "/portable" };
    for (size_t i = 0; i < sizeof roots / sizeof roots[0]; i++) {
        char user[16] = "00000001", line[256];
        snprintf(p, sizeof p, "%s%s/GuiConfigs/persistent_settings.dat", base, roots[i]);
        FILE* f = fopen(p, "r");
        if (f) {
            int in_users = 0;
            while (fgets(line, sizeof line, f)) {
                char* s = line;
                while (*s == ' ' || *s == '\t') s++;
                if (*s == '[') { in_users = !strncmp(s, "[Users]", 7); continue; }
                if (in_users && !strncmp(s, "active_user", 11)) {
                    char* v = strchr(s, '=');
                    if (v) {
                        v++;
                        while (*v == ' ' || *v == '\t') v++;
                        if (strspn(v, "0123456789") == 8) { memcpy(user, v, 8); user[8] = 0; }
                    }
                }
            }
            fclose(f);
        }
        snprintf(p, sizeof p, "%s%s/dev_hdd0/home/%s", base, roots[i], user);
        if (is_dir(p)) { snprintf(out, n, "%s", p); return 1; }
    }
    snprintf(p, sizeof p, "%s/home/00000001", base);                 /* its dev_hdd0 */
    if (is_dir(p)) { snprintf(out, n, "%s", p); return 1; }
    return 0;
}

/* The pack for <commId>_<nn>: PS3_GAME/TROPDIR under the disc root, else a game
 * folder under dev_hdd0/game (a PSN title keeps TROPDIR in its own folder). */
int trophy_find_trp(const char* disc_root, const char* hdd0_root, const char* folder, char* out, size_t n)
{
    if (disc_root && *disc_root) {
        snprintf(out, n, "%s/PS3_GAME/TROPDIR/%s/TROPHY.TRP", disc_root, folder);
        if (file_size(out) > 0) return 1;
        snprintf(out, n, "%s/TROPDIR/%s/TROPHY.TRP", disc_root, folder);
        if (file_size(out) > 0) return 1;
    }
    if (!hdd0_root || !*hdd0_root) return 0;
#ifdef _WIN32
    char pat[1100];
    snprintf(pat, sizeof pat, "%s/game/*", hdd0_root);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    int found = 0;
    do {
        if (fd.cFileName[0] == '.' || !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        snprintf(out, n, "%s/game/%s/TROPDIR/%s/TROPHY.TRP", hdd0_root, fd.cFileName, folder);
        found = file_size(out) > 0;
    } while (!found && FindNextFileA(h, &fd));
    FindClose(h);
    return found;
#else
    char games[1100];
    snprintf(games, sizeof games, "%s/game", hdd0_root);
    DIR* d = opendir(games);
    if (!d) return 0;
    struct dirent* de;
    int found = 0;
    while (!found && (de = readdir(d))) {
        if (de->d_name[0] == '.') continue;
        snprintf(out, n, "%s/%s/TROPDIR/%s/TROPHY.TRP", games, de->d_name, folder);
        found = file_size(out) > 0;
    }
    closedir(d);
    return found;
#endif
}
