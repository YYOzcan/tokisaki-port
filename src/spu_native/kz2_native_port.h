/* tokisaki: what Project Pyrrhus's runtime gives these kernels and this port's ps3recomp does not -- an
 * spu_ls_dbg_armed() (the local-store debug watches of its runtime; none on the native path here) and the two float
 * switches of the Killzone 3 port's SPU helpers there (its patches 1071, 1079): extended-range lanes on, the fused
 * multiply-add fast path on. This runtime's helpers are exact SPU arithmetic either way. */
#ifndef KZ2_NATIVE_PORT_H
#define KZ2_NATIVE_PORT_H
#ifdef __cplusplus
extern "C" {
#endif
extern int g_kz3_spu_xr, g_kz3_spu_slowcg;
static inline int spu_ls_dbg_armed(void) { return 0; }
#ifdef __cplusplus
}
#endif
#endif
