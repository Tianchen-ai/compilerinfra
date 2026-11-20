#ifndef COMPILERINFRA_DEVELOPMENT_H
#define COMPILERINFRA_DEVELOPMENT_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"
#include <memory>
#include <string>

namespace compilerinfra {

// A serial client of the product's persistent development action scope.
// argv contains compilerinfra-dev (or a compatible launcher) and its arguments.
class DevelopmentScope {
public:
  static llvm::Expected<std::unique_ptr<DevelopmentScope>>
  open(llvm::ArrayRef<std::string> argv);
  ~DevelopmentScope();
  DevelopmentScope(const DevelopmentScope &) = delete;
  DevelopmentScope &operator=(const DevelopmentScope &) = delete;

  // "actions" returns the typed catalog, including product-supplied actions.
  llvm::Expected<llvm::json::Value> call(llvm::StringRef action,
                                         llvm::json::Object arguments = {});

private:
  struct Impl;
  explicit DevelopmentScope(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl;
};

} // namespace compilerinfra
#endif
