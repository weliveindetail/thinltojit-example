//===----- CompileOnDemandLayer.cpp - Lazily emit IR on first call --------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/CompileOnDemandLayer.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"

#include "llvm/IR/Verifier.h"

using namespace llvm;
using namespace llvm::orc;

static ThreadSafeModule extractSubModule(ThreadSafeModule &TSM,
                                         StringRef Suffix,
                                         GVPredicate ShouldExtract) {

  auto DeleteExtractedDefs = [](GlobalValue &GV) {
    // If a value is unused then delete it from the module.
    if (GV.use_empty()) {
      GV.eraseFromParent();
      return;
    }

    // Bump the linkage: this global will be provided by the external module.
    GV.setLinkage(GlobalValue::ExternalLinkage);

    // Delete the definition in the source module.
    if (isa<Function>(GV)) {
      auto &F = cast<Function>(GV);
      F.deleteBody();
      F.setPersonalityFn(nullptr);
    } else if (isa<GlobalVariable>(GV)) {
      cast<GlobalVariable>(GV).setInitializer(nullptr);
    } else if (isa<GlobalAlias>(GV)) {
      // We need to turn deleted aliases into function or variable decls based
      // on the type of their aliasee.
      auto &A = cast<GlobalAlias>(GV);
      Constant *Aliasee = A.getAliasee();
      assert(A.hasName() && "Anonymous alias?");
      assert(Aliasee->hasName() && "Anonymous aliasee");
      std::string AliasName = std::string(A.getName());

      if (isa<Function>(Aliasee)) {
        auto *F = cloneFunctionDecl(*A.getParent(), *cast<Function>(Aliasee));
        A.replaceAllUsesWith(F);
        A.eraseFromParent();
        F->setName(AliasName);
      } else if (isa<GlobalVariable>(Aliasee)) {
        auto *G = cloneGlobalVariableDecl(*A.getParent(),
                                          *cast<GlobalVariable>(Aliasee));
        A.replaceAllUsesWith(G);
        A.eraseFromParent();
        G->setName(AliasName);
      } else
        llvm_unreachable("Alias to unsupported type");
    } else
      llvm_unreachable("Unsupported global type");
  };

  auto NewTSM = cloneToNewContext(TSM, ShouldExtract, DeleteExtractedDefs);
  NewTSM.withModuleDo([&](Module &M) {
    M.setModuleIdentifier((M.getModuleIdentifier() + Suffix).str());
  });

  return NewTSM;
}

namespace llvm {
namespace orc {

class PartitioningIRMaterializationUnit : public IRMaterializationUnit {
public:
  PartitioningIRMaterializationUnit(ExecutionSession &ES,
                                    const IRSymbolMapper::ManglingOptions &MO,
                                    ThreadSafeModule TSM, VModuleKey K,
                                    CompileOnDemandLayer &Parent)
      : IRMaterializationUnit(ES, MO, std::move(TSM), std::move(K)),
        Parent(Parent) {}

  PartitioningIRMaterializationUnit(
      ThreadSafeModule TSM, SymbolFlagsMap SymbolFlags,
      SymbolStringPtr InitSymbol, SymbolNameToDefinitionMap SymbolToDefinition,
      CompileOnDemandLayer &Parent)
      : IRMaterializationUnit(std::move(TSM), std::move(K),
                              std::move(SymbolFlags), std::move(InitSymbol),
                              std::move(SymbolToDefinition)),
        Parent(Parent) {}

private:
  void materialize(MaterializationResponsibility R) override {
    Parent.emitPartition(std::move(R), std::move(TSM),
                         std::move(SymbolToDefinition));
  }

  void discard(const JITDylib &V, const SymbolStringPtr &Name) override {
    // All original symbols were materialized by the CODLayer and should be
    // final. The function bodies provided by M should never be overridden.
    llvm_unreachable("Discard should never be called on an "
                     "ExtractingIRMaterializationUnit");
  }

  mutable std::mutex SourceModuleMutex;
  CompileOnDemandLayer &Parent;
};

Optional<CompileOnDemandLayer::GlobalValueSet>
CompileOnDemandLayer::compileRequested(GlobalValueSet Requested) {
  return std::move(Requested);
}

Optional<CompileOnDemandLayer::GlobalValueSet>
CompileOnDemandLayer::compileWholeModule(GlobalValueSet Requested) {
  return None;
}

CompileOnDemandLayer::CompileOnDemandLayer(
    ExecutionSession &ES, IRLayer &BaseLayer, LazyCallThroughManager &LCTMgr,
    IndirectStubsManagerBuilder BuildIndirectStubsManager)
    : IRLayer(ES, BaseLayer.getManglingOptions()), BaseLayer(BaseLayer),
      LCTMgr(LCTMgr),
      BuildIndirectStubsManager(std::move(BuildIndirectStubsManager)) {}

void CompileOnDemandLayer::setPartitionFunction(PartitionFunction Partition) {
  this->Partition = std::move(Partition);
}

void CompileOnDemandLayer::setImplMap(ImplSymbolMap *Imp) {
  this->AliaseeImpls = Imp;
}

void CompileOnDemandLayer::emit(MaterializationResponsibility R,
                                ThreadSafeModule TSM) {
  assert(TSM && "Null module");

  auto &ES = getExecutionSession();

  // Sort the callables and non-callables, build re-exports and lodge the
  // actual module with the implementation dylib.
  auto &PDR = getPerDylibResources(R.getTargetJITDylib());

  auto CleanUpModule = [&](Module &M) -> Error {
    // Delete bodies for available externally functions.
    for (auto &F : M) {
      if (F.hasAvailableExternallyLinkage()) {
        F.deleteBody();
        F.setPersonalityFn(nullptr);
      }
    }

    // Promote globals and define new materializing symbols.
    SmallVector<GlobalValue *, 16> PromotedGlobals;
    for (auto &GV : M.global_values()) {

      if (GV.isDeclaration() || GV.hasAvailableExternallyLinkage())
        continue;

      bool Promoted = true;

      // Rename if necessary.
      if (!GV.hasName())
        GV.setName("__orc_anon." + Twine(++NextPromoId));
      else if (GV.getName().startswith("\01L"))
        GV.setName("__" + GV.getName().substr(1) + "." + Twine(++NextPromoId));
      else if (GV.hasLocalLinkage())
        GV.setName("__orc_lcl." + GV.getName() + "." + Twine(++NextPromoId));
      else
        Promoted = false;

      // Promote internal linkage to external/hidden.
      if (GV.hasLocalLinkage()) {
        GV.setLinkage(GlobalValue::ExternalLinkage);
        GV.setVisibility(GlobalValue::HiddenVisibility);
        Promoted = true;
      }

      // Remove unnamed-addr.
      GV.setUnnamedAddr(GlobalValue::UnnamedAddr::None);

      if (Promoted)
        PromotedGlobals.push_back(&GV);
    }

    if (PromotedGlobals.empty())
      return Error::success();

    SymbolFlagsMap PromotedSymbolFlags;
    IRSymbolMapper::add(ES, *getManglingOptions(), PromotedGlobals,
                        PromotedSymbolFlags);

    return R.defineMaterializing(PromotedSymbolFlags);
  };

  if (auto Err = TSM.withModuleDo(CleanUpModule)) {
    ES.reportError(std::move(Err));
    R.failMaterialization();
    return;
  }

  // Build the lazy-aliases map.
  SymbolAliasMap Callables, NonCallables;
  for (auto &KV : R.getSymbols()) {
    auto &Name = KV.first;
    auto &Flags = KV.second;
    if (Flags.isCallable())
      Callables[Name] = SymbolAliasMapEntry(Name, Flags);
    else
      NonCallables[Name] = SymbolAliasMapEntry(Name, Flags);
  }

  // Create a partitioning materialization unit and lodge it with the
  // implementation dylib.
  if (auto Err = PDR.getImplDylib().define(
          std::make_unique<PartitioningIRMaterializationUnit>(
              ES, *getManglingOptions(), std::move(TSM), R.getVModuleKey(),
              *this))) {
    ES.reportError(std::move(Err));
    R.failMaterialization();
    return;
  }

  R.replace(reexports(PDR.getImplDylib(), std::move(NonCallables),
                      JITDylibLookupFlags::MatchAllSymbols));
  R.replace(lazyReexports(LCTMgr, PDR.getISManager(), PDR.getImplDylib(),
                          std::move(Callables), AliaseeImpls));
}

CompileOnDemandLayer::PerDylibResources &
CompileOnDemandLayer::getPerDylibResources(JITDylib &TargetD) {
  auto I = DylibResources.find(&TargetD);
  if (I == DylibResources.end()) {
    auto &ImplD =
        getExecutionSession().createBareJITDylib(TargetD.getName() + ".impl");
    JITDylibSearchOrder NewSearchOrder;
    TargetD.withSearchOrderDo(
        [&](const JITDylibSearchOrder &TargetSearchOrder) {
          NewSearchOrder = TargetSearchOrder;
        });

    assert(
        !NewSearchOrder.empty() && NewSearchOrder.front().first == &TargetD &&
        NewSearchOrder.front().second == JITDylibLookupFlags::MatchAllSymbols &&
        "TargetD must be at the front of its own search order and match "
        "non-exported symbol");
    NewSearchOrder.insert(std::next(NewSearchOrder.begin()),
                          {&ImplD, JITDylibLookupFlags::MatchAllSymbols});
    ImplD.setSearchOrder(NewSearchOrder, false);
    TargetD.setSearchOrder(std::move(NewSearchOrder), false);

    PerDylibResources PDR(ImplD, BuildIndirectStubsManager());
    I = DylibResources.insert(std::make_pair(&TargetD, std::move(PDR))).first;
  }

  return I->second;
}

void CompileOnDemandLayer::emitPartition(
    MaterializationResponsibility R, ThreadSafeModule TSM,
    IRMaterializationUnit::SymbolNameToDefinitionMap Defs) {

  // FIXME: Need a 'notify lazy-extracting/emitting' callback to tie the
  //        extracted module key, extracted module, and source module key
  //        together. This could be used, for example, to provide a specific
  //        memory manager instance to the linking layer.

  auto &ES = getExecutionSession();
  GlobalValueSet RequestedGVs;
  for (auto &Name : R.getRequestedSymbols()) {
    assert(Defs.count(Name) && "No definition for symbol");
    RequestedGVs.insert(Defs[Name]);
  }

  /// Perform partitioning with the context lock held, since the partition
  /// function is allowed to access the globals to compute the partition.
  auto GVsToExtract =
      TSM.withModuleDo([&](Module &M) { return Partition(RequestedGVs); });

  // Take a 'None' partition to mean the whole module (as opposed to an empty
  // partition, which means "materialize nothing"). Emit the whole module
  // unmodified to the base layer.
  if (GVsToExtract == None) {
    Defs.clear();
    BaseLayer.emit(std::move(R), std::move(TSM));
    return;
  }

  // If the partition is empty, return the whole module to the symbol table.
  if (GVsToExtract->empty()) {
    R.replace(std::make_unique<PartitioningIRMaterializationUnit>(
        std::move(TSM), R.getSymbols(), R.getInitializerSymbol(),
        std::move(Defs), *this));
    return;
  }

  // Ok -- we actually need to partition the symbols. Expand the partition to
  // include any aliases.
  {
    SmallVector<const GlobalValue *, 8> GVsToAdd;
    for (auto *GV : *GVsToExtract)
      if (auto *A = dyn_cast<GlobalAlias>(GV))
        if (auto *GVAliasee = dyn_cast<GlobalValue>(A->getAliasee()))
          GVsToAdd.push_back(GVAliasee);
    for (auto *GV : GVsToAdd)
      GVsToExtract->insert(GV);
  }

  // Now extract the partition.
  auto ShouldExtract = [&](const GlobalValue &GV) -> bool {
    if (auto *A = dyn_cast<GlobalAlias>(&GV))
      if (GVsToExtract->count(cast<GlobalValue>(A->getAliasee())))
        return true;
    return GVsToExtract->count(&GV);
  };

  auto ExtractedTSM = extractSubModule(TSM, ".submodule", ShouldExtract);
  R.replace(std::make_unique<PartitioningIRMaterializationUnit>(
      ES, *getManglingOptions(), std::move(TSM), R.getVModuleKey(), *this));
  BaseLayer.emit(std::move(R), std::move(ExtractedTSM));
}

} // end namespace orc
} // end namespace llvm
