/*
 * Tokisaki Port -- Killzone 2 specific HLE overrides.
 *
 * Handlers registered here with ps3_hle_register_ctx() run before the generic
 * ps3recomp handlers for the same NID (context handlers are matched first,
 * first registration wins). Keep every override documented with the reason
 * the generic one is not enough for this title.
 */
#include "ppu_recomp.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <zlib.h>

typedef void (*hle_ctx_fn)(ppu_context*);
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn);
extern "C" int32_t cellSpursEventFlagSet(void* event_flag_ea, uint16_t bits);
extern "C" uint8_t* vm_base;
extern "C" int32_t cellFontInit(const void* config);
extern "C" int32_t cellFontGetRenderCharGlyphMetrics(void* font, uint32_t code, void* metrics);
extern "C" void cellSaveData_register_ctx_handlers(void);
extern "C" int32_t cellFontOpenFontFile(void* font, const char* path, uint32_t sub, int32_t uid);
extern "C" int32_t cellSaveDataAutoSave2(uint32_t version, const char* dirName, uint32_t errDialog,
                                         void* setBuf, void* funcStat, void* funcFile,
                                         uint32_t container, void* userdata);

/* ---------------------------------------------------------------------------
 * Guest scratch memory for structures these handlers hand to the title.
 *
 * 0x2F000000 sits between the runtime's HLE inject window (0x20000000, GCM
 * labels / control register / offset tables, 0x8000 bytes) and the raw-SPU
 * windows at 0x30000000; Killzone 2 never maps anything there (its heaps come
 * from sys_memory at 0x40000000 and the stacks at 0xD0000000). Bump-allocated,
 * never freed -- only a handful of small objects are ever created.
 * -----------------------------------------------------------------------*/
static uint32_t kz2_guest_alloc(uint32_t size, uint32_t align)
{
    static std::atomic<uint32_t> next{0x2F000000u};
    uint32_t cur = next.load(), ea;
    do {
        ea = (cur + align - 1) & ~(align - 1);
    } while (!next.compare_exchange_weak(cur, ea + size));
    memset(vm_base + ea, 0, size);
    return ea;
}

/* Killzone 2 ships no game-side PRX in USRDIR; every import is firmware. */
extern "C" void ps3_load_prx_modules(void)
{
}

/* ---------------------------------------------------------------------------
 * Edge Zlib, inflated on the host
 *
 * FIOS hands every compressed data_ps3.psarc block to Sony's Edge Zlib SPURS
 * task (edgezlib_inflate_task, SPU image at 0xCC3500) through
 * _cellSpursLFQueuePushBody, then blocks in cellSpursEventFlagWait until the
 * task signals. The runtime implements neither the PPU side of the SPURS
 * LFQueue nor anything that would carry the request to the lifted task, so
 * the request is serviced here, synchronously, before the push returns.
 *
 * The 32-byte queue element, as captured from the title:
 *   +0x00 be32 EA of the compressed data (raw deflate: FIOS points past the
 *              2-byte zlib header of the psarc block)
 *   +0x04 be32 EA of the output buffer
 *   +0x08 be32 compressed size
 *   +0x0C be32 expected uncompressed size
 *   +0x10 be32 EA of the work-to-do counter (be32); bit 0 = inflate, else copy
 *   +0x14 be32 EA of the CellSpursEventFlag to set when the counter hits zero
 *   +0x18 be16 event flag bits
 *   +0x1C be16 output skip-begin size, +0x1E be16 output skip-end size
 *              (bytes of the uncompressed stream not written out)
 * -----------------------------------------------------------------------*/
static uint8_t* guest(uint32_t ea) { return vm_base + ea; }

static bool kz2_edge_inflate(uint32_t src, uint32_t src_size, uint32_t dst,
                             uint32_t expected, uint32_t skip_begin, uint32_t skip_end,
                             bool do_inflate)
{
    static thread_local uint8_t* scratch = nullptr;
    static thread_local uint32_t scratch_cap = 0;
    if (scratch_cap < expected) {
        free(scratch);
        scratch_cap = expected;
        scratch = (uint8_t*)malloc(scratch_cap);
    }

    uint32_t produced = 0;
    if (!do_inflate) {
        memcpy(scratch, guest(src), src_size < expected ? src_size : expected);
        produced = src_size < expected ? src_size : expected;
    } else {
        z_stream zs = {};
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) return false;
        zs.next_in   = guest(src);
        zs.avail_in  = src_size;
        zs.next_out  = scratch;
        zs.avail_out = expected;
        int rc = inflate(&zs, Z_FINISH);
        produced = expected - zs.avail_out;
        inflateEnd(&zs);
        if (rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
            fprintf(stderr, "[kz2] edge inflate FAILED rc=%d src=0x%08X size=%u -> %u/%u\n",
                    rc, src, src_size, produced, expected);
            return false;
        }
    }
    if (skip_begin + skip_end > produced) return false;
    /* memmove: Edge inflates in place, the output may overlap the input. */
    memmove(guest(dst), scratch + skip_begin, produced - skip_begin - skip_end);
    return produced == expected;
}

static void kz2_lfqueue_push_body(ppu_context* ctx)
{
    const uint32_t q   = (uint32_t)ctx->gpr[3];
    const uint32_t buf = (uint32_t)ctx->gpr[4];

    const uint32_t src        = vm_read32(buf + 0x00);
    const uint32_t dst        = vm_read32(buf + 0x04);
    const uint32_t src_size   = vm_read32(buf + 0x08);
    const uint32_t expected   = vm_read32(buf + 0x0C);
    const uint32_t counter_w  = vm_read32(buf + 0x10);
    const uint32_t event_flag = vm_read32(buf + 0x14);
    const uint16_t bits       = (uint16_t)(vm_read32(buf + 0x18) >> 16);
    const uint32_t skips      = vm_read32(buf + 0x1C);
    const uint32_t counter    = counter_w & ~3u;

    bool ok = kz2_edge_inflate(src, src_size, dst, expected, skips >> 16, skips & 0xFFFF,
                               (counter_w & 1) != 0);

    static std::atomic<unsigned> n{0};
    unsigned k = ++n;
    if (k <= 8 || !ok || (k % 500) == 0)
        fprintf(stderr, "[kz2] edge-zlib #%u q=0x%08X %s 0x%08X(%u) -> 0x%08X(%u) counter=0x%08X(%u) flag=0x%08X/0x%04X %s\n",
                k, q, (counter_w & 1) ? "inflate" : "copy", src, src_size, dst, expected,
                counter, counter ? vm_read32(counter) : 0, event_flag, bits, ok ? "ok" : "FAILED");

    /* Work-to-do counter: decremented once per element; the flag is raised
     * by the element that takes it to zero. */
    bool last = true;
    if (counter) {
        std::atomic_ref<uint32_t> word(*(uint32_t*)guest(counter));
        uint32_t old_be = word.load(), next_be;
        do {
            uint32_t v = __builtin_bswap32(old_be);
            next_be = __builtin_bswap32(v ? v - 1 : 0);
            last = (v <= 1);
        } while (!word.compare_exchange_weak(old_be, next_be));
    }
    if (last && event_flag)
        cellSpursEventFlagSet((void*)(uintptr_t)event_flag, bits);

    ctx->gpr[3] = 0;
}

/* ---------------------------------------------------------------------------
 * cellFont, SDK 3.x entry points
 *
 * Killzone 2 initialises the font library with the *WithRevision forms, which
 * the runtime does not register. Unregistered imports return CELL_OK without
 * doing anything, so cellFontInit never ran (every later cellFont call then
 * reported UNINITIALIZED) and the FreeType library pointer stayed NULL --
 * cellFontOpenFontset was handed lib=0. Both forward to the existing
 * implementation; the revision flags only describe the SDK the title was
 * built with.
 * -----------------------------------------------------------------------*/
static void kz2_font_initialize_with_revision(ppu_context* ctx)
{
    /* (u64 revisionFlags, CellFontConfig* config) */
    int32_t rc = cellFontInit((const void*)(uintptr_t)(uint32_t)ctx->gpr[4]);
    if (rc == (int32_t)0x80540004u) rc = 0;   /* ALREADY_INITIALIZED: harmless */
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

static void kz2_font_init_library_freetype_with_revision(ppu_context* ctx)
{
    /* (u64 revisionFlags, const CellFontLibraryConfigFT* config,
     *  const CellFontLibrary** lib) */
    const uint32_t lib_out = (uint32_t)ctx->gpr[5];
    if (!lib_out) { ctx->gpr[3] = 0x80540002u; return; }   /* INVALID_PARAMETER */
    static uint32_t lib = 0;
    if (!lib) lib = kz2_guest_alloc(0x40, 0x10);
    vm_write32(lib_out, lib);
    fprintf(stderr, "[kz2] cellFontInitLibraryFreeTypeWithRevision -> lib=0x%08X\n", lib);
    ctx->gpr[3] = 0;
}

/* cellFontGetCharGlyphMetrics(CellFont* font, u32 code, CellFontGlyphMetrics* m):
 * the unscaled-outline form of the metrics query; the runtime only has the
 * render (scaled) form, which fills the same struct. */
static void kz2_font_get_char_glyph_metrics(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)(int64_t)cellFontGetRenderCharGlyphMetrics(
        (void*)(uintptr_t)(uint32_t)ctx->gpr[3], (uint32_t)ctx->gpr[4],
        (void*)(uintptr_t)(uint32_t)ctx->gpr[5]);
}

/* cellFontOpenFontset(CellFontLibrary* lib, CellFontType* type, CellFont* font)
 *
 * The runtime's version hands back an empty slot ("system fonts are not
 * available in recomp"), so every glyph is blank. The firmware fonts are in
 * the user's own dev_flash ($PS3_DEV_FLASH, RPCS3's by default): open the
 * matching SCE-PS3 TrueType file through cellFontOpenFontFile, which reads a
 * host path. Killzone 2 asks for type 0x109; anything that is not an explicit
 * light/bold request gets Rodin Sans Serif Latin. */
static void kz2_font_open_fontset(ppu_context* ctx)
{
    const uint32_t type_ea = (uint32_t)ctx->gpr[4];
    const uint32_t font_ea = (uint32_t)ctx->gpr[5];
    const uint32_t type = type_ea ? vm_read32(type_ea) : 0;
    const char* file = "SCE-PS3-RD-R-LATIN.TTF";
    if ((type & 0xFF) == 0x01) file = "SCE-PS3-RD-L-LATIN.TTF";
    if ((type & 0xFF) == 0x02) file = "SCE-PS3-RD-B-LATIN.TTF";
    const char* flash = getenv("PS3_DEV_FLASH");
    char host[600];
    snprintf(host, sizeof host, "%s/data/font/%s", flash && *flash ? flash : ".", file);
    const uint32_t path_ea = kz2_guest_alloc((uint32_t)strlen(host) + 1, 16);
    memcpy(vm_base + path_ea, host, strlen(host) + 1);
    int32_t rc = cellFontOpenFontFile((void*)(uintptr_t)font_ea,
                                      (const char*)(uintptr_t)path_ea, 0, 0);
    if (rc == 0) vm_write32(font_ea + 4, type);   /* CellFont +4: font type */
    fprintf(stderr, "[kz2] cellFontOpenFontset(type=0x%X) -> %s rc=0x%X\n", type, host, (uint32_t)rc);
    ctx->gpr[3] = (uint64_t)(int64_t)rc;
}

/* ---------------------------------------------------------------------------
 * cellSaveDataUserAutoSave: nine arguments
 *
 * (version, userId, dirName, errDialog, setBuf, funcStat, funcFile, container,
 *  userdata) -- the ninth does not fit in r3..r10 and is the caller's
 * parameter-save-area doubleword at SP+0x70. The generated 8-argument bridge
 * called the stat/file callbacks with userdata = NULL; Killzone 2's callbacks
 * dereference it as their save-job object, so its profile load/save state
 * machine wrote into the null page and never finished.
 * (cellSaveDataUserAutoLoad has a correct ctx handler in the runtime, which
 * nothing registers; cellSaveData_register_ctx_handlers() below does.)
 * -----------------------------------------------------------------------*/
static void kz2_savedata_user_auto_save(ppu_context* ctx)
{
    const uint32_t userdata = (uint32_t)vm_read64((uint32_t)ctx->gpr[1] + 0x70);
    fprintf(stderr, "[kz2] cellSaveDataUserAutoSave(user=%u, userdata=0x%08X)\n",
            (uint32_t)ctx->gpr[4], userdata);
    ctx->gpr[3] = (uint64_t)(int64_t)cellSaveDataAutoSave2(
        (uint32_t)ctx->gpr[3], (const char*)(uintptr_t)(uint32_t)ctx->gpr[5],
        (uint32_t)ctx->gpr[6], (void*)(uintptr_t)(uint32_t)ctx->gpr[7],
        (void*)(uintptr_t)(uint32_t)ctx->gpr[8], (void*)(uintptr_t)(uint32_t)ctx->gpr[9],
        (uint32_t)ctx->gpr[10], (void*)(uintptr_t)userdata);
}

extern "C" void kz2_hle_register(void)
{
    cellSaveData_register_ctx_handlers();          /* 9-arg cellSaveDataUserAutoLoad */
    ps3_hle_register_ctx(0xA885CC9B, "cellFontOpenFontset", kz2_font_open_fontset);
    ps3_hle_register_ctx(0x52AAC4FA, "cellSaveDataUserAutoSave", kz2_savedata_user_auto_save);
    ps3_hle_register_ctx(0xD8EAEE9F, "cellFontGetCharGlyphMetrics",
                         kz2_font_get_char_glyph_metrics);
    ps3_hle_register_ctx(0xF03DCC29, "cellFontInitializeWithRevision",
                         kz2_font_initialize_with_revision);
    ps3_hle_register_ctx(0x7A0A83C4, "cellFontInitLibraryFreeTypeWithRevision",
                         kz2_font_init_library_freetype_with_revision);
    ps3_hle_register_ctx(0x8A85674D, "_cellSpursLFQueuePushBody", kz2_lfqueue_push_body);
}
