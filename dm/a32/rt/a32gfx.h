#pragma once
#include "a32force.h"
uint32_t a32_guest_alloc(uint32_t size, uint32_t align);
extern const ForceEnt a32_force_gfx[]; extern const unsigned a32_nforce_gfx;
typedef struct { const char *name; void (*fn)(Frame *); void **impl; } A32NvnEnt;
extern int g_nvn_trace; void a32_nvn_trace(Frame *f);        // A32_NVN=1: every NVN call with its caller and first arguments
