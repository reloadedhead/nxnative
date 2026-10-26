#pragma once
#include "a32host.h"
typedef struct { const char *name; void (*fn)(Frame *); } ForceEnt;
extern const ForceEnt a32_force[]; extern const unsigned a32_nforce;
extern const ForceEnt a32_force_movie[]; extern const unsigned a32_nforce_movie;
extern const ForceEnt a32_force_audren[]; extern const unsigned a32_nforce_audren;
