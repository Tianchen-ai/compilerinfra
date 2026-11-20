#ifndef COMPILERINFRA_COMPARISON_H
#define COMPILERINFRA_COMPARISON_H

#include "compilerinfra/Session.h"

namespace compilerinfra {
struct IRComparison {
  // Both arms start from one bytecode image at this boundary.
  std::string sharedThrough;
  Artifact baseline;
  Artifact candidate;
};

// Import once; execute the common prefix once. Compare the default and edited
// pipelines at range.through in independent sessions. Neither emitter is
// called.
llvm::Expected<IRComparison> compareIR(const Compiler &compiler,
                                       llvm::StringRef target,
                                       llvm::MemoryBufferRef input,
                                       const StageRange &range,
                                       const SessionOptions &candidate);
} // namespace compilerinfra
#endif
