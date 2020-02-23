#ifndef LLVM_EXAMPLES_THINLTOJIT_ERRORS_H
#define LLVM_EXAMPLES_THINLTOJIT_ERRORS_H

#include "llvm/IR/GlobalValue.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/Error.h"

#include <memory>
#include <string>
#include <system_error>
#include <vector>

namespace llvm {

class Module;
class DataLayout;
class raw_ostream;

namespace orc {

class ModuleIndex;

class IncompatibleDataLayout : public ErrorInfo<IncompatibleDataLayout> {
public:
  static char ID;

  IncompatibleDataLayout(Module &M, DataLayout &DL);
  void log(raw_ostream &OS) const override;

  std::error_code convertToErrorCode() const override {
    return inconvertibleErrorCode();
  }

private:
  std::string ModuleName;
  std::string ActualDataLayout;
  std::string ExpectedDataLayout;
};

class SummaryNotFound : public ErrorInfo<SymbolsNotFound> {
public:
  static char ID;

  SummaryNotFound(std::string UnmangledName, GlobalValue::GUID Guid,
                  const ModuleIndex &GlobalIndex);
  void log(raw_ostream &OS) const override;

  std::error_code convertToErrorCode() const override {
    return inconvertibleErrorCode();
  }

private:
  GlobalValue::GUID Guid;
  std::string UnmangledName;
  std::vector<std::string> ProvidedModules;
};

class SourceManagerDiagnostic : public ErrorInfo<SourceManagerDiagnostic> {
public:
  static char ID;

  SourceManagerDiagnostic(SMDiagnostic Err) : Err(std::move(Err)) {}
  void log(raw_ostream &OS) const override;

  std::error_code convertToErrorCode() const override {
    return inconvertibleErrorCode();
  }

private:
  SMDiagnostic Err;
};

}
}

#endif
