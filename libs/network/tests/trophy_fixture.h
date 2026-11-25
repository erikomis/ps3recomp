#ifndef TROPHY_FIXTURE_H
#define TROPHY_FIXTURE_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char g_fixture_root[256];

static const char k_fixture_conf[] =
    "<!--Sce-Np-Trophy-Signature: 00-->\n"
    "<trophyconf version=\"1.1\">\n"
    " <npcommid>NPWR00000_00</npcommid>\n"
    " <trophy id=\"000\" hidden=\"yes\" ttype=\"P\" pid=\"-1\"/>\n"
    " <trophy id=\"001\" hidden=\"no\" ttype=\"B\" pid=\"000\"/>\n"
    " <trophy id=\"002\" hidden=\"yes\" ttype=\"G\" pid=\"000\"/>\n"
    "</trophyconf>\n";

static const char k_fixture_text[] =
    "<!--Sce-Np-Trophy-Signature: 00-->\n"
    "<trophyconf version=\"1.1\">\n"
    " <npcommid>NPWR00000_00</npcommid>\n"
    " <title-name>Fixture &amp; Co</title-name>\n"
    " <title-detail>Trophies for the fixture.</title-detail>\n"
    " <trophy id=\"000\" hidden=\"yes\" ttype=\"P\" pid=\"-1\"><name>All</name><detail>Get them all.</detail></trophy>\n"
    " <trophy id=\"001\" hidden=\"no\" ttype=\"B\" pid=\"000\"><name>First</name><detail>Start.</detail></trophy>\n"
    " <trophy id=\"002\" hidden=\"yes\" ttype=\"G\" pid=\"000\"><name>Last</name><detail>Finish.</detail></trophy>\n"
    "</trophyconf>\n";

static void fx_be32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static void fx_be64(uint8_t* p, uint64_t v) { fx_be32(p, (uint32_t)(v >> 32)); fx_be32(p + 4, (uint32_t)v); }

static uint64_t fixture_write_pack(void)
{
    const char* names[2] = { "TROPCONF.SFM", "TROP.SFM" };
    const char* bodies[2] = { k_fixture_conf, k_fixture_text };
    const uint32_t count = 2, esz = 0x40;
    uint64_t off = 0x40 + count * esz;
    uint64_t total = off;
    for (int i = 0; i < 2; i++) total += strlen(bodies[i]);
    uint8_t* b = (uint8_t*)calloc(1, (size_t)total);
    fx_be32(b, 0xDCA24D00u);
    fx_be32(b + 4, 1);
    fx_be64(b + 8, total);
    fx_be32(b + 16, count);
    fx_be32(b + 20, esz);
    for (uint32_t i = 0; i < count; i++) {
        uint8_t* e = b + 0x40 + i * esz;
        memcpy(e, names[i], strlen(names[i]));
        fx_be64(e + 32, off);
        fx_be64(e + 40, strlen(bodies[i]));
        memcpy(b + off, bodies[i], strlen(bodies[i]));
        off += strlen(bodies[i]);
    }
    char path[512];
    const char* parts[] = { "/PS3_GAME", "/PS3_GAME/TROPDIR", "/PS3_GAME/TROPDIR/NPWR00000_00" };
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s%s", g_fixture_root, parts[i]);
        mkdir(path, 0755);
    }
    snprintf(path, sizeof path, "%s/PS3_GAME/TROPDIR/NPWR00000_00/TROPHY.TRP", g_fixture_root);
    FILE* f = fopen(path, "wb");
    fwrite(b, 1, (size_t)total, f);
    fclose(f);
    free(b);
    return total - 0x40 - (uint64_t)count * esz;
}

static void fixture_remove_pack(void)
{
    char path[512];
    snprintf(path, sizeof path, "%s/PS3_GAME/TROPDIR/NPWR00000_00/TROPHY.TRP", g_fixture_root);
    remove(path);
    const char* parts[] = { "/PS3_GAME/TROPDIR/NPWR00000_00", "/PS3_GAME/TROPDIR", "/PS3_GAME" };
    for (int i = 0; i < 3; i++) {
        snprintf(path, sizeof path, "%s%s", g_fixture_root, parts[i]);
        rmdir(path);
    }
}

const char* cellGame_get_boot_path(void) { return NULL; }
int cellfs_translate_path(const char* ps3_path, char* host_buf, size_t buf_size)
{
    if (strncmp(ps3_path, "/dev_bdvd/", 10)) return -1;
    snprintf(host_buf, buf_size, "%s/%s", g_fixture_root, ps3_path + 10);
    return 0;
}

static void fixture_guest_sign(uint32_t ea)
{
    memset(vm_base + ea, 0, 160);
    fx_be32(vm_base + ea, 0xB9DDE13Bu);
    vm_base[ea + 4] = 0x01;
    vm_base[ea + 5] = 0x00;
}

#endif
