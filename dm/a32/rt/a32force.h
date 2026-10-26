#pragma once
#include "a32host.h"
typedef struct { const char *name; void (*fn)(Frame *); } ForceEnt;
extern const ForceEnt a32_force[]; extern const unsigned a32_nforce;
