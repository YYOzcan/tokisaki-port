/*
 * Tokisaki Port -- Killzone 2's entity-update fiber jobs, run on the host.
 *
 * The world update hands every entity that wants to think to a small fiber
 * job system (docs/PROGRESS.md section 37):
 *
 *   func_00684560  builds a batch {job OPD 0x66E420, &entity slot} per entity
 *   func_00072438  runs the batch: five fiber contexts (stack, TOC, entry,
 *                  argument) and a switch into the first one
 *   func_0007203C  the switch: save the callee-saved registers, pick the next
 *                  runnable fiber (func_00072680), load its registers -- r1
 *                  included -- and branch to its entry (func_00071F24)
 *   func_0066E420  the job: wait for the parent entity (func_00072600), run
 *                  the entity's update, then end the job (func_00072760),
 *                  which switches to the next fiber and never returns
 *
 * Lifted to C, the switch cannot move to another stack or jump to an entry
 * address: it returns into the next function in the binary instead. No
 * entity job ever ran, so nothing in the level updated after it loaded --
 * the opening cutscene's sequences sat on frame 0 and the camera never
 * moved. tools/postlift_fixups.py routes the three functions here:
 *
 *   kz2_fiber_run_batch  run the jobs one after the other on the calling
 *                        thread, each in its own frame below the caller's
 *   kz2_fiber_job_end    return to kz2_fiber_run_batch
 *   kz2_fiber_wait       return: the batch is sorted so that a parent comes
 *                        before its children (func_00684628), so the flag
 *                        it waits for is already set
 *
 * The Killzone 3 port replaced the same mechanism with host code too (fix
 * list P3, "hand-written context switches").
 */
#include "ppu_recomp.h"
#include <stdio.h>
#include <stdlib.h>

extern "C" void ps3_indirect_call(ppu_context* ctx);
extern "C" PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
/* The thread's current context. lwarx/stwcx. keep their reservation in it
 * (ppu_stwcx32 reads g_active_ctx, not the context the lifted code passes),
 * so a job has to be the active context while it runs, and the caller's has
 * to be restored after: left pointing at a finished job's context, every
 * later stwcx. on the thread failed and the allocator's semaphore spun. */
extern "C" PPU_THREAD_LOCAL ppu_context* g_active_ctx;

extern "C" void kz2_fiber_run_batch(ppu_context* ctx)
{
    const uint32_t sched = (uint32_t)ctx->gpr[3];
    const uint32_t batch = (uint32_t)ctx->gpr[4];
    if (!sched || !batch) return;
    const uint32_t n = vm_read32(batch);
    const uint32_t jobs = vm_read32(batch + 4);
    if (!n || !jobs) return;

    /* The guest's own bookkeeping: current batch, next job (all taken), no
     * running fiber. */
    vm_write32(sched + 0x0, batch);
    vm_write32(sched + 0x4, n);
    vm_write32(sched + 0xC, 0xFFFFFFFFu);

    static int s_said = 0;
    if (s_said < 3) {
        s_said++;
        fprintf(stderr, "[kz2-fiber] running a batch of %u entity jobs on the host\n", n);
    }
    ppu_context* const caller_active = g_active_ctx;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t opd = vm_read32(jobs + 8 * i);
        const uint32_t arg = vm_read32(jobs + 8 * i + 4);
        if (!opd) continue;
        ppu_context job = *ctx;
        job.gpr[1] = (ctx->gpr[1] - 0x400) & ~0xFull;    /* a frame below the caller's */
        job.gpr[2] = vm_read32(opd + 4);
        job.gpr[3] = arg;
        job.gpr[4] = sched;
        job.ctr = vm_read32(opd);
        job.reserve_addr = 0;
        g_active_ctx = &job;
        ps3_indirect_call(&job);
        while (g_trampoline_fn) {
            void (*tf)(void*) = g_trampoline_fn;
            g_trampoline_fn = 0;
            tf(&job);
        }
        g_active_ctx = caller_active;
    }
    ctx->reserve_addr = 0;
}

extern "C" void kz2_fiber_job_end(ppu_context* ctx)
{
    (void)ctx;
}

extern "C" void kz2_fiber_wait(ppu_context* ctx)
{
    const uint32_t flag = (uint32_t)ctx->gpr[4];
    if (flag && !vm_read32(flag)) {
        static int s_said = 0;
        if (s_said < 8) {
            s_said++;
            fprintf(stderr, "[kz2-fiber] job waits on 0x%08X, which is not set yet; continuing\n", flag);
        }
    }
}
