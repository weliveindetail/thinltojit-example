#ifndef LLVM_EXAMPLES_THINLTOJIT_THINLTOJIT_H
#define LLVM_EXAMPLES_THINLTOJIT_THINLTOJIT_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/SymbolStringPool.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ThreadPool.h"

#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "Errors.h"
#include "ModuleIndex.h"
#include "OnDemandLayer.h"

namespace llvm {
namespace orc {

class CompileWholeModuleOnDemandLayer;
class IRCompileLayer;
class RTDyldObjectLinkingLayer;

class JITDylib;
class JITTargetMachineBuilder;
class MangleAndInterner;

enum class DispatchMaterialization {
  Never,
  WholeModules,
  Always
};

class ThinLtoJIT {
public:
  ThinLtoJIT(ArrayRef<std::string> InputFiles, StringRef MainFunctionName,
             DispatchMaterialization DispatchPolicy,
             unsigned LookaheadLevels, unsigned NumCompileThreads,
             unsigned NumLoadThreads, unsigned NumDiscoveryThreads,
             bool PrintStats, Error &Err);
  ~ThinLtoJIT();

  ThinLtoJIT(const ThinLtoJIT &) = delete;
  ThinLtoJIT &operator=(const ThinLtoJIT &) = delete;
  ThinLtoJIT(ThinLtoJIT &&) = delete;
  ThinLtoJIT &operator=(ThinLtoJIT &&) = delete;

  using MainFunctionType = int(int, char *[]);

  template <typename FunctionType = MainFunctionType>
  Expected<int> call(StringRef EntryPoint, ArrayRef<std::string> Args) {
    SymbolStringPtr MangledName = (*Mangle)(EntryPoint);

    auto Symbol = ES.lookup({MainJD}, MangledName);
    if (!Symbol)
      return Symbol.takeError();

    JITTargetAddress Addr = Symbol->getAddress();
    auto Ptr = jitTargetAddressToFunction<FunctionType *>(Addr);

    return runAsMain(Ptr, Args, StringRef("ThinLtoJIT"));
  }

private:
  ExecutionSession ES;
  DataLayout DL{""};

  JITDylib *MainJD;
  std::unique_ptr<ModuleIndex> GlobalIndex;
  std::unique_ptr<ThreadPool> CompileThreads;
  std::unique_ptr<ThreadPool> DiscoveryThreads;

  std::unique_ptr<RTDyldObjectLinkingLayer> ObjLinkingLayer;
  std::unique_ptr<IRCompileLayer> CompileLayer;
  std::unique_ptr<CompileWholeModuleOnDemandLayer> OnDemandLayer;

  std::unique_ptr<MangleAndInterner> Mangle;

  void setupCompilePipeline(JITTargetMachineBuilder JTMB,
                            unsigned NumCompileThreads,
                            unsigned NumDiscoveryThreads,
                            unsigned LookaheadLevels,
                            DispatchMaterialization DispatchPolicy);

  Error setupJITDylib(JITDylib *JD, bool PrintStats);

  Expected<JITTargetMachineBuilder> setupTargetUtils(ThreadSafeModule &TSM);
  Error applyDataLayout(ThreadSafeModule &TSM);
  Error applyDataLayout(Module &M);

  using FetchModuleResult = ModuleIndex::FetchModuleResult;
  using ModuleInfo = ModuleIndex::ModuleInfo;

  Error submitWholeModuleBlocking(FetchModuleResult R, bool &NotReady);
  Error submitReexportsBlocking(FetchModuleResult R);
  Error submitCallThroughStubs(StringRef ModulePath, ModuleInfo MI);

  Error runDiscovery(FunctionSummary *Origin, StringRef MangledName,
                     unsigned LookaheadLevels);

  Error generateMissingSymbolsBlocking(
      LookupKind K, JITDylib &JD, const SymbolLookupSet &Symbols);

  ExecutionSession::DispatchMaterializationFunction
  createThreadPoolDispatcher(DispatchMaterialization DispatchPolicy);

  void dispatchToThreadpool(JITDylib &JD,
                            std::unique_ptr<MaterializationUnit> MU);

  static void exitOnLazyCallThroughFailure() {
    errs() << "Compilation failed. Aborting.\n";
    exit(1);
  }
};

} // namespace orc
} // namespace llvm

#endif
