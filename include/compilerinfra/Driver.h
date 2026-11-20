#ifndef COMPILERINFRA_DRIVER_H
#define COMPILERINFRA_DRIVER_H

#include "compilerinfra/Compiler.h"

namespace compilerinfra {
// Process entry, called once. A factory can consume product LLVM command-line
// options after parsing; construction must not probe devices for --list.
int driverMain(int argc, const char *const *argv,
               llvm::function_ref<llvm::Expected<Compiler>()> makeCompiler);
int driverMain(int argc, const char *const *argv, const Compiler &compiler);
} // namespace compilerinfra

#endif
