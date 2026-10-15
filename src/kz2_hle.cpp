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

extern "C" void kz2_hle_register(void)
{
    ps3_hle_register_ctx(0x8A85674D, "_cellSpursLFQueuePushBody", kz2_lfqueue_push_body);
}
