#ifndef KALEIDOSCOPE_OBJECTEMITTER_H
#define KALEIDOSCOPE_OBJECTEMITTER_H

#include "llvm/IR/Module.h"
#include "llvm/Target/TargetMachine.h"

#include <memory>
#include <string>

namespace kaleidoscope {

class ObjectEmitter {
public:
  static void initializeTargets();

  static std::unique_ptr<llvm::TargetMachine>
  createHostTargetMachine(std::string &Error);

  static bool emit(llvm::Module &Mod, llvm::TargetMachine &TM,
                   const std::string &Filename, std::string &Error);
};

} // namespace kaleidoscope

#endif
