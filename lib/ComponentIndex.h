#ifndef COMPILERINFRA_LIB_COMPONENTINDEX_H
#define COMPILERINFRA_LIB_COMPONENTINDEX_H

#include "compilerinfra/Assembly.h"
#include "llvm/Support/JSON.h"

namespace compilerinfra::detail {
// Provenance of actual link decisions; never read by lowering or emission.
struct Entry {
  SourceRef source;
  llvm::json::Object declaration;
  std::map<std::string, std::vector<std::string>>
      uses; // target -> missing features
};

struct ComponentIndex {
  std::map<std::string, Entry> entries;
  std::map<std::string, std::string> targetRoutes;
};
} // namespace compilerinfra::detail
#endif
