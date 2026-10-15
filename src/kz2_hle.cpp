/*
 * Tokisaki Port -- Killzone 2 specific HLE overrides.
 *
 * Handlers registered here with ps3_hle_register_ctx() run before the generic
 * ps3recomp handlers for the same NID (context handlers are matched first,
 * first registration wins). Keep every override documented with the reason
 * the generic one is not enough for this title.
 */
#include "ppu_recomp.h"

#include <cstdint>
#include <cstdio>

typedef void (*hle_ctx_fn)(ppu_context*);
extern "C" void ps3_hle_register_ctx(uint32_t nid, const char* name, hle_ctx_fn fn);

/* Killzone 2 ships no game-side PRX in USRDIR; every import is firmware. */
extern "C" void ps3_load_prx_modules(void)
{
}

extern "C" void kz2_hle_register(void)
{
}
