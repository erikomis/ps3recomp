#include "sceNpTrophy.h"
#include "trophy_files.h"
#include <stddef.h>
#include "../system/cellSysutil.h"
#include "../system/cellGame.h"
#include "../filesystem/cellFs.h"
#include "ps3emu/guest_call.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"
#include "../guest_struct.h"

#ifdef _WIN32
#include <direct.h>
#define mkdir_p(path) _mkdir(path)
#define PATH_SEP '\\'
#else
#include <sys/stat.h>
#define mkdir_p(path) mkdir(path, 0755)
#define PATH_SEP '/'
#endif

#define NP_TROPHY_COMM_SIGN_MAGIC 0xB9DDE13Bu
#define TRP_HEADER_SIZE 0x40u
#define RTC_TICK_UNIX_EPOCH 62135596800000000ull

static int s_trophy_initialized = 0;
static char s_storage_path[512] = "gamedata/trophies";
static int  s_storage_path_set = 0;

static void trophy_adopt_gamedata_root(void)
{
    static int s_done = 0;
    if (s_done || s_storage_path_set) return;
    s_done = 1;
    const char* env = getenv("PS3_GAMEDATA_ROOT");
    if (env && *env)
        snprintf(s_storage_path, sizeof(s_storage_path), "%s/trophies", env);
}

typedef struct {
    int        in_use;
    int        registered;
    int        read_only;
    char       name[16];
    trp_index  pack;
    trophy_set set;
    u8         unlocked[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
    u64        tick[SCE_NP_TROPHY_MAX_NUM_TROPHIES];
} TrophyContext;

typedef struct {
    int in_use;
} TrophyHandle;

static TrophyContext s_contexts[SCE_NP_TROPHY_MAX_CONTEXTS + 1];
static TrophyHandle  s_handles[SCE_NP_TROPHY_MAX_HANDLES + 1];

static u64 rtc_tick_now(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return RTC_TICK_UNIX_EPOCH + (u64)ts.tv_sec * 1000000ull + (u64)ts.tv_nsec / 1000ull;
}

static void guest_put_string(u32 ea, const char* s, size_t cap)
{
    size_t n = strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(vm_base + ea, s, n);
    vm_base[ea + n] = 0;
}

static void guest_zero(u32 ea, size_t n)
{
    memset(vm_base + ea, 0, n);
}

static u32 grade_of(char ttype)
{
    switch (ttype) {
    case 'P': return SCE_NP_TROPHY_GRADE_PLATINUM;
    case 'G': return SCE_NP_TROPHY_GRADE_GOLD;
    case 'S': return SCE_NP_TROPHY_GRADE_SILVER;
    case 'B': return SCE_NP_TROPHY_GRADE_BRONZE;
    default:  return SCE_NP_TROPHY_GRADE_UNKNOWN;
    }
}

static void trophy_ensure_dir(void)
{
    char tmp[512];
    trophy_adopt_gamedata_root();
    strncpy(tmp, s_storage_path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char* p = tmp + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = '\0';
            mkdir_p(tmp);
            *p = PATH_SEP;
        }
    }
    mkdir_p(tmp);
}

static void trophy_get_filepath(const TrophyContext* ctx, char* buf, size_t bufsize)
{
    trophy_adopt_gamedata_root();
    snprintf(buf, bufsize, "%s/%s.json", s_storage_path, ctx->name);
}

static int trophy_installed(const TrophyContext* ctx)
{
    char path[1024];
    trophy_get_filepath(ctx, path, sizeof(path));
    FILE* fp = fopen(path, "r");
    if (!fp) return 0;
    fclose(fp);
    return 1;
}

static int trophy_save(const TrophyContext* ctx)
{
    char filepath[1024];
    trophy_ensure_dir();
    trophy_get_filepath(ctx, filepath, sizeof(filepath));
    FILE* fp = fopen(filepath, "w");
    if (!fp) {
        printf("[sceNpTrophy] could not save trophies to %s\n", filepath);
        return 0;
    }
    int first = 1;
    fprintf(fp, "{\n  \"trophies\": [");
    for (int i = 0; i < ctx->set.n; i++) {
        const int id = ctx->set.t[i].id;
        if (id < 0 || id >= SCE_NP_TROPHY_MAX_NUM_TROPHIES || !ctx->unlocked[id]) continue;
        fprintf(fp, "%s\n    { \"id\": %d, \"tick\": %llu }", first ? "" : ",", id,
                (unsigned long long)ctx->tick[id]);
        first = 0;
    }
    fprintf(fp, "\n  ]\n}\n");
    fclose(fp);
    return 1;
}

static void trophy_load(TrophyContext* ctx)
{
    char filepath[1024];
    char line[256];
    trophy_get_filepath(ctx, filepath, sizeof(filepath));
    FILE* fp = fopen(filepath, "r");
    if (!fp) return;
    while (fgets(line, sizeof(line), fp)) {
        const char* id_pos = strstr(line, "\"id\":");
        const char* tick_pos = strstr(line, "\"tick\":");
        unsigned id = 0;
        unsigned long long tick = 0;
        if (id_pos && tick_pos && sscanf(id_pos, "\"id\": %u", &id) == 1 &&
            sscanf(tick_pos, "\"tick\": %llu", &tick) == 1 && id < SCE_NP_TROPHY_MAX_NUM_TROPHIES) {
            ctx->unlocked[id] = 1;
            ctx->tick[id] = tick;
        }
    }
    fclose(fp);
}

static int trophy_pack_open(TrophyContext* ctx)
{
    const char* boot = cellGame_get_boot_path();
    char app[512] = "/dev_bdvd/PS3_GAME/";
    int from_hdd0 = 0;
    if (boot && !strncmp(boot, "/dev_hdd0/game/", 15)) {
        const char* usrdir = strstr(boot + 15, "/USRDIR/");
        if (usrdir && (size_t)(usrdir - boot) + 2 < sizeof(app)) {
            memcpy(app, boot, (size_t)(usrdir - boot) + 1);
            app[usrdir - boot + 1] = 0;
            from_hdd0 = 1;
        }
    }
    for (int pass = 0; pass < 2; pass++) {
        char guest[640], host[1024];
        snprintf(guest, sizeof(guest), "%sTROPDIR/%s/TROPHY.TRP",
                 pass == 0 ? app : "/dev_bdvd/PS3_GAME/", ctx->name);
        if (cellfs_translate_path(guest, host, sizeof(host)) == 0 && trp_open(host, &ctx->pack)) {
            printf("[sceNpTrophy] trophy pack %s\n", guest);
            return 1;
        }
        if (!from_hdd0) break;
    }
    return 0;
}

static void trophy_set_open(TrophyContext* ctx)
{
    char lang_file[32];
    trophy_lang_file((int)ps3_sysutil_language(), lang_file, sizeof(lang_file));
    trophy_set_load(&ctx->set, &ctx->pack, lang_file);
}

static const trophy_def* trophy_def_of(const TrophyContext* ctx, s32 id)
{
    return trophy_set_find(&ctx->set, id);
}

static s32 trophy_get_context(SceNpTrophyContext context, SceNpTrophyHandle handle,
                              int writeable, TrophyContext** out)
{
    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!s_contexts[context].in_use)
        return (s32)SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
    if (writeable && s_contexts[context].read_only)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_CONTEXT;
    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!s_handles[handle].in_use)
        return (s32)SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;
    *out = &s_contexts[context];
    return CELL_OK;
}

static s32 trophy_platinum_unlocked_by(const TrophyContext* ctx, s32 trophyId)
{
    const trophy_def* d = trophy_def_of(ctx, trophyId);
    if (!d || d->pid < 0 || d->pid >= SCE_NP_TROPHY_MAX_NUM_TROPHIES || ctx->unlocked[d->pid])
        return SCE_NP_TROPHY_INVALID_TROPHY_ID;
    for (int i = 0; i < ctx->set.n; i++) {
        const trophy_def* t = &ctx->set.t[i];
        if (t->pid == d->pid && (t->id < 0 || t->id >= SCE_NP_TROPHY_MAX_NUM_TROPHIES || !ctx->unlocked[t->id]))
            return SCE_NP_TROPHY_INVALID_TROPHY_ID;
    }
    return d->pid;
}

void sceNpTrophySetStoragePath(const char* path)
{
    path = path ? (const char*)(vm_base + (uint32_t)(uintptr_t)path) : NULL;
    if (path) {
        strncpy(s_storage_path, path, sizeof(s_storage_path) - 1);
        s_storage_path[sizeof(s_storage_path) - 1] = '\0';
        s_storage_path_set = 1;
    }
}

s32 sceNpTrophyInit(void* poolPtr, u32 poolSize, u32 containerId, u64 options)
{
    (void)poolPtr; (void)poolSize; (void)containerId;
    if (options > 0)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;
    if (s_trophy_initialized)
        return (s32)SCE_NP_TROPHY_ERROR_ALREADY_INITIALIZED;
    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 1;
    return CELL_OK;
}

s32 sceNpTrophyTerm(void)
{
    if (!s_trophy_initialized)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    memset(s_contexts, 0, sizeof(s_contexts));
    memset(s_handles, 0, sizeof(s_handles));
    s_trophy_initialized = 0;
    return CELL_OK;
}

s32 sceNpTrophyCreateContext(SceNpTrophyContext* context,
                             const SceNpCommunicationId* commId,
                             const SceNpCommunicationSignature* commSign,
                             u64 options)
{
    if (!commSign)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!s_trophy_initialized)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (!context || !commId)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (options > SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;

    const u32 id_ea = GUEST_EA(commId);
    const u32 sign_ea = GUEST_EA(commSign);
    const u8 num = vm_read8(id_ea + offsetof(SceNpCommunicationId, num));
    if (num > 99)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID;
    if (vm_read32(sign_ea) != NP_TROPHY_COMM_SIGN_MAGIC || vm_read16(sign_ea + 4) != 0x100)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID;
    for (u32 i = 6; i < 12; i++)
        if (vm_read8(sign_ea + i))
            return (s32)SCE_NP_TROPHY_ERROR_INVALID_NP_COMM_ID;

    char data[SCE_NP_COMMUNICATION_ID_MAX_LENGTH + 1];
    memcpy(data, vm_base + id_ea, SCE_NP_COMMUNICATION_ID_MAX_LENGTH);
    data[SCE_NP_COMMUNICATION_ID_MAX_LENGTH] = 0;

    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_CONTEXTS; i++) {
        if (s_contexts[i].in_use) continue;
        memset(&s_contexts[i], 0, sizeof(TrophyContext));
        s_contexts[i].in_use = 1;
        s_contexts[i].read_only = options == SCE_NP_TROPHY_OPTIONS_CREATE_CONTEXT_READ_ONLY;
        snprintf(s_contexts[i].name, sizeof(s_contexts[i].name), "%s_%02u", data, (unsigned)num);
        vm_write32(GUEST_EA(context), (u32)i);
        printf("[sceNpTrophy] CreateContext(%s) -> ctx=%d\n", s_contexts[i].name, i);
        return CELL_OK;
    }
    return (s32)SCE_NP_TROPHY_ERROR_EXCEEDS_MAX;
}

s32 sceNpTrophyDestroyContext(SceNpTrophyContext context)
{
    if (!s_trophy_initialized)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (context <= 0 || context > SCE_NP_TROPHY_MAX_CONTEXTS)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!s_contexts[context].in_use)
        return (s32)SCE_NP_TROPHY_ERROR_UNKNOWN_CONTEXT;
    s_contexts[context].in_use = 0;
    return CELL_OK;
}

s32 sceNpTrophyCreateHandle(SceNpTrophyHandle* handle)
{
    if (!s_trophy_initialized)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (!handle)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    for (s32 i = 1; i <= SCE_NP_TROPHY_MAX_HANDLES; i++) {
        if (s_handles[i].in_use) continue;
        s_handles[i].in_use = 1;
        vm_write32(GUEST_EA(handle), (u32)i);
        return CELL_OK;
    }
    return (s32)SCE_NP_TROPHY_ERROR_EXCEEDS_MAX;
}

s32 sceNpTrophyDestroyHandle(SceNpTrophyHandle handle)
{
    if (!s_trophy_initialized)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    if (handle <= 0 || handle > SCE_NP_TROPHY_MAX_HANDLES)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!s_handles[handle].in_use)
        return (s32)SCE_NP_TROPHY_ERROR_UNKNOWN_HANDLE;
    s_handles[handle].in_use = 0;
    return CELL_OK;
}

s32 sceNpTrophyRegisterContext(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               u32 statusCb,
                               u32 arg,
                               u64 options)
{
    if (!s_trophy_initialized)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    TrophyContext* ctx;
    s32 err = trophy_get_context(context, handle, 1, &ctx);
    if (err) return err;
    if (!statusCb)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (options > SCE_NP_TROPHY_OPTIONS_REGISTER_CONTEXT_SHOW_ERROR_EXIT)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;

    if (!trophy_pack_open(ctx)) {
        printf("[sceNpTrophy] RegisterContext(%s): no TROPHY.TRP\n", ctx->name);
        return (s32)SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST;
    }

    const u32 status = trophy_installed(ctx) ? SCE_NP_TROPHY_STATUS_INSTALLED
                                             : SCE_NP_TROPHY_STATUS_NOT_INSTALLED;
    uint64_t r3 = 0;
    if (!ppu_guest_call_ct_checked(vm_read32(statusCb), vm_read32(statusCb + 4),
                                   (u32)context, status, 0, 0, arg, 0, 0, 0, &r3))
        return (s32)SCE_NP_TROPHY_ERROR_UNKNOWN;
    if ((s32)(u32)r3 < 0)
        return (s32)SCE_NP_TROPHY_ERROR_PROCESSING_ABORTED;

    err = trophy_get_context(context, handle, 1, &ctx);
    if (err) return err;

    trophy_set_open(ctx);
    if (ctx->set.n <= 0 || ctx->set.n > SCE_NP_TROPHY_MAX_NUM_TROPHIES) {
        printf("[sceNpTrophy] RegisterContext(%s): %s holds no usable trophy list\n", ctx->name, ctx->pack.path);
        return (s32)SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE;
    }
    memset(ctx->unlocked, 0, sizeof(ctx->unlocked));
    memset(ctx->tick, 0, sizeof(ctx->tick));
    trophy_load(ctx);
    if (!trophy_save(ctx))
        return (s32)SCE_NP_TROPHY_ERROR_ILLEGAL_UPDATE;
    ctx->registered = 1;
    printf("[sceNpTrophy] RegisterContext(%s): %d trophies, \"%s\"\n", ctx->name, ctx->set.n, ctx->set.title);

    const s32 progress = ctx->set.n - 1;
    const struct { u32 status; s32 total; } steps[] = {
        { SCE_NP_TROPHY_STATUS_PROCESSING_SETUP,    3 },
        { SCE_NP_TROPHY_STATUS_PROCESSING_PROGRESS, progress },
        { SCE_NP_TROPHY_STATUS_PROCESSING_FINALIZE, (progress > 9 ? progress : 9) - 5 },
        { SCE_NP_TROPHY_STATUS_PROCESSING_COMPLETE, 0 },
    };
    for (unsigned s = 0; s < sizeof(steps) / sizeof(steps[0]); s++) {
        for (s32 done = 0; done <= steps[s].total; done++) {
            const u64 args[8] = { (u32)context, steps[s].status, (u64)(u32)done,
                                  (u64)(u32)steps[s].total, arg, 0, 0, 0 };
            if (cellSysutilQueueGuestCallbackArgs(statusCb, args) != CELL_OK)
                return (s32)SCE_NP_TROPHY_ERROR_OUT_OF_MEMORY;
        }
    }
    return CELL_OK;
}

s32 sceNpTrophyGetRequiredDiskSpace(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    u64* reqSpace, u64 options)
{
    if (options > 0)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_SUPPORTED;
    if (!s_trophy_initialized)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    TrophyContext* ctx;
    s32 err = trophy_get_context(context, handle, 0, &ctx);
    if (err) return err;
    if (!reqSpace)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u64 space = 0;
    if (!trophy_installed(ctx)) {
        if (!trophy_pack_open(ctx))
            return (s32)SCE_NP_TROPHY_ERROR_CONF_DOES_NOT_EXIST;
        space = ctx->pack.file_size - TRP_HEADER_SIZE -
                (u64)ctx->pack.entry_count * ctx->pack.entry_size;
    }
    vm_write64(GUEST_EA(reqSpace), space);
    return CELL_OK;
}

s32 sceNpTrophyGetGameInfo(SceNpTrophyContext context,
                           SceNpTrophyHandle handle,
                           SceNpTrophyGameDetails* details,
                           SceNpTrophyGameData* data)
{
    if (!s_trophy_initialized)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    TrophyContext* ctx;
    s32 err = trophy_get_context(context, handle, 0, &ctx);
    if (err) return err;
    if (!ctx->registered)
        return (s32)SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;
    if (!details && !data)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;

    u32 count[5] = {0}, unlocked[5] = {0};
    for (int i = 0; i < ctx->set.n; i++) {
        const trophy_def* t = &ctx->set.t[i];
        const u32 g = grade_of(t->grade);
        count[0]++;
        if (g) count[g]++;
        if (t->id >= 0 && t->id < SCE_NP_TROPHY_MAX_NUM_TROPHIES && ctx->unlocked[t->id]) {
            unlocked[0]++;
            if (g) unlocked[g]++;
        }
    }

    if (details) {
        const u32 ea = GUEST_EA(details);
        guest_zero(ea, sizeof(SceNpTrophyGameDetails));
        vm_write32(ea + offsetof(SceNpTrophyGameDetails, numTrophies), count[0]);
        vm_write32(ea + offsetof(SceNpTrophyGameDetails, numPlatinum), count[SCE_NP_TROPHY_GRADE_PLATINUM]);
        vm_write32(ea + offsetof(SceNpTrophyGameDetails, numGold), count[SCE_NP_TROPHY_GRADE_GOLD]);
        vm_write32(ea + offsetof(SceNpTrophyGameDetails, numSilver), count[SCE_NP_TROPHY_GRADE_SILVER]);
        vm_write32(ea + offsetof(SceNpTrophyGameDetails, numBronze), count[SCE_NP_TROPHY_GRADE_BRONZE]);
        guest_put_string(ea + offsetof(SceNpTrophyGameDetails, title), ctx->set.title,
                         SCE_NP_TROPHY_GAME_TITLE_MAX_SIZE);
        guest_put_string(ea + offsetof(SceNpTrophyGameDetails, description), ctx->set.title_detail,
                         SCE_NP_TROPHY_GAME_DESC_MAX_SIZE);
    }
    if (data) {
        const u32 ea = GUEST_EA(data);
        vm_write32(ea + offsetof(SceNpTrophyGameData, unlockedTrophies), unlocked[0]);
        vm_write32(ea + offsetof(SceNpTrophyGameData, unlockedPlatinum), unlocked[SCE_NP_TROPHY_GRADE_PLATINUM]);
        vm_write32(ea + offsetof(SceNpTrophyGameData, unlockedGold), unlocked[SCE_NP_TROPHY_GRADE_GOLD]);
        vm_write32(ea + offsetof(SceNpTrophyGameData, unlockedSilver), unlocked[SCE_NP_TROPHY_GRADE_SILVER]);
        vm_write32(ea + offsetof(SceNpTrophyGameData, unlockedBronze), unlocked[SCE_NP_TROPHY_GRADE_BRONZE]);
    }
    return CELL_OK;
}

s32 sceNpTrophyGetTrophyInfo(SceNpTrophyContext context,
                             SceNpTrophyHandle handle,
                             SceNpTrophyId trophyId,
                             SceNpTrophyDetails* details,
                             SceNpTrophyData* data)
{
    if (!s_trophy_initialized)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    TrophyContext* ctx;
    s32 err = trophy_get_context(context, handle, 0, &ctx);
    if (err) return err;
    if (!details && !data)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!ctx->registered)
        return (s32)SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    const trophy_def* t = trophy_def_of(ctx, trophyId);
    if (!t || trophyId >= SCE_NP_TROPHY_MAX_NUM_TROPHIES)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
    const int unlocked = ctx->unlocked[trophyId];
    if (t->hidden && !unlocked)
        return (s32)SCE_NP_TROPHY_ERROR_HIDDEN;

    if (details) {
        const u32 ea = GUEST_EA(details);
        guest_zero(ea, sizeof(SceNpTrophyDetails));
        vm_write32(ea + offsetof(SceNpTrophyDetails, trophyId), (u32)trophyId);
        vm_write32(ea + offsetof(SceNpTrophyDetails, trophyGrade), grade_of(t->grade));
        guest_put_string(ea + offsetof(SceNpTrophyDetails, name), t->name, SCE_NP_TROPHY_NAME_MAX_SIZE);
        guest_put_string(ea + offsetof(SceNpTrophyDetails, description), t->detail, SCE_NP_TROPHY_DESC_MAX_SIZE);
        vm_write8(ea + offsetof(SceNpTrophyDetails, hidden), t->hidden ? 1 : 0);
    }
    if (data) {
        const u32 ea = GUEST_EA(data);
        guest_zero(ea, sizeof(SceNpTrophyData));
        vm_write64(ea + offsetof(SceNpTrophyData, timestamp), ctx->tick[trophyId]);
        vm_write32(ea + offsetof(SceNpTrophyData, trophyId), (u32)trophyId);
        vm_write8(ea + offsetof(SceNpTrophyData, unlocked), unlocked ? 1 : 0);
    }
    return CELL_OK;
}

s32 sceNpTrophyUnlockTrophy(SceNpTrophyContext context,
                            SceNpTrophyHandle handle,
                            SceNpTrophyId trophyId,
                            SceNpTrophyId* platinumId)
{
    if (!s_trophy_initialized)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    TrophyContext* ctx;
    s32 err = trophy_get_context(context, handle, 1, &ctx);
    if (err) return err;
    if (!ctx->registered)
        return (s32)SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    const trophy_def* t = trophy_def_of(ctx, trophyId);
    if (trophyId < 0 || trophyId >= ctx->set.n || trophyId >= SCE_NP_TROPHY_MAX_NUM_TROPHIES || !t)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_TROPHY_ID;
    if (grade_of(t->grade) == SCE_NP_TROPHY_GRADE_PLATINUM)
        return (s32)SCE_NP_TROPHY_ERROR_CANNOT_UNLOCK_PLATINUM;
    if (ctx->unlocked[trophyId])
        return (s32)SCE_NP_TROPHY_ERROR_ALREADY_UNLOCKED;

    const u64 now = rtc_tick_now();
    ctx->unlocked[trophyId] = 1;
    ctx->tick[trophyId] = now;
    printf("[sceNpTrophy] trophy %d unlocked: %s\n", trophyId, t->name);

    const s32 plat = trophy_platinum_unlocked_by(ctx, trophyId);
    if (plat != SCE_NP_TROPHY_INVALID_TROPHY_ID) {
        ctx->unlocked[plat] = 1;
        ctx->tick[plat] = now;
        printf("[sceNpTrophy] platinum trophy %d unlocked\n", plat);
    }
    if (platinumId)
        vm_write32(GUEST_EA(platinumId), (u32)plat);
    trophy_save(ctx);
    return CELL_OK;
}

s32 sceNpTrophyGetTrophyUnlockState(SceNpTrophyContext context,
                                    SceNpTrophyHandle handle,
                                    SceNpTrophyFlagArray* flags,
                                    u32* count)
{
    if (!s_trophy_initialized)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    TrophyContext* ctx;
    s32 err = trophy_get_context(context, handle, 0, &ctx);
    if (err) return err;
    if (!flags || !count)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!ctx->registered)
        return (s32)SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    const u32 n = (u32)ctx->set.n;
    vm_write32(GUEST_EA(count), n);
    u32 bits[SCE_NP_TROPHY_MAX_NUM_TROPHIES / 32] = {0};
    for (u32 id = 0; id < n && id < SCE_NP_TROPHY_MAX_NUM_TROPHIES; id++)
        if (ctx->unlocked[id])
            bits[id / 32] |= 1u << (id % 32);
    const u32 ea = GUEST_EA(flags);
    for (u32 w = 0; w < SCE_NP_TROPHY_MAX_NUM_TROPHIES / 32; w++)
        vm_write32(ea + w * 4, bits[w]);
    return CELL_OK;
}

s32 sceNpTrophyGetGameProgress(SceNpTrophyContext context,
                               SceNpTrophyHandle handle,
                               s32* percentage)
{
    if (!s_trophy_initialized)
        return (s32)SCE_NP_TROPHY_ERROR_NOT_INITIALIZED;
    TrophyContext* ctx;
    s32 err = trophy_get_context(context, handle, 0, &ctx);
    if (err) return err;
    if (!percentage)
        return (s32)SCE_NP_TROPHY_ERROR_INVALID_ARGUMENT;
    if (!ctx->registered)
        return (s32)SCE_NP_TROPHY_ERROR_CONTEXT_NOT_REGISTERED;

    u32 unlocked = 0;
    const u32 n = (u32)ctx->set.n;
    for (u32 id = 0; id < n && id < SCE_NP_TROPHY_MAX_NUM_TROPHIES; id++)
        unlocked += ctx->unlocked[id] ? 1u : 0u;
    const u32 pct = n ? (unlocked * 100u + n / 2u) / n : 0u;
    vm_write32(GUEST_EA(percentage), pct);
    return CELL_OK;
}
