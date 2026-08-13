#ifndef KALEIDOSCOPE_OBJECTEMITTER_H
#define KALEIDOSCOPE_OBJECTEMITTER_H

#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <memory>
#include <string>

namespace kaleidoscope {

/// ObjectEmitter - The Chapter 8 backend: lower a Module to a native .o.
///
/// Kept separate from CodeGen because it is one of two possible consumers of a
/// finished module (the other being the JIT). CodeGen itself stays unaware of
/// which one it is feeding -- it only needs a DataLayout.
class ObjectEmitter {
public:
  /// Registers all targets/MCs/asm printers. Call once, before creating a
  /// TargetMachine.
  static void initializeTargets();

  /// Builds a TargetMachine for the host. Returns null and fills Error on
  /// failure.
  static std::unique_ptr<llvm::TargetMachine>
  createHostTargetMachine(std::string &Error);

  /// Runs the codegen pipeline, writing Mod to Filename as an object file.
  /// Returns false and fills Error on failure.
  static bool emit(llvm::Module &Mod, llvm::TargetMachine &TM,
                   const std::string &Filename, std::string &Error);
};

} // namespace kaleidoscope

#endif
