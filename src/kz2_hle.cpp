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
#include <ctime>
#include <sched.h>
#include <initializer_list>

#include <zlib.h>
#include <vector>

#include "stb_truetype.h"   /* implementation is compiled into the runtime (libs/font/cellFont.c) */

typedef void (*hle_ctx_fn)(ppu_context*);
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn);
extern "C" int32_t cellSpursEventFlagSet(void* event_flag_ea, uint16_t bits);
extern "C" uint8_t* vm_base;
extern "C" int32_t cellFontInit(const void* config);
extern "C" int32_t cellFontGetRenderCharGlyphMetrics(void* font, uint32_t code, void* metrics);
extern "C" void cellSaveData_register_ctx_handlers(void);
extern "C" int32_t cellFontOpenFontFile(void* font, const char* path, uint32_t sub, int32_t uid);
extern "C" int32_t cellVdecQueryAttr(const void* type, void* attr);
extern "C" int32_t cellVdecOpen(const void* type, const void* res, const void* cb, void* handle);
extern "C" int32_t cellAdecOpen(const void* type, const void* res, const void* cb, void* handle);
extern "C" int32_t cellSaveDataAutoSave2(uint32_t version, const char* dirName, uint32_t errDialog,
                                         void* setBuf, void* funcStat, void* funcFile,
                                         uint32_t container, void* userdata);

/* ---------------------------------------------------------------------------
 * Guest scratch memory for structures these handlers hand to the title.
 *
 * 0xE9000000 is above the guest stacks (0xD0000000..0xE0000000) and next to
 * the runtime's GCM window, which boot_main.cpp moves to 0xE8000000. The
 * title maps nothing up here: its mmapper heap grows upward from 0x11000000
 * (and ran over the old 0x2F000000 home), sys_memory is at 0x40000000, RSX
 * local memory at 0xC0000000. Bump-allocated, never freed -- only a handful
 * of small objects are ever created.
 * -----------------------------------------------------------------------*/
static uint32_t kz2_guest_alloc(uint32_t size, uint32_t align)
{
    static std::atomic<uint32_t> next{0xE9000000u};
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

struct Kz2EdgeReq {
    uint32_t q, src, dst, src_size, expected, counter_w, event_flag, skips;
    uint16_t bits;
};

/* One queue element: inflate or copy, then count down and raise the flag. */
static void kz2_edge_service(const Kz2EdgeReq& r)
{
    const uint32_t q = r.q, src = r.src, dst = r.dst, src_size = r.src_size;
    const uint32_t expected = r.expected, counter_w = r.counter_w, event_flag = r.event_flag;
    const uint32_t skips = r.skips;
    const uint16_t bits = r.bits;
    const uint32_t counter = counter_w & ~3u;
    bool ok = kz2_edge_inflate(src, src_size, dst, expected, skips >> 16, skips & 0xFFFF,
                               (counter_w & 1) != 0);

    static std::atomic<unsigned> n{0};
    unsigned k = ++n;
    /* KZ2_EDGE_LOG=1: every request (who filled which buffer, and when). */
    static const bool log_all = getenv("KZ2_EDGE_LOG") != nullptr;
    if (log_all || k <= 8 || !ok || (k % 500) == 0) {
        fprintf(stderr, "[kz2] edge-zlib #%u q=0x%08X %s 0x%08X(%u) -> 0x%08X(%u) counter=0x%08X(%u) flag=0x%08X/0x%04X skip=%u/%u %s",
                k, q, (counter_w & 1) ? "inflate" : "copy", src, src_size, dst, expected,
                counter, counter ? vm_read32(counter) : 0, event_flag, bits,
                skips >> 16, skips & 0xFFFF, ok ? "ok" : "FAILED");
        if (log_all) {   /* nonzero bytes per 16 KB slice of what was written */
            const uint32_t out = expected - (skips >> 16) - (skips & 0xFFFF);
            fprintf(stderr, " nz");
            for (uint32_t o = 0; o < out; o += 0x4000) {
                uint32_t nz = 0;
                for (uint32_t b = o; b < o + 0x4000 && b < out; b++) nz += guest(dst)[b] != 0;
                fprintf(stderr, " %u", nz);
            }
        }
        fprintf(stderr, "\n");
    }

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

}

/* KZ2_EDGE_ASYNC=1: service the elements on a host worker in push order, as
 * the SPU task does, instead of inside the push. FIOS does its own
 * bookkeeping after the push returns; finishing the element before that may
 * be what leaves a cached block unaccounted for (PROGRESS section 27). */
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
static std::mutex s_edge_lock;
static std::condition_variable s_edge_cv;
static std::deque<Kz2EdgeReq> s_edge_q;
static void kz2_edge_worker()
{
    for (;;) {
        Kz2EdgeReq r;
        {
            std::unique_lock<std::mutex> g(s_edge_lock);
            s_edge_cv.wait(g, [] { return !s_edge_q.empty(); });
            r = s_edge_q.front();
            s_edge_q.pop_front();
        }
        kz2_edge_service(r);
    }
}

static void kz2_lfqueue_push_body(ppu_context* ctx)
{
    const uint32_t q   = (uint32_t)ctx->gpr[3];
    const uint32_t buf = (uint32_t)ctx->gpr[4];
    Kz2EdgeReq r;
    r.q          = q;
    r.src        = vm_read32(buf + 0x00);
    r.dst        = vm_read32(buf + 0x04);
    r.src_size   = vm_read32(buf + 0x08);
    r.expected   = vm_read32(buf + 0x0C);
    r.counter_w  = vm_read32(buf + 0x10);
    r.event_flag = vm_read32(buf + 0x14);
    r.bits       = (uint16_t)(vm_read32(buf + 0x18) >> 16);
    r.skips      = vm_read32(buf + 0x1C);

    static int async = -1;
    if (async < 0) { const char* e = getenv("KZ2_EDGE_ASYNC"); async = (e && atoi(e)) ? 1 : 0; }
    if (async) {
        static std::once_flag once;
        std::call_once(once, [] { std::thread(kz2_edge_worker).detach(); });
        {
            std::lock_guard<std::mutex> g(s_edge_lock);
            s_edge_q.push_back(r);
        }
        s_edge_cv.notify_one();
    } else {
        kz2_edge_service(r);
    }
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
 * cellFontGenerateCharGlyph(CellFont* font, u32 code, CellFontGlyph** glyph)
 *
 * The title rasterises some text itself from glyph OUTLINES -- the
 * language-select names in Cyrillic and other scripts its own fonts lack --
 * into a 512x512 glyph-cache texture. The runtime has no outline API, so the
 * cache stayed empty and those strings never drew. Outlines come from the same
 * firmware TTF cellFontOpenFontset opened, scaled to the font's pixel scale
 * (CellFont +8/+12, set by cellFontSetScalePixel), y up, FreeType tags
 * (1 = on curve, 0 = conic control, 2 = cubic control).
 *
 * CellFontGlyph (64 bytes): u16 CF_type, u16 type, u32 size,
 *   Metrics { f32 width, height; Horizontal {bearingX, bearingY, advance};
 *             Vertical {bearingX, bearingY, advance} }            @ +0x08
 *   Outline { s16 contoursCount, pointsCount; u32 Points, pointTags,
 *             contourIndexs, flags, generateEnv }                 @ +0x28
 * followed here by the point, tag and contour arrays in the same block.
 * -----------------------------------------------------------------------*/
static stbtt_fontinfo s_kz2_font;
static std::vector<unsigned char> s_kz2_font_data;

static bool kz2_font_ready(void)
{
    if (!s_kz2_font_data.empty()) return true;
    const char* flash = getenv("PS3_DEV_FLASH");
    char path[600];
    snprintf(path, sizeof path, "%s/data/font/SCE-PS3-RD-R-LATIN.TTF", flash && *flash ? flash : ".");
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    s_kz2_font_data.resize((size_t)n);
    size_t got = fread(s_kz2_font_data.data(), 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n ||
        !stbtt_InitFont(&s_kz2_font, s_kz2_font_data.data(),
                        stbtt_GetFontOffsetForIndex(s_kz2_font_data.data(), 0))) {
        s_kz2_font_data.clear();
        return false;
    }
    return true;
}

static inline void wf32(uint32_t ea, float v) { uint32_t u; memcpy(&u, &v, 4); vm_write32(ea, u); }

static void kz2_font_generate_char_glyph(ppu_context* ctx)
{
    const uint32_t font_ea = (uint32_t)ctx->gpr[3];
    const uint32_t code    = (uint32_t)ctx->gpr[4];
    const uint32_t out_ea  = (uint32_t)ctx->gpr[5];
    if (!out_ea || !kz2_font_ready()) { ctx->gpr[3] = 0x80540002u; return; }

    float sy = 0;
    if (font_ea) { uint32_t u = vm_read32(font_ea + 12); memcpy(&sy, &u, 4); }
    if (!(sy > 0.5f && sy < 512.0f)) sy = 32.0f;
    const float scale = stbtt_ScaleForMappingEmToPixels(&s_kz2_font, sy);

    const int gi = stbtt_FindGlyphIndex(&s_kz2_font, (int)code);
    stbtt_vertex* v = nullptr;
    const int nv = gi ? stbtt_GetGlyphShape(&s_kz2_font, gi, &v) : 0;

    std::vector<float> px, py; std::vector<uint8_t> tag; std::vector<uint16_t> cont;
    for (int i = 0; i < nv; i++) {
        switch (v[i].type) {
        case STBTT_vmove:
            if (!px.empty()) cont.push_back((uint16_t)(px.size() - 1));
            px.push_back(v[i].x * scale); py.push_back(v[i].y * scale); tag.push_back(1); break;
        case STBTT_vline:
            px.push_back(v[i].x * scale); py.push_back(v[i].y * scale); tag.push_back(1); break;
        case STBTT_vcurve:
            px.push_back(v[i].cx * scale); py.push_back(v[i].cy * scale); tag.push_back(0);
            px.push_back(v[i].x * scale);  py.push_back(v[i].y * scale);  tag.push_back(1); break;
        case STBTT_vcubic:
            px.push_back(v[i].cx * scale);  py.push_back(v[i].cy * scale);  tag.push_back(2);
            px.push_back(v[i].cx1 * scale); py.push_back(v[i].cy1 * scale); tag.push_back(2);
            px.push_back(v[i].x * scale);   py.push_back(v[i].y * scale);   tag.push_back(1); break;
        }
    }
    if (!px.empty()) cont.push_back((uint16_t)(px.size() - 1));
    if (v) stbtt_FreeShape(&s_kz2_font, v);

    const uint32_t np = (uint32_t)px.size(), nc = (uint32_t)cont.size();
    const uint32_t pts_off = 0x40, tag_off = pts_off + np * 8;
    const uint32_t con_off = (tag_off + np + 1) & ~1u;
    const uint32_t size    = (con_off + nc * 2 + 15) & ~15u;
    const uint32_t g = kz2_guest_alloc(size, 16);

    int adv = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    if (gi) {
        stbtt_GetGlyphHMetrics(&s_kz2_font, gi, &adv, &lsb);
        stbtt_GetGlyphBox(&s_kz2_font, gi, &x0, &y0, &x1, &y1);
    }
    vm_write16(g + 0, 0); vm_write16(g + 2, 0); vm_write32(g + 4, size);
    wf32(g + 0x08, (x1 - x0) * scale);   /* width   */
    wf32(g + 0x0C, (y1 - y0) * scale);   /* height  */
    wf32(g + 0x10, x0 * scale);          /* Horizontal.bearingX */
    wf32(g + 0x14, y1 * scale);          /* Horizontal.bearingY */
    wf32(g + 0x18, adv * scale);         /* Horizontal.advance  */
    wf32(g + 0x1C, -(x1 - x0) * scale * 0.5f);  /* Vertical.bearingX */
    wf32(g + 0x20, 0.0f);                /* Vertical.bearingY */
    wf32(g + 0x24, sy);                  /* Vertical.advance  */
    vm_write16(g + 0x28, (uint16_t)nc);
    vm_write16(g + 0x2A, (uint16_t)np);
    vm_write32(g + 0x2C, np ? g + pts_off : 0);
    vm_write32(g + 0x30, np ? g + tag_off : 0);
    vm_write32(g + 0x34, nc ? g + con_off : 0);
    vm_write32(g + 0x38, 0);
    vm_write32(g + 0x3C, 0);
    for (uint32_t i = 0; i < np; i++) { wf32(g + pts_off + i * 8, px[i]); wf32(g + pts_off + i * 8 + 4, py[i]); }
    for (uint32_t i = 0; i < np; i++) vm_write8(g + tag_off + i, tag[i]);
    for (uint32_t i = 0; i < nc; i++) vm_write16(g + con_off + i * 2, cont[i]);
    vm_write32(out_ea, g);

    static int n = 0;
    if (n++ < 12)
        fprintf(stderr, "[kz2] GenerateCharGlyph(U+%04X, %.1fpx) -> 0x%08X: %u contours, %u points\n",
                code, sy, g, nc, np);
    ctx->gpr[3] = 0;
}

/* cellFontDeleteGlyph(CellFont*, CellFontGlyph*): the scratch heap is
 * bump-allocated; the handful of glyphs the title generates are not reused. */
static void kz2_font_delete_glyph(ppu_context* ctx) { ctx->gpr[3] = 0; }

/* ---------------------------------------------------------------------------
 * SPURS task exit-code containers (Havok's hkSpuUtil sets one on every task).
 * Not implemented by the runtime; logged here while the task-exit protocol
 * is being worked out. CellSpursTaskExitCode is 128 bytes.
 * -----------------------------------------------------------------------*/
static volatile uint32_t s_kz2_exitcode_ea;
extern "C" uint32_t kz2_last_exitcode_container(void) { return s_kz2_exitcode_ea; }

static void kz2_task_exitcode_initialize(ppu_context* ctx)
{
    const uint32_t ea = (uint32_t)ctx->gpr[3];
    if (ea) for (uint32_t o = 0; o < 128; o += 4) vm_write32(ea + o, 0);
    s_kz2_exitcode_ea = ea;
    static int n = 0;
    if (n++ < 8) fprintf(stderr, "[kz2] cellSpursTaskExitCodeInitialize(0x%08X)\n", ea);
    ctx->gpr[3] = 0;
}

static void kz2_task_attr_set_exitcode(ppu_context* ctx)
{
    const uint32_t attr = (uint32_t)ctx->gpr[3], ea = (uint32_t)ctx->gpr[4];
    static int n = 0;
    if (n++ < 8) fprintf(stderr, "[kz2] cellSpursTaskAttributeSetExitCodeContainer(attr=0x%08X, 0x%08X)\n", attr, ea);
    ctx->gpr[3] = 0;
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

/* ---------------------------------------------------------------------------
 * KZ2_SELFTEST=1: call lifted guest libc routines on known inputs before the
 * title starts, and compare with the host's answer. A lifter bug in a string
 * routine corrupts everything built on it silently; this names it.
 * -----------------------------------------------------------------------*/
static uint32_t kz2_put_str(const char* s)
{
    uint32_t ea = kz2_guest_alloc((uint32_t)strlen(s) + 1, 16);
    memcpy(vm_base + ea, s, strlen(s) + 1);
    return ea;
}

static uint32_t kz2_call2(void (*fn)(ppu_context*), uint32_t a, uint32_t b)
{
    static uint32_t stack = 0;
    if (!stack) stack = kz2_guest_alloc(0x10000, 0x100) + 0xFF00;
    ppu_context c;
    memset(&c, 0, sizeof c);
    c.gpr[1] = stack;
    c.gpr[2] = 0x00EB2958;       /* module TOC */
    c.gpr[3] = a;
    c.gpr[4] = b;
    fn(&c);
    return (uint32_t)c.gpr[3];
}

static uint32_t kz2_call3(void (*fn)(ppu_context*), uint32_t a, uint32_t b, uint32_t c3)
{
    static uint32_t stack = 0;
    if (!stack) stack = kz2_guest_alloc(0x10000, 0x100) + 0xFF00;
    ppu_context c;
    memset(&c, 0, sizeof c);
    c.gpr[1] = stack; c.gpr[2] = 0x00EB2958;
    c.gpr[3] = a; c.gpr[4] = b; c.gpr[5] = c3;
    fn(&c);
    return (uint32_t)c.gpr[3];
}

static void kz2_selftest_url(void)
{
    const uint32_t url  = kz2_put_str("file:///kin/index.jsp");
    const uint32_t base = kz2_guest_alloc(0x400, 16);
    for (int which = 0; which < 2; which++) {
        const uint32_t out = kz2_guest_alloc(0x400, 16);
        uint32_t rc = which ? kz2_call3(func_0039C230, out, base, url)
                            : kz2_call3(func_0039BDB0, out, base, url);
        fprintf(stderr, "[selftest] %s(file:///kin/index.jsp) rc=%u scheme='%s' host/path='%s' port=%u\n",
                which ? "parse" : "parse_abs", rc, (char*)(vm_base + out),
                (char*)(vm_base + out + 0x10), vm_read16(out + 0x90));
        fprintf(stderr, "[selftest]   out:");
        for (int o = 0; o < 0x40; o += 4) fprintf(stderr, " %08X", vm_read32(out + o));
        fprintf(stderr, "\n");
    }
}

static void kz2_selftest(void)
{
    kz2_selftest_url();
    const char* urls[] = { "file:///kin/index.jsp", "http://x/y", "abc:def", "nocolon" };
    for (const char* u : urls) {
        uint32_t s = kz2_put_str(u);
        for (char ch : { ':', '/', 'j', 'z' }) {
            uint32_t got = kz2_call2(func_007AE780, s, (uint32_t)(uint8_t)ch);
            const char* want = strchr(u, ch);
            uint32_t want_ea = want ? s + (uint32_t)(want - u) : 0;
            fprintf(stderr, "[selftest] strchr(\"%s\", '%c') = 0x%08X want 0x%08X %s\n",
                    u, ch, got, want_ea, got == want_ea ? "ok" : "MISMATCH");
        }
        for (const char* needle : { "://", ":", "//", "jsp" }) {
            uint32_t n = kz2_put_str(needle);
            uint32_t got = kz2_call2(func_007AF780, s, n);
            const char* want = strstr(u, needle);
            uint32_t want_ea = want ? s + (uint32_t)(want - u) : 0;
            fprintf(stderr, "[selftest] strstr(\"%s\", \"%s\") = 0x%08X want 0x%08X %s\n",
                    u, needle, got, want_ea, got == want_ea ? "ok" : "MISMATCH");
        }
    }
}

/* cellVdecQueryAttrEx / cellVdecOpenEx: the intro and attract movies (H.264
 * in AVI) are decoded through the Ex forms, which the runtime does not
 * register. CellVdecTypeEx begins with the same codecType word as
 * CellVdecType (then a pointer to codec-specific info instead of a profile
 * level), and the runtime ignores the resource block, so both forward. */
static void kz2_vdec_query_attr_ex(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)(int64_t)cellVdecQueryAttr(
        (const void*)(uintptr_t)(uint32_t)ctx->gpr[3], (void*)(uintptr_t)(uint32_t)ctx->gpr[4]);
}

static void kz2_vdec_open_ex(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)(int64_t)cellVdecOpen(
        (const void*)(uintptr_t)(uint32_t)ctx->gpr[3], (const void*)(uintptr_t)(uint32_t)ctx->gpr[4],
        (const void*)(uintptr_t)(uint32_t)ctx->gpr[5], (void*)(uintptr_t)(uint32_t)ctx->gpr[6]);
}

/* cellAdecOpenEx: the movies' AC3 track opens through the Ex form, which
 * differs from cellAdecOpen only in the resource block. */
static void kz2_adec_open_ex(ppu_context* ctx)
{
    ctx->gpr[3] = (uint64_t)(int64_t)cellAdecOpen(
        (const void*)(uintptr_t)(uint32_t)ctx->gpr[3], (const void*)(uintptr_t)(uint32_t)ctx->gpr[4],
        (const void*)(uintptr_t)(uint32_t)ctx->gpr[5], (void*)(uintptr_t)(uint32_t)ctx->gpr[6]);
}

/* ---------------------------------------------------------------------------
 * Sound stream stop watchdog (func_009AD268)
 *
 * When the campaign starts, the sound engine halts active streams. Stream 7
 * (menu music, handle 0x84070002) is waited on in func_009AD268 via
 * func_009B6098 (sys_timer_usleep 100ms) indefinitely because its teardown
 * is not completed. After 5 consecutive spin iterations (~500ms), we release
 * the stream record so the title can progress into level loading.
 * -----------------------------------------------------------------------*/
typedef int64_t (*lv2_syscall_fn)(ppu_context* ctx);
static uint64_t ps3_ms_now_kz2(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}
struct lv2_syscall_table {
    lv2_syscall_fn handlers[1024];
};
extern "C" lv2_syscall_table g_lv2_syscalls;

static lv2_syscall_fn s_orig_sys_timer_usleep = nullptr;

static int64_t kz2_sys_timer_usleep(ppu_context* ctx)
{
    const uint32_t caller_lr = (uint32_t)ctx->lr;
    /* KZ2_USLEEP_SPIN_US=<n>: a request of at most n microseconds waits to
     * its deadline yielding the core, as the runtime does on Windows, instead
     * of in nanosleep -- which on Linux takes ~55 us however short the request
     * (the 50 us timer slack). The main thread polls an SPU job counter with
     * sys_timer_usleep(1) in func_004D3D00, ~a quarter of its time. */
    static const long spin_us = [] { const char* e = getenv("KZ2_USLEEP_SPIN_US"); return e ? atol(e) : 0L; }();
    if (spin_us > 0) {
        const uint64_t usec = ctx->gpr[3];
        if (usec <= (uint64_t)spin_us) {
            struct timespec t0, t;
            clock_gettime(CLOCK_MONOTONIC, &t0);
            const long long end = t0.tv_sec * 1000000000ll + t0.tv_nsec + (long long)usec * 1000;
            do { sched_yield(); clock_gettime(CLOCK_MONOTONIC, &t); }
            while (t.tv_sec * 1000000000ll + t.tv_nsec < end);
            ctx->gpr[3] = 0;
            return 0;
        }
    }
    /* Off by default since the SPURS taskset fixes (docs/PROGRESS.md section
     * 47): the menu-music stream now finishes stopping on its own, and the
     * forged stop below left the sound engine's voice lists broken -- the
     * level was silent. KZ2_SOUND_WATCHDOG=1 brings it back. */
    static const bool off = !(getenv("KZ2_SOUND_WATCHDOG") && getenv("KZ2_SOUND_WATCHDOG")[0] == '1');
    if (off) return s_orig_sys_timer_usleep ? s_orig_sys_timer_usleep(ctx) : 0;
    if (caller_lr == 0x009AD294 || caller_lr == 0x009AD2CC) {
        static int s_spin_count[16] = {0};
        const uint32_t stream_idx = (uint32_t)ctx->gpr[29];
        if (stream_idx < 16) {
            s_spin_count[stream_idx]++;
            if (s_spin_count[stream_idx] >= 5) {
                const uint32_t toc = (uint32_t)ctx->gpr[2];
                const uint32_t stream_base_ptr = vm_read32(toc - 0x4358);
                const uint32_t stream_base = stream_base_ptr ? vm_read32(stream_base_ptr) : 0;
                if (stream_base) {
                    const uint32_t rec_ea = stream_base + stream_idx * 0x38D8;
                    const uint32_t h = vm_read32(rec_ea);
                    fprintf(stderr, "[kz2-sound] stream %u spinning in stop wait (spin=%d, handle=0x%08X at 0x%08X), releasing\n",
                            stream_idx, s_spin_count[stream_idx], h, rec_ea);
                    /* Let the stream service thread finish the stop itself:
                     * it holds off while +0x1B4 (reads in flight) is set and
                     * no completion arrives. Clearing just that flag sends it
                     * down its own stop path (func_009B2B98), which releases
                     * the stream's voices properly. The old version forged
                     * the handle and status instead, and the master teardown
                     * then needed the voice list cleared by hand, which
                     * corrupted it (double insert -> fatal loop in
                     * func_009A21D0, now neutralised by tools/postlift_fixups.py).
                     * The gentle variant does not get the stop through yet, so
                     * it is opt-in: KZ2_GENTLE_SOUND_WATCHDOG=1. */
                    static const bool old_wd = getenv("KZ2_GENTLE_SOUND_WATCHDOG") == nullptr;
                    if (old_wd) {
                        vm_write32(rec_ea + 0x00, h & 0x7FFFFFFF);
                        vm_write32(rec_ea + 0x04, 0);
                        vm_write32(rec_ea + 0x1B0, 0);
                        vm_write32(rec_ea + 0x1C0, 0);
                    } else {
                        vm_write32(rec_ea + 0x1B4, 0);
                    }
                    s_spin_count[stream_idx] = 0;
                    ctx->gpr[3] = 0;
                    return 0;
                }
            }
        }
    }
    static const bool no_voice = getenv("KZ2_GENTLE_SOUND_WATCHDOG") != nullptr;
    if (caller_lr == 0x009A31B8 && !no_voice) {
        static int s_spin_teardown = 0;
        s_spin_teardown++;
        if (s_spin_teardown >= 5) {
            const uint32_t list_ptr = (uint32_t)ctx->gpr[31];
            if (list_ptr) {
                const uint32_t head = vm_read32(list_ptr);
                fprintf(stderr, "[kz2-sound] func_009A30F0 spinning in master teardown wait (spin=%d, head=0x%08X at 0x%08X), releasing voice list\n",
                        s_spin_teardown, head, list_ptr);
                vm_write32(list_ptr, 0);
            }
            s_spin_teardown = 0;
            ctx->gpr[3] = 0;
            return 0;
        }
    }
    return s_orig_sys_timer_usleep ? s_orig_sys_timer_usleep(ctx) : 0;
}

/* ---------------------------------------------------------------------------
 * Lost SPU task completions (docs/PROGRESS.md section 50)
 *
 * About one level start in six stalls with a guest thread waiting forever for
 * SPU task completion events that never come: func_004F7668 starts a batch
 * of tasks and counts their completions on an event queue (directly, and in
 * func_004EEA68), and func_004EBD08 services the queue whose events the main
 * thread's func_004EED70 waits for. None of the three looks at the event's
 * contents -- they count. The root cause (a task that never runs, or a send
 * that is lost) is not found yet. Until it is, a wait at those three call
 * sites that has lasted KZ2_LOST_EVENT_S seconds (off by default) while
 * not a single frame was presented gets a synthesized, empty event, logged.
 * In the one stall it met it fired ten times and the level did not come back.
 * -----------------------------------------------------------------------*/
static lv2_syscall_fn s_orig_sys_event_queue_receive = nullptr;
extern "C" unsigned ppu_boot_frames_presented(void);

static int64_t kz2_sys_event_queue_receive(ppu_context* ctx)
{
    const uint32_t caller_lr = (uint32_t)ctx->lr;
    static const unsigned limit_s = [] {
        const char* e = getenv("KZ2_LOST_EVENT_S");
        return e ? (unsigned)strtoul(e, 0, 0) : 0u;     /* off: it did not bring a stall back */
    }();
    const bool watched = caller_lr == 0x004F78A8 || caller_lr == 0x004EEA9C || caller_lr == 0x004EBEFC;
    if (!limit_s || !watched || ctx->gpr[5] != 0)          /* only untimed waits at those sites */
        return s_orig_sys_event_queue_receive(ctx);

    const uint64_t r3 = ctx->gpr[3], r4 = ctx->gpr[4];
    const uint64_t t0 = ps3_ms_now_kz2();
    unsigned frames0 = ppu_boot_frames_presented();
    uint64_t still_since = t0;
    for (;;) {
        ctx->gpr[3] = r3; ctx->gpr[4] = r4; ctx->gpr[5] = 1000000;   /* 1 s slices */
        const int64_t rc = s_orig_sys_event_queue_receive(ctx);
        if ((uint32_t)rc != 0x8001000Bu) { ctx->gpr[5] = 0; return rc; }     /* not ETIMEDOUT */
        const uint64_t now = ps3_ms_now_kz2();
        const unsigned frames = ppu_boot_frames_presented();
        if (frames != frames0) { frames0 = frames; still_since = now; }
        if (now - still_since >= limit_s * 1000ull && now - t0 >= limit_s * 1000ull) {
            const uint32_t ev = (uint32_t)r4;
            if (ev) for (int k = 0; k < 4; k++) vm_write64(ev + 8u * k, 0);
            ctx->gpr[4] = ctx->gpr[5] = ctx->gpr[6] = ctx->gpr[7] = 0;
            ctx->gpr[3] = 0;
            fprintf(stderr, "[kz2-evq] no frame for %u s and queue %u silent at lr=0x%08X: "
                            "synthesized a lost SPU task completion\n",
                    limit_s, (uint32_t)r3, caller_lr);
            return 0;
        }
    }
}

extern "C" void kz2_hle_register(void)
{
    s_orig_sys_timer_usleep = g_lv2_syscalls.handlers[141];
    g_lv2_syscalls.handlers[141] = kz2_sys_timer_usleep;
    s_orig_sys_event_queue_receive = g_lv2_syscalls.handlers[130];
    g_lv2_syscalls.handlers[130] = kz2_sys_event_queue_receive;

    ps3_hle_register_ctx(0x8B5551A4, "cellAdecOpenEx", kz2_adec_open_ex);
    ps3_hle_register_ctx(0xC982A84A, "cellVdecQueryAttrEx", kz2_vdec_query_attr_ex);
    ps3_hle_register_ctx(0x0053E2D8, "cellVdecOpenEx", kz2_vdec_open_ex);
    if (getenv("KZ2_SELFTEST")) kz2_selftest();
    cellSaveData_register_ctx_handlers();          /* 9-arg cellSaveDataUserAutoLoad */
    ps3_hle_register_ctx(0xA885CC9B, "cellFontOpenFontset", kz2_font_open_fontset);
    ps3_hle_register_ctx(0xC17259DE, "cellFontGenerateCharGlyph", kz2_font_generate_char_glyph);
    ps3_hle_register_ctx(0x34552FA6, "cellSpursTaskExitCodeInitialize", kz2_task_exitcode_initialize);
    ps3_hle_register_ctx(0xA121A224, "cellSpursTaskAttributeSetExitCodeContainer", kz2_task_attr_set_exitcode);
    ps3_hle_register_ctx(0xD62F5D76, "cellFontDeleteGlyph", kz2_font_delete_glyph);
    ps3_hle_register_ctx(0x52AAC4FA, "cellSaveDataUserAutoSave", kz2_savedata_user_auto_save);
    ps3_hle_register_ctx(0xD8EAEE9F, "cellFontGetCharGlyphMetrics",
                         kz2_font_get_char_glyph_metrics);
    ps3_hle_register_ctx(0xF03DCC29, "cellFontInitializeWithRevision",
                         kz2_font_initialize_with_revision);
    ps3_hle_register_ctx(0x7A0A83C4, "cellFontInitLibraryFreeTypeWithRevision",
                         kz2_font_init_library_freetype_with_revision);
    ps3_hle_register_ctx(0x8A85674D, "_cellSpursLFQueuePushBody", kz2_lfqueue_push_body);
}


/* ---------------------------------------------------------------------------
 * func_003BD660 (applied by tools/postlift_fixups.py): the main thread's wait
 * for a job batch's progress -- until the smallest of the seven u16 counters
 * at *(u32*)r3 + (r4 << 4) + 0x42..0x4E reaches r5. The lifted loop re-read
 * them flat out, ~40% of the main thread in the level, on a core it shares
 * with an SPU worker (SMT) and out of the laptop's power budget. The same
 * wait here pauses between passes (and yields now and then); the registers it
 * leaves are the ones the guest loop leaves (r0 the minimum, r4 the scaled
 * index, r9 the counter block; the rest are volatile across the call).
 * KZ2_SPIN_WAIT=0 keeps the lifted loop.
 * -------------------------------------------------------------------------*/
#include <sched.h>
#if defined(__x86_64__)
#include <immintrin.h>
#endif
extern "C" int kz2_spu_progress_wait(ppu_context* ctx)
{
    static int on = -1;
    if (on < 0) { const char* e = getenv("KZ2_SPIN_WAIT"); on = !(e && e[0] == '0'); }
    if (!on) return 0;
    auto rd16 = [](uint32_t ea) -> uint32_t {
        uint16_t v; memcpy(&v, vm_base + ea, 2); return __builtin_bswap16(v); };
    const uint32_t r3 = (uint32_t)ctx->gpr[3];
    const uint32_t off = ((uint32_t)ctx->gpr[4] << 4) & 0xFFFFFFF0u;     /* rlwinm r4,r4,4,0,27 */
    const uint32_t target = (uint32_t)ctx->gpr[5];
    ctx->gpr[4] = off;
    for (unsigned pass = 1;; pass++) {
        uint32_t head; memcpy(&head, vm_base + r3, 4);
        const uint32_t base = __builtin_bswap32(head) + off + 0x40u;
        uint32_t m = rd16(base + 2);
        std::atomic_thread_fence(std::memory_order_acquire);           /* lwsync */
        for (uint32_t o = 4; o <= 0xE; o += 2) { const uint32_t v = rd16(base + o); if (v < m) m = v; }
        if (m >= target) {
            ctx->gpr[0] = m; ctx->gpr[9] = base;
            return 1;
        }
#if defined(__x86_64__)
        _mm_pause();
#endif
        /* KZ2_SPIN_SLEEP_US=<us>: after ~4096 passes, sleep this long between
         * passes instead of spinning on -- the core is then free for an SPU
         * worker on its sibling thread. 0 (default): yield every 256. */
        static int sleep_us = -1;
        if (sleep_us < 0) { const char* e = getenv("KZ2_SPIN_SLEEP_US"); sleep_us = e ? atoi(e) : 0; }
        if (sleep_us > 0 && pass >= 4096) {
            struct timespec ts = { 0, (long)sleep_us * 1000L };
            nanosleep(&ts, nullptr);
        } else if ((pass & 255) == 0) sched_yield();
    }
}
