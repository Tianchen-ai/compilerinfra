#pragma once
#include "compilerinfra/Assembly.h"

// Implemented by SDK-generated product glue.
#if defined(__GNUC__)
#pragma GCC visibility push(hidden)
#endif
void ci_register(compilerinfra::Assembly &assembly);
const char *ci_manifest();
#if defined(__GNUC__)
#pragma GCC visibility pop
#endif
