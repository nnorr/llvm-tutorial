#include "ObjectEmitter.h"

#include "llvm/IR/LegacyPassManager.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Host.h"

using namespace llvm;

namespace kaleidoscope {

void ObjectEmitter::initializeTargets() {
  // The tutorial calls InitializeAll*() here, which forces every backend
  // (AArch64, AMDGPU, RISCV, ...) to be linked in. We only ever emit for the
  // host triple, so the native initializers are enough -- and they keep the
  // link line to the `native` component instead of `all`.
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  InitializeNativeTargetAsmParser();
}

std::unique_ptr<TargetMachine>
ObjectEmitter::createHostTargetMachine(std::string &Error) {
  auto TargetTriple = sys::getDefaultTargetTriple();

  const Target *TheTarget = TargetRegistry::lookupTarget(TargetTriple, Error);
  if (!TheTarget)
    return nullptr;

  const char *CPU = "generic";
  const char *Features = "";
  TargetOptions Opt;

  return std::unique_ptr<TargetMachine>(TheTarget->createTargetMachine(
      TargetTriple, CPU, Features, Opt, Reloc::PIC_));
}

bool ObjectEmitter::emit(Module &Mod, TargetMachine &TM,
                         const std::string &Filename, std::string &Error) {
  Mod.setTargetTriple(TM.getTargetTriple().getTriple());
  Mod.setDataLayout(TM.createDataLayout());

  std::error_code EC;
  raw_fd_ostream Dest(Filename, EC, sys::fs::OF_None);
  if (EC) {
    Error = "Could not open file: " + EC.message();
    return false;
  }

  // Object emission still uses the legacy pass manager -- the new pass manager
  // does not cover the target codegen pipeline.
  legacy::PassManager Pass;
  if (TM.addPassesToEmitFile(Pass, Dest, nullptr, CodeGenFileType::ObjectFile)) {
    Error = "TargetMachine can't emit a file of this type";
    return false;
  }

  Pass.run(Mod);
  Dest.flush();
  return true;
}

} // namespace kaleidoscope
