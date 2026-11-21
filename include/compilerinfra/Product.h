#pragma once
#include "compilerinfra/Assembly.h"

// Implemented by SDK-generated product glue.
void ci_register(compilerinfra::Assembly &assembly);
const char *ci_manifest();
