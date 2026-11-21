#ifndef COMPILERINFRA_SESSION_H
#define COMPILERINFRA_SESSION_H

#include "compilerinfra/Compiler.h"
#include "llvm/ADT/SmallVector.h"
#include <optional>

namespace compilerinfra {

enum class IRFormat { Assembly, Bytecode };

struct StageRange {
  // Both refer to the output of a named stage. With 'after', the supplied IR
  // must satisfy that stage's boundary. 'through' includes the named stage.
  std::optional<std::string> after;
  std::optional<std::string> through;
};

// Borrowed, read-only view for the duration of the callback.
using Observer =
    llvm::function_ref<llvm::Error(llvm::StringRef, mlir::ModuleOp)>;

// Serial, non-reentrant. Use a separate session and context per concurrent job.
class Session {
public:
  ~Session();
  mlir::MLIRContext &context();
  void printPipeline(llvm::raw_ostream &output) const;
  // Compile source, or parse a checkpoint for range.after. A range.through
  // returns serialized IR; otherwise emit the exit artifact. No file I/O.
  llvm::Expected<Artifact> compile(llvm::MemoryBufferRef input,
                                   const StageRange &range = {},
                                   IRFormat format = IRFormat::Bytecode,
                                   Observer observe = {});
  // Consume the product frontend's existing module without a text round trip.
  llvm::Expected<Artifact> compile(mlir::OwningOpRef<mlir::ModuleOp> module,
                                   const StageRange &range = {},
                                   IRFormat format = IRFormat::Bytecode,
                                   Observer observe = {});

  // In-place entry for native frontends/JITs. Module belongs to context().
  // On failed lowering the caller must discard the partially rewritten module.
  llvm::Error lower(mlir::ModuleOp module, const StageRange &range = {},
                    Observer observe = {});

  static llvm::Error writeIR(mlir::ModuleOp module, IRFormat format,
                             llvm::raw_ostream &output);

private:
  friend class Compiler;
  struct Impl;
  explicit Session(std::unique_ptr<Impl> impl);
  static llvm::Expected<std::unique_ptr<Session>>
  create(std::shared_ptr<const Compiler::Definition> definition,
         const Exit &exit, mlir::MLIRContext *external,
         const SessionOptions &options);
  std::unique_ptr<Impl> impl;
};

} // namespace compilerinfra

#endif
