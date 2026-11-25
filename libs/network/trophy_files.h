/*
 * trophy_files -- a title's PS3 trophy files on the host (game-agnostic).
 *
 * Adapted from the inFAMOUS 2 port's trophy reader (SamiEnazi/infamous2-port,
 * MIT) via Redgrave961/ps3recomp@d06b6134, with modifications for this tree.
 *
 *   TROPHY.TRP     the trophy pack a game ships (PS3_GAME/TROPDIR/<id>_<nn>/):
 *                  the definitions (TROPCONF.SFM), their text (TROP.SFM and
 *                  TROP_<language>.SFM) and the icons (ICON0.PNG, TROP<nnn>.PNG)
 *   TROPUSR.DAT    one user's unlocks with their times
 *
 * The same folder as RPCS3's dev_hdd0/home/<user>/trophy/<id>_<nn>/ is written
 * from them (trophy_folder_install + trophy_folder_write_usr), so the trophy
 * trackers that read RPCS3's folder (Playnite's SuccessStory and Playnite
 * Achievements) can read the port's too. sceNpTrophy.c keeps its own unlock
 * file as the source of truth; this folder is a copy, rewritten from it.
 *
 * The formats, as the public documentation and those trackers' readers have them:
 *
 * TROPHY.TRP (big-endian): a 0x40-byte header -- magic 0xDCA24D00, version,
 * file size (u64), entry count, entry size (0x40), a dev flag, a SHA-1 (checked
 * from version 2 on) and padding -- then the entries: name[32], offset (u64),
 * size (u64), flags (u32), padding. A PS3 pack stores its files as they are.
 *
 * TROP*.SFM: XML after a signature comment. <trophyconf> holds <npcommid>,
 * <title-name>, and per trophy <trophy id="000" hidden="no" ttype="P" pid="-1">
 * with <name> and <detail> (TROPCONF.SFM in a pack may carry the attributes
 * only; the text is in TROP.SFM / TROP_xx.SFM). ttype: P(latinum) G S B.
 *
 * TROPUSR.DAT (big-endian):
 *   0x00 u32 magic 0x818F54AD   0x04 u32 0x00010000   0x08 u32 table count
 *   0x0C u32 0                  0x10 0x20 bytes of zero
 *   then one 0x20-byte header per table:
 *     +0 u32 type  +4 u32 entry size (the contents after each entry's own
 *     0x10-byte header)  +8 u32 1  +12 u32 entry count  +16 u64 file offset of
 *     the entries  +24 u64 0
 *   each entry: +0 u32 type  +4 u32 entry size  +8 u32 index  +12 u32 0, then
 *     type 4 (size 0x50): trophy id, grade (1 platinum, 2 gold, 3 silver,
 *       4 bronze), pid (0xFFFFFFFF = none), zeros
 *     type 6 (size 0x60): trophy id, state (1 = unlocked), u32 0, u32 0,
 *       time 1 (u64), time 2 (u64), zeros
 *   The times are CellRtc ticks: microseconds since 0001-01-01 00:00 UTC.
 *   Playnite Achievements walks the table headers to type 6 and reads +16 id,
 *   +20 state, +40 time 2; SuccessStory scans the bytes for the entry headers
 *   (type 4 / 0x50 and type 6 / 0x60), takes the last <trophy count> of them,
 *   the low byte of the index as the id, +20 state and the low 7 bytes of time 1.
 *   So: index == trophy id, both times equal, type 6 last in the file.
 */
#ifndef PS3RECOMP_TROPHY_FILES_H
#define PS3RECOMP_TROPHY_FILES_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TROPHY_FILES_MAX     128     /* SCE_NP_TROPHY_MAX_NUM_TROPHIES */
#define TROPHY_TRP_MAX_FILES 512

typedef struct trophy_def {
    int  id;
    char grade;          /* 'P', 'G', 'S', 'B' ('B' when the file says nothing) */
    int  pid;            /* the platinum it counts towards; -1 = none */
    int  hidden;
    char name[128];      /* UTF-8, XML entities decoded */
    char detail[1024];
} trophy_def;

typedef struct trophy_set {
    char       comm_id[16];   /* <npcommid>, e.g. "NPWR01730_00" */
    char       title[128];
    char       title_detail[1024];
    int        n;             /* trophies in t[], in file order */
    trophy_def t[TROPHY_FILES_MAX];
} trophy_set;

typedef struct trp_entry {
    char     name[33];
    uint64_t offset, size;
} trp_entry;

typedef struct trp_index {
    char      path[1024];
    uint64_t  file_size;
    uint32_t  entry_count, entry_size;
    int       n;
    trp_entry e[TROPHY_TRP_MAX_FILES];
} trp_index;

/* ---- TROPHY.TRP ---- */
int   trp_open(const char* path, trp_index* out);                  /* 1 = a trophy pack */
const trp_entry* trp_find_entry(const trp_index* t, const char* name);
char* trp_read_entry_data(const trp_index* t, const char* name, size_t* size);  /* malloc'd, NUL-terminated */

/* ---- TROP*.SFM ---- */
/* Parse a trophyconf XML. With `into` already filled (structure from TROPCONF.SFM),
 * a text file only adds names, details and the title; trophies it lists that `into`
 * lacks are added. Returns the trophy count. */
int trophy_sfm_parse(const char* xml, trophy_set* into);
/* The set from a pack: TROPCONF.SFM, then the text of `lang_file` (e.g. "TROP_02.SFM"),
 * else TROP.SFM. Returns the trophy count (0 = none). */
int trophy_set_load(trophy_set* s, const trp_index* t, const char* lang_file);
const trophy_def* trophy_set_find(const trophy_set* s, int id);
/* The TROP_xx.SFM a PS3 system language id uses ("TROP_01.SFM" for 1). */
void trophy_lang_file(int ps3_lang, char* out, size_t n);

/* ---- TROPUSR.DAT ---- */
uint64_t trophy_tick_from_unix(uint64_t unix_secs);   /* 0 stays 0 */
uint64_t trophy_unix_from_tick(uint64_t tick);
size_t   tropusr_size(int n);
/* unlocked[id] / unix_secs[id] indexed by trophy id (TROPHY_FILES_MAX entries). */
size_t   tropusr_build(const trophy_set* s, const uint8_t* unlocked, const uint64_t* unix_secs,
                       uint8_t* out, size_t cap);

typedef struct tropusr_state {
    uint32_t id, state;
    uint64_t time1, time2;
    size_t   offset;          /* of the entry in the file */
} tropusr_state;
/* The type-6 table, checked the way the trackers check it. Returns the entry count,
 * -1 when the file is not a valid TROPUSR.DAT. */
int tropusr_parse(const uint8_t* b, size_t len, tropusr_state* out, int max);
/* Merge our unlocks into an existing file, in place: a trophy unlocked here and
 * locked there is unlocked with our time; unlocked on both sides keeps the earlier
 * time. Nothing is ever locked. Returns the entries changed, -1 = not a valid file. */
int tropusr_merge(uint8_t* b, size_t len, const uint8_t* unlocked, const uint64_t* unix_secs);

/* ---- the folder ---- */
int      trophy_mkdirs(const char* path);
uint8_t* trophy_read_file(const char* path, size_t* n);          /* malloc'd */
int      trophy_write_atomic(const char* path, const void* data, size_t n);   /* temp file + rename */
/* Every file of the pack into `dir` (made if missing), where missing or of another
 * size. TROPCONF.SFM becomes the text of `lang_file` (else TROP.SFM, else the pack's
 * own TROPCONF.SFM): names and details in one file, as the trackers read it. The
 * other TROP*.SFM are kept beside it (Playnite Achievements reads its language's).
 * Returns the files written, -1 on an error. */
int trophy_folder_install(const char* dir, const trp_index* t, const char* lang_file);
/* TROPUSR.DAT in `dir`. merge = 0: written from our state. merge = 1 (someone
 * else's folder): an existing valid file is merged into (tropusr_merge, written only
 * when something changed, a copy kept once as TROPUSR.DAT.port-backup); a missing
 * one is written from our state; an invalid one is left alone (-1). Returns 1 written,
 * 0 nothing to do, -1 error. */
int trophy_folder_write_usr(const char* dir, const trophy_set* s, const uint8_t* unlocked,
                            const uint64_t* unix_secs, int merge);
/* Where RPCS3 keeps one user's trophies, from what a player may give: the RPCS3
 * folder (the one with dev_hdd0, or its portable/ folder), its dev_hdd0, a
 * home/<user> folder or that user's trophy folder. 1 = `out` is the home/<user>
 * folder (it exists). */
int trophy_rpcs3_home(const char* given, char* out, size_t n);
/* The pack of trophy folder `folder` ("NPWR01730_00"): <disc_root>/PS3_GAME/TROPDIR/
 * <folder>/TROPHY.TRP (or <disc_root>/TROPDIR/...), else <hdd0_root>/game/<any>/
 * TROPDIR/<folder>/TROPHY.TRP. 1 = found, its path in `out`. */
int trophy_find_trp(const char* disc_root, const char* hdd0_root, const char* folder, char* out, size_t n);

#ifdef __cplusplus
}
#endif

#endif
