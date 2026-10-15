/*
 * Tokisaki Port -- register the WWS job manager policy module.
 *
 * Every Killzone 2 SPURS workload ("Wws_Job" / jm_default, jm_anim, jm_gfx,
 * jm_particles, jm_havok, jm_amadeus ...) runs the same raw policy module,
 * linked into the EBOOT at guest 0x00D8B000, 0x2E80 bytes, entered at LS
 * 0xA00. tools/lift_spu.sh wraps and lifts it into
 * spu_recompiled/pm_DDD59DC528B4CED6; cellSpurs resolves a workload's PM by
 * the FNV-1a-64 of those bytes, which is the name of the directory.
 *
 * Built only when that lift exists (see CMakeLists.txt).
 */
#include "spu_workload.h"

extern void spu_begin_image(int image_id);
extern void pm_DDD59DC528B4CED6_spu_func_00000A00(spu_context*);
extern void pm_DDD59DC528B4CED6_spu_recomp_register(void);

#define KZ2_PM_IMAGE_ID 100   /* SPU ELF images use 1..23, job modules 200.. */

__attribute__((constructor(200))) static void kz2_spu_pm_register(void)
{
    spu_begin_image(KZ2_PM_IMAGE_ID);
    pm_DDD59DC528B4CED6_spu_recomp_register();
    spu_workload_register_img(0xDDD59DC528B4CED6ULL,
                              pm_DDD59DC528B4CED6_spu_func_00000A00,
                              KZ2_PM_IMAGE_ID, "wws_job_pm");
    spu_begin_image(0);
}
