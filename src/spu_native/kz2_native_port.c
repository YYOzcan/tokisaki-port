/* tokisaki: the native gates' table (Project Pyrrhus defines it in its generated spu_workloads.c; here the gates are
 * inserted into the lifted job modules by tools/spu_native_gates.py from tools/spu_native_gates.tsv) and the float
 * switches kz2_native_port.h declares. */
#include "kz2_native_jobs.h"

int (*g_kz2_native_gate[KZ2_NATIVE_SLOTS])(spu_context*);
int g_kz3_spu_xr = 1, g_kz3_spu_slowcg = 0;
