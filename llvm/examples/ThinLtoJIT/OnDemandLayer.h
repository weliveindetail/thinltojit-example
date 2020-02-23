//===- CompileWholeModuleOnDemandLayer.h ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// JIT layer for compiling individual modules on demand.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_COMPILEWHOLEMODULEONDEMANDLAYER_H
#define LLVM_EXECUTIONENGINE_ORC_COMPILEWHOLEMODULEONDEMANDLAYER_H

#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/IndirectionUtils.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/Layer.h"
#include "llvm/ExecutionEngine/Orc/SymbolStringPool.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"

#include <functional>
#include <memory>
#include <vector>

#include "ModuleIndex.h"

namespace llvm {
namespace orc {

class NotifyingCallThroughManager;

class CompileWholeModuleOnDemandLayer : public IRLayer {
  friend class EmitCallThroughStubs;
  friend class EmitReexports;
  friend class EmitWholeModule;
  friend class LoadAndEmitWholeModule;

public:
  using NotifyCallThroughToSymbolFunction =
      unique_function<void(SymbolStringPtr)>;

  using IndirectStubsManagerBuilder =
      std::function<std::unique_ptr<IndirectStubsManager>()>;

  /// Construct a CompileWholeModuleOnDemandLayer.
  CompileWholeModuleOnDemandLayer(
      ExecutionSession &ES, IRLayer &BaseLayer, JITTargetMachineBuilder JTMB,
      ModuleIndex &GlobalIndex, char GlobalManglePrefix,
      JITTargetAddress ErrorHandlerAddr);
  ~CompileWholeModuleOnDemandLayer();

  Error addWholeModule(JITDylib &JD, ThreadSafeModule TSM, VModuleKey Id,
                       bool FromDiscovery);
  Error addReexports(JITDylib &JD, ThreadSafeModule TSM, VModuleKey Id);
  Error addCallThroughStubs(JITDylib &TargetJD, StringRef ModulePath,
                            SymbolFlagsMap SymbolFlags);

  void onNotifyCallThroughToSymbol(NotifyCallThroughToSymbolFunction F);

private:
  IRLayer &BaseLayer;
  std::unique_ptr<NotifyingCallThroughManager> CallThroughManager;
  IndirectStubsManagerBuilder BuildIndirectStubsManager;
  ModuleIndex &GlobalIndex;
  char GlobalManglePrefix;

  void emitStubsOnly(MaterializationResponsibility R, StringRef ModulePath);
  void emitReexports(MaterializationResponsibility R, ThreadSafeModule TSM);
  void emitWholeModule(MaterializationResponsibility R, ThreadSafeModule TSM,
                       bool SubmittedAsynchronously);

  void emit(MaterializationResponsibility R, ThreadSafeModule TSM) override {
    llvm_unreachable("Currently not wired up");
  }

  // Workaround for races where another thread materializes the same module.
  std::vector<MaterializationResponsibility> MaterializationResponsibilitySink;

  struct JITDylibResources {
    JITDylib *TargetJD;
    JITDylib *ShadowJD;
    std::unique_ptr<IndirectStubsManager> ISManager;
  };

  std::vector<JITDylibResources> DylibResources;

  JITDylibResources &getJITDylibResources(JITDylib &MainJD);
  JITDylib *setupShadowJITDylib(JITDylib *MainJD);

  using FilterFunction = std::function<bool(Module &, SymbolStringPtr)>;
  SymbolFlagsMap collectModuleDefinitions(Module &M,
                                          FilterFunction Select = nullptr);

  NotifyCallThroughToSymbolFunction NotifyCallThroughToSymbol;
};

} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_COMPILEWHOLEMODULEONDEMANDLAYER_H
