#pragma once

#include "../../external/arm-recomp-core/profiles/armv4t_gba/runtime_arm_types.h"

// Byte verification diagnostics: addr is the first differing guest byte,
// value is its live value and aux is the AOT template's expected value.
#define RUNTIME_TRACE_CODE_MISMATCH 9u
