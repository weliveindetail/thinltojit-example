#include "ThinLtoJIT.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/raw_ostream.h"

#include "ModuleIndex.h"

#define DEBUG_TYPE "thinltojit"

namespace llvm {
namespace orc {

char SummaryNotFound::ID;
char IncompatibleDataLayout::ID;
char SourceManagerDiagnostic::ID;

SummaryNotFound::SummaryNotFound(std::string UnmangledName,
                                 GlobalValue::GUID Guid,
                                 const ModuleIndex &GlobalIndex)
    : Guid(Guid), UnmangledName(std::move(UnmangledName)) {
  ModuleNames Paths = GlobalIndex.getAllModulePaths();
  ProvidedModules.reserve(Paths.size());
  for (StringRef Path : Paths) {
    ProvidedModules.push_back(Path.str());
  }
}

void SummaryNotFound::log(raw_ostream &OS) const {
  OS << "No ValueInfo for '" << UnmangledName << "' (" << Guid
     << ") in provided modules: ";
  bool PrependSeperator = false;
  for (const std::string &M : ProvidedModules) {
    if (PrependSeperator)
      OS << ", ";
    OS << M;
    PrependSeperator = true;
  }
  OS << "\n";
}

IncompatibleDataLayout::IncompatibleDataLayout(Module &M, DataLayout &DL)
    : ModuleName(M.getName().str()),
      ActualDataLayout(M.getDataLayout().getStringRepresentation()),
      ExpectedDataLayout(DL.getStringRepresentation()) {}

void IncompatibleDataLayout::log(raw_ostream &OS) const {
  OS << "Module '" << ModuleName << "' has incompatible data layout\n";
  OS << "  Actual:   " << ActualDataLayout << "\n";
  OS << "  Expected: " << ExpectedDataLayout << "\n";
}

void SourceManagerDiagnostic::log(raw_ostream &OS) const {
  constexpr static bool Colors = false;
  Err.print("ThinLtoJIT", OS, Colors);
}

}
}
