//===----- CompileWholeModuleOnDemandLayer.cpp ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "OnDemandLayer.h"

#include "llvm/ADT/Triple.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/OrcABISupport.h"
#include "llvm/ExecutionEngine/Orc/LazyReexports.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"

#include "StatsUtils.h"
#include "Utils.h"

#include <chrono>
#include <thread>

#define DEBUG_TYPE "thinltojit"

namespace llvm {
namespace orc {

class NamedMaterializationUnit : public MaterializationUnit {
public:
  NamedMaterializationUnit(
      SymbolFlagsMap Symbols, VModuleKey K, StringRef TypeName,
      CompileWholeModuleOnDemandLayer &Parent)
    : MaterializationUnit(std::move(Symbols), nullptr, K),
      TypeName(TypeName), Parent(Parent) {}

  StringRef getName() const override {
    return TypeName;
  }

protected:
  StringRef TypeName;
  CompileWholeModuleOnDemandLayer &Parent;
};

class EmitReexports : public NamedMaterializationUnit {
public:
  EmitReexports(SymbolFlagsMap SymbolFlags, ThreadSafeModule TSM, VModuleKey K,
                CompileWholeModuleOnDemandLayer &Parent)
    : NamedMaterializationUnit(std::move(SymbolFlags), K, "<EmitReexports>",
                               Parent),
      TSM(std::move(TSM)) {}

private:
  void materialize(MaterializationResponsibility R) override {
    Parent.emitReexports(std::move(R), std::move(TSM));
  }

  void discard(const JITDylib &V, const SymbolStringPtr &Name) override {
    // We picked up the module and it can only happen once. Discard would drop
    // the module's actual definitions forever. This cannot happen.
    llvm_unreachable("EmitReexports cannot be discarded");
  }

  ThreadSafeModule TSM;
};

class EmitWholeModule : public NamedMaterializationUnit {
public:
  EmitWholeModule(SymbolFlagsMap SymbolFlags,
                  ThreadSafeModule TSM, VModuleKey K,
                  CompileWholeModuleOnDemandLayer &Parent,
                  bool SubmittedAsynchronously)
      : NamedMaterializationUnit(std::move(SymbolFlags), K, "<EmitWholeModule>",
                                 Parent),
        TSM(std::move(TSM)), SubmittedAsynchronously(SubmittedAsynchronously) {}

private:
  void materialize(MaterializationResponsibility R) override {
    Parent.emitWholeModule(std::move(R), std::move(TSM),
                           SubmittedAsynchronously);
  }

  void discard(const JITDylib &V, const SymbolStringPtr &Name) override {
    // We picked up the module and it can only happen once. Discard would drop
    // the module's actual definitions forever. This cannot happen.
    llvm_unreachable("EmitWholeModule cannot be discarded");
  }

  ThreadSafeModule TSM;
  bool SubmittedAsynchronously;
};

class LoadAndEmitWholeModule : public NamedMaterializationUnit {
public:
  LoadAndEmitWholeModule(SymbolFlagsMap EmittedCallThroughStubs,
                         StringRef ModulePath, VModuleKey K,
                         CompileWholeModuleOnDemandLayer &Parent)
      : NamedMaterializationUnit(std::move(EmittedCallThroughStubs), K,
                                 "<LoadAndEmitWholeModule>", Parent),
        ModulePath(ModulePath.str()), Discarded(false) {}

  ~LoadAndEmitWholeModule() {
    assert((!Discarded || getSymbols().empty()) &&
           "If we discard one symbol, we must discard them all");
  }

private:
  void materialize(MaterializationResponsibility R) override;

  void discard(const JITDylib &V, const SymbolStringPtr &Name) override {
    // Discard can happen, if this unit was added for synthetic call-through
    // stubs and they are now replaced by reexports. Make sure we discard all
    // or nothing.
    Discarded = true;
  }

  std::string ModulePath;
  bool Discarded;
};

void LoadAndEmitWholeModule::materialize(MaterializationResponsibility R) {
  assert(!Discarded && "Invalidated by another LoadAndEmitWholeModule unit");
  PSTATS("[", ModulePath , "] Load whole module");

  // Wait for the module to finish parsing and pick it up.
  auto TSM = Parent.GlobalIndex.pickUpSingleModuleBlocking(ModulePath);
  if (!TSM) {
    // The module was picked up by someone else in the meantime. The pick-up
    // call blocked until the module was submitted so we are done, but what to
    // do with the MaterializationResponsibility? For now, move them into a sink
    // container so that the assertion fails on shutdown.
    SymbolNameSet Symbols;
    for (const auto &KV : R.getSymbols()) {
      Symbols.insert(KV.first);
    }
    Parent.MaterializationResponsibilitySink.push_back(
        R.delegate(std::move(Symbols)));
    return;
  }

  PSTATS("[", ModulePath , "] Emit whole module");

  // Collect and claim responsibility for all symbols in the module.
  SymbolFlagsMap ModuleSymbols = TSM.withModuleDo([&](Module &M) {
    return Parent.collectModuleDefinitions(M);
  });

#ifndef NDEBUG
  // Inherited symbols should be a subset of the module's symbols.
  for (auto &KV : R.getSymbols())
    assert(ModuleSymbols.find(KV.first) != ModuleSymbols.end() &&
           "Inherited foreign symbol");

  JITDylib &JD = R.getTargetJITDylib();
  LLVM_DEBUG(dbgs() << "[" << ModulePath << "] Adding symbols ("
                    << JD.getName() << "): " << ModuleSymbols << "\n");
  LLVM_DEBUG(dbgs() << "[" << ModulePath << "] Existing symbols: "
                    << R.getSymbols() << "\n");
#endif

  // TODO: Take responsibility for all symbols in the module. They should
  // replace all synthetic weak definitions inherited from the
  // EmitCallThroughStubs unit.

  static constexpr bool SubmittedAsynchronously = false;
  Parent.emitWholeModule(std::move(R), std::move(TSM), SubmittedAsynchronously);
}

class EmitCallThroughStubs : public NamedMaterializationUnit {
public:
  EmitCallThroughStubs(SymbolFlagsMap SymbolFlags, StringRef ModulePath,
                       CompileWholeModuleOnDemandLayer &Parent)
      : NamedMaterializationUnit(std::move(SymbolFlags), VModuleKey(),
                                 "<EmitCallThroughStubs>", Parent),
        ModulePath(ModulePath.str()), Discarded(false) {}

  ~EmitCallThroughStubs() {
    assert((!Discarded || getSymbols().empty()) &&
           "If we discard one symbol, we must discard them all");
  }

private:
  void materialize(MaterializationResponsibility R) override {
    PSTATS("[", ModulePath , "] Materialize call-through stubs: ",
           R.getSymbols());

    assert(!Discarded && "Invalidated by a LoadAndEmitWholeModule unit");
    Parent.emitStubsOnly(std::move(R), ModulePath);
  }

  void discard(const JITDylib &V, const SymbolStringPtr &Name) override {
    // TODO: comment cases
    Discarded = true;
  }

  std::string ModulePath;
  bool Discarded;
};

// A lazy call-through manager that builds trampolines in the current process
// and invokes a custom notification function synchronously on call-through.
class NotifyingCallThroughManager : public LazyCallThroughManager {
private:
  using NotifyCallThroughToSymbolFunction =
      CompileWholeModuleOnDemandLayer::NotifyCallThroughToSymbolFunction;

  NotifyCallThroughToSymbolFunction NotifyCallThroughToSymbol;

  NotifyingCallThroughManager(ExecutionSession &ES,
                              JITTargetAddress ErrorHandlerAddr)
      : LazyCallThroughManager(ES, ErrorHandlerAddr, nullptr) {}

  JITTargetAddress callThroughToSymbol(JITTargetAddress TrampolineAddr);

public:
  /// Create using the given ABI. See createCallThroughManager.
  template <typename ORCABI>
  static Expected<std::unique_ptr<NotifyingCallThroughManager>>
  Create(ExecutionSession &ES, JITTargetAddress ErrorHandlerAddr) {
    auto CTM = std::unique_ptr<NotifyingCallThroughManager>(
        new NotifyingCallThroughManager(ES, ErrorHandlerAddr));

    auto TP = LocalTrampolinePool<ORCABI>::Create(
        [Self = CTM.get()](JITTargetAddress TrampolineAddr) {
          return Self->callThroughToSymbol(TrampolineAddr);
        });
    if (!TP)
      return TP.takeError();

    CTM->setTrampolinePool(std::move(*TP));
    return std::move(CTM);
  }

  void onCallThroughToSymbol(NotifyCallThroughToSymbolFunction F) {
    NotifyCallThroughToSymbol = std::move(F);
  }
};

JITTargetAddress NotifyingCallThroughManager::callThroughToSymbol(
    JITTargetAddress TrampolineAddr) {
  // TODO: Allow derived classes to find the reexports entry for TrampolineAddr

  // Kick-off discovery from here
  if (NotifyCallThroughToSymbol) {
    NotifyCallThroughToSymbol(SymbolStringPtr());
  }

  // TODO: Allow derived classes to resolve the call-through.
  return JITTargetAddress();
}

CompileWholeModuleOnDemandLayer::CompileWholeModuleOnDemandLayer(
    ExecutionSession &ES, IRLayer &BaseLayer, JITTargetMachineBuilder JTMB,
    ModuleIndex &GlobalIndex, char GlobalManglePrefix,
    JITTargetAddress ErrorHandlerAddr)
    : IRLayer(ES, BaseLayer.getManglingOptions()), BaseLayer(BaseLayer),
      CallThroughManager(cantFail(
          createCallThroughManager<NotifyingCallThroughManager>(
              JTMB.getTargetTriple(), ES, ErrorHandlerAddr))),
      BuildIndirectStubsManager(createLocalIndirectStubsManagerBuilder(JTMB.getTargetTriple())),
      GlobalIndex(GlobalIndex), GlobalManglePrefix(GlobalManglePrefix) {}

// Destructor must see local type definitions of its members.
CompileWholeModuleOnDemandLayer::~CompileWholeModuleOnDemandLayer() {}

void CompileWholeModuleOnDemandLayer::onNotifyCallThroughToSymbol(
    NotifyCallThroughToSymbolFunction F) {
  CallThroughManager->onCallThroughToSymbol(std::move(F));
}

void CompileWholeModuleOnDemandLayer::emitStubsOnly(
    MaterializationResponsibility R, StringRef ModulePath) {
  PSTATS("[", ModulePath , "] Starting to emit stubs-only");

  SymbolAliasMap Callables;
  for (auto &KV : R.getSymbols()) {
    SymbolStringPtr Name = KV.first;
    JITSymbolFlags Flags = KV.second;
    assert(Flags.isCallable() && Flags.isWeak());
    Callables[Name] = SymbolAliasMapEntry(Name, Flags);
  }

  // TODO: Create a materialization unit that will load and emit the module. The
  // reexports that we generate in the end will issue a query on call-through,
  // which should trigger the actual materialization.

  // No need to track function aliasees.
  constexpr ImplSymbolMap *NoAliaseeImpls = nullptr;

  // Emit call-through symbols to main dylib.
  JITDylibResources &JDR = getJITDylibResources(R.getTargetJITDylib());
  R.replace(lazyReexports(*CallThroughManager, *JDR.ISManager, *JDR.ShadowJD,
                          std::move(Callables), NoAliaseeImpls));

  PSTATS("[", ModulePath , "] Done emitting stubs-only");
}

void CompileWholeModuleOnDemandLayer::emitReexports(
    MaterializationResponsibility R, ThreadSafeModule TSM) {

  // Delete bodies for available externally functions.
  std::string ModulePath = TSM.withModuleDo([](Module &M) {
    for (auto &F : M) {
      if (F.hasAvailableExternallyLinkage()) {
        F.deleteBody();
        F.setPersonalityFn(nullptr);
      }
    }
    return M.getName().str();
  });

  PSTATS("[", ModulePath , "] Split symbols into callables and data");

  // Build the lazy-aliases map.
  SymbolAliasMap Callables, NonCallables;
  for (auto &KV : R.getSymbols()) {
    SymbolStringPtr Name = KV.first;
    JITSymbolFlags Flags = KV.second;

    if (Flags.isCallable())
      Callables[Name] = SymbolAliasMapEntry(Name, Flags | JITSymbolFlags::Weak);
    else
      NonCallables[Name] = SymbolAliasMapEntry(Name, Flags);
  }

  // TODO: Create a materialization unit that will emit the whole module. The
  // reexports that we generate in the end will issue a query on call-through,
  // which should trigger the actual materialization.

  PSTATS("[", ModulePath , "] Start emitting reexports");

  // No need to track function aliasees.
  constexpr ImplSymbolMap *NoAliaseeImpls = nullptr;
  JITDylibResources &JDR = getJITDylibResources(R.getTargetJITDylib());

  R.replace(reexports(*JDR.ShadowJD, std::move(NonCallables),
                      JITDylibLookupFlags::MatchAllSymbols));
  R.replace(lazyReexports(*CallThroughManager, *JDR.ISManager, *JDR.ShadowJD,
                          std::move(Callables), NoAliaseeImpls));

  PSTATS("[", ModulePath , "] Done emitting reexports");
}

void CompileWholeModuleOnDemandLayer::emitWholeModule(
    MaterializationResponsibility R, ThreadSafeModule TSM,
    bool SubmittedAsynchronously) {

  std::string ModulePath = TSM.withModuleDo([](const Module &M) {
    return M.getName().str();
  });

  PSTATS("[", ModulePath , "] Emitting to CompileLayer",
         (SubmittedAsynchronously ? " (asynchronously)" : ""));

  BaseLayer.emit(std::move(R), std::move(TSM));
  GlobalIndex.setModuleSubmitted(ModulePath, SubmittedAsynchronously);

  PSTATS("[", ModulePath , "] Done emitting to CompileLayer");
}

CompileWholeModuleOnDemandLayer::JITDylibResources &
CompileWholeModuleOnDemandLayer::getJITDylibResources(JITDylib &TargetJD) {
  // Look for existing entry
  for (JITDylibResources &JDR : DylibResources) {
    if (JDR.TargetJD == &TargetJD)
      return JDR;
  }

  // Make a new entry
  JITDylib *ShadowJD = setupShadowJITDylib(&TargetJD);
  DylibResources.push_back({&TargetJD, ShadowJD, BuildIndirectStubsManager()});
  assert(DylibResources.size() == 1 && "So far we only support a single dylib");
  return DylibResources.back();
}

JITDylib *CompileWholeModuleOnDemandLayer::setupShadowJITDylib(JITDylib *MainJD) {
  ExecutionSession &ES = getExecutionSession();
  std::string ShadowJDName = MainJD->getName() + ".impl";
  JITDylib *ShadowJD = &ES.createBareJITDylib(std::move(ShadowJDName));

  JITDylibSearchOrder NewSearchOrder;
  MainJD->withSearchOrderDo(
      [&](const JITDylibSearchOrder &MainJDSearchOrder) {
        NewSearchOrder = MainJDSearchOrder;
      });

  assert(!NewSearchOrder.empty() && NewSearchOrder.front().first == MainJD &&
         "MainJD must be at the front of its own search order");
  assert(NewSearchOrder.front().second == JITDylibLookupFlags::MatchAllSymbols &&
         "MainJD must match non-exported symbols");

  // Append the new JD to the search order and use it for both JDs.
  NewSearchOrder.push_back({ShadowJD, JITDylibLookupFlags::MatchAllSymbols});

  ShadowJD->setSearchOrder(NewSearchOrder, false);
  MainJD->setSearchOrder(std::move(NewSearchOrder), false);

  return ShadowJD;
}

Error CompileWholeModuleOnDemandLayer::addWholeModule(
    JITDylib &JD, ThreadSafeModule TSM, VModuleKey Id, bool FromDiscovery) {
  // Collect and claim responsibility for all symbols in the module.
  SymbolFlagsMap ModuleSymbols = TSM.withModuleDo([this](Module &M) {
    return collectModuleDefinitions(M);
  });
  JITDylibResources &JDR = getJITDylibResources(JD);
  return JDR.ShadowJD->define(std::make_unique<EmitWholeModule>(
      std::move(ModuleSymbols), std::move(TSM), Id, *this, FromDiscovery));
}

Error CompileWholeModuleOnDemandLayer::addReexports(
    JITDylib &JD, ThreadSafeModule TSM, VModuleKey Id) {
  // Collect and claim responsibility for all those symbols in the module, that
  // we didn't already emit call-through stubs for.
  SymbolFlagsMap ModuleSymbols = TSM.withModuleDo([this](Module &M) {
    auto FilterCallThroughStubs =
        [this](Module &M, SymbolStringPtr S) -> bool {
          StringRef N = stripManglePrefix(S, GlobalManglePrefix);
          if (GlobalIndex.hasCallThroughStubEmitted(M.getName(), N)) {
            LLVM_DEBUG(dbgs() << "[" << M.getName()
                              << "] not collecting definition for "<< *S << "\n");
            return false;
          }
          return true;
        };

    return collectModuleDefinitions(M, std::move(FilterCallThroughStubs));
  });
  return JD.define(std::make_unique<EmitReexports>(
      std::move(ModuleSymbols), std::move(TSM), Id, *this));
}

Error CompileWholeModuleOnDemandLayer::addCallThroughStubs(
    JITDylib &JD, StringRef ModulePath, SymbolFlagsMap SymbolFlags) {
  // Declare all synthetic call-through stubs weak, so that they can be
  // overridden easily.
  for (auto &Entry : SymbolFlags) {
    Entry.second |= JITSymbolFlags::Weak;
  }
  return JD.define(std::make_unique<EmitCallThroughStubs>(
      std::move(SymbolFlags), ModulePath, *this));
}

// TODO: Share code with IRMaterializationUnit to properly handle init symbol,
// emulated-TLS, etc.?
SymbolFlagsMap CompileWholeModuleOnDemandLayer::collectModuleDefinitions(
    Module &M, FilterFunction Select) {
  SymbolFlagsMap Symbols;
  MangleAndInterner Mangle(getExecutionSession(), M.getDataLayout());

  for (GlobalValue &G : M.global_values()) {
    // Skip globals that don't generate symbols.
    if (!G.hasName() || G.isDeclaration() || G.hasLocalLinkage() ||
        G.hasAvailableExternallyLinkage() || G.hasAppendingLinkage())
      continue;

    SymbolStringPtr MangledName = Mangle(G.getName());

    // Apply custom filter.
    if (Select && !Select(M, MangledName))
      continue;

    Symbols[MangledName] = JITSymbolFlags::fromGlobalValue(G);
  }

  return Symbols;
}

} // end namespace orc
} // end namespace llvm
