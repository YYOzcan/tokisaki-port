/* Tokisaki Port -- direct mouse look for Killzone 2.
 *
 * The mouse used to drive the right stick, and the title's look code shapes a
 * stick before it turns the view: a dead zone, a response curve, a turn-speed
 * ramp and aim friction, then the turn rate times the frame time. A mouse
 * through that is not a mouse -- small motions vanish in the dead zone, the
 * curve bends the rest, and a fast flick hits the stick's limit. And the pad
 * thread samples every few milliseconds while the game keeps only the last
 * sample of each frame, so most of the motion never arrived.
 *
 * Instead the look code takes the mouse motion itself, once per frame, at the
 * point where it has finished with the stick (two fixups in
 * tools/postlift_fixups.py, in the player aim update func_0016E620):
 *
 *   - kz2_mouse_look_mark, after the call to GetLookInput (func_00080A78):
 *     only the local human player's aim object gets there;
 *   - kz2_mouse_look_apply, where the new pitch is stored and the yaw input is
 *     about to be:
 *       pitch  [sp+0xA0], the new view pitch in radians (the title keeps it
 *              in the aim object at +0x78), clamped to the player's limits
 *              (+0x1A4 min, +0x1A8 max) -- moved by the mouse directly;
 *       yaw    f28, the turn input, a normalised rate: the body turns by
 *              f28 * turn_speed_deg * pi/180 * dt this frame
 *              (func_00180ED8; turn_speed at [[player+0xC4]+0x2A8], dt the
 *              frame time at 0xF2A1D4) -- so a mouse turn of d radians adds
 *              d / (turn_speed * pi/180 * dt).
 *   Measured with the stick held (tools/gdb/look_probe.py): f28 -0.0289 turned
 *   the view 0.0101 rad per 1/15 s frame, turn speed 300 deg/s -- the formula's
 *   0.0101.
 *
 * One count is 0.022 deg times PS3_PAD_MOUSE_SENS (default 3: 0.066 deg, a
 * PC shooter's usual feel), scaled by the aim object's zoom factor (+0x310)
 * when aiming down the sights. KZ2_MOUSE_DIRECT=0 goes back to the stick.
 */
#include "ppu_recomp.h"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" uint8_t* vm_base;
extern "C" int cellPad_take_mouse_motion(int* dx, int* dy, float* sens, int* invert);

namespace {

constexpr float kDegToRad = 0.017453292f;
constexpr float kDegPerCount = 0.022f;
constexpr uint32_t kFrameDt = 0xF2A1D4;      /* the title's frame time (s) */

uint32_t rd32(uint32_t ea) { uint32_t v; std::memcpy(&v, vm_base + ea, 4); return __builtin_bswap32(v); }
float rdf(uint32_t ea) { uint32_t v = rd32(ea); float f; std::memcpy(&f, &v, 4); return f; }
void wrf(uint32_t ea, float f)
{
    uint32_t v; std::memcpy(&v, &f, 4); v = __builtin_bswap32(v);
    std::memcpy(vm_base + ea, &v, 4);
}

bool enabled()
{
    static const int on = [] { const char* e = std::getenv("KZ2_MOUSE_DIRECT"); return !(e && e[0] == '0'); }();
    return on;
}

std::atomic<uint32_t> g_marked{0};   /* aim object that took the local player's look input */

} // namespace

extern "C" void kz2_mouse_look_mark(ppu_context* ctx)
{
    if (enabled()) g_marked.store((uint32_t)ctx->gpr[31], std::memory_order_relaxed);
}

extern "C" void kz2_mouse_look_apply(ppu_context* ctx)
{
    const uint32_t aim = (uint32_t)ctx->gpr[31];
    if (!enabled() || g_marked.load(std::memory_order_relaxed) != aim) return;
    g_marked.store(0, std::memory_order_relaxed);

    int dx, dy, invert; float sens;
    const int live = cellPad_take_mouse_motion(&dx, &dy, &sens, &invert);
    /* KZ2_MOUSE_TEST="dx,dy": a synthetic mouse moving that many counts every
     * frame, for measuring the turn (tools/gdb/angle_find.py). */
    static int tdx = 0, tdy = 0;
    static const bool test = [] { const char* e = std::getenv("KZ2_MOUSE_TEST");
                                  return e && std::sscanf(e, "%d,%d", &tdx, &tdy) == 2; }();
    if (test) { dx = tdx; dy = tdy; }
    else if (!live) return;
    if (dx == 0 && dy == 0) return;

    const uint32_t player = (uint32_t)ctx->gpr[30];
    const uint32_t sp = (uint32_t)ctx->gpr[1];
    float zoom = rdf(aim + 0x310);
    if (!(zoom > 0.01f && zoom <= 1.5f)) zoom = 1.0f;
    const float k = kDegPerCount * sens * kDegToRad * zoom;   /* radians per count */

    /* Yaw: mouse right turns right, which is a negative turn input. */
    const float dt = rdf(kFrameDt);
    const uint32_t settings = rd32(player + 0xC4);
    const float turn = settings ? rdf(settings + 0x2A8) : 0.0f;
    if (dx != 0 && dt > 0.0001f && dt < 1.0f && turn > 1.0f && turn < 100000.0f) {
        const float d = (float)dx * k;
        ctx->fpr[28] = (double)(float)(ctx->fpr[28] - d / (turn * kDegToRad * dt));
    }

    /* Pitch: mouse up looks up (a larger pitch), within the player's limits. */
    if (dy != 0) {
        float p = rdf(sp + 0xA0) + (float)(invert ? dy : -dy) * k;
        float lo = rdf(player + 0x1A4), hi = rdf(player + 0x1A8);
        if (lo > hi) { const float t = lo; lo = hi; hi = t; }
        if (std::isfinite(lo) && std::isfinite(hi) && hi > lo) {
            if (p < lo) p = lo;
            if (p > hi) p = hi;
        }
        if (std::isfinite(p)) wrf(sp + 0xA0, p);
    }
}
