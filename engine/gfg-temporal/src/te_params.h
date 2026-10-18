/* GFG Temporal Engine defaults.  Mirrors reference/gfg_temporal_ref/params.py;
 * tests/test_temporal_reference.py keeps both lists equal. */
#ifndef TE_PARAMS_H
#define TE_PARAMS_H

#define TE_DEFAULT_TILE 16
#define TE_DEFAULT_LEVELS 3
#define TE_DEFAULT_COARSE_RANGE 6
#define TE_DEFAULT_ME_BIAS (1.0f / 1024.0f)
#define TE_DEFAULT_STATIC_THR (1.0f / 255.0f)
#define TE_DEFAULT_STATIC_MAX_THR (16.0f / 255.0f)
#define TE_DEFAULT_LIGHT_THR (3.0f / 255.0f)
#define TE_DEFAULT_PROBE 4
#define TE_DEFAULT_CUT_THR (20.0f / 255.0f)
#define TE_DEFAULT_RESIDUAL_THR (6.0f / 255.0f)
#define TE_DEFAULT_OCC_LO 0.04f
#define TE_DEFAULT_OCC_HI 0.12f
#define TE_DEFAULT_SMOOTH_BIAS 0.01f
#define TE_DEFAULT_HYSTERESIS 2
#define TE_DEFAULT_FULL_DENSITY 2

#endif
