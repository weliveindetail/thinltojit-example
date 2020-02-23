#include "ThinLtoJIT.h"

#include "llvm/ExecutionEngine/Orc/CompileUtils.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/IRCompileLayer.h"
#include "llvm/ExecutionEngine/Orc/IndirectionUtils.h"
#include "llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/Support/Debug.h"

#include "Errors.h"
#include "LambdaGenerator.h"
#include "OnDemandLayer.h"
#include "StatsUtils.h"
#include "Utils.h"

#include <set>

#ifndef NDEBUG
#include <chrono>
#endif

#define DEBUG_TYPE "thinltojit"

namespace llvm {
namespace orc {

ThinLtoJIT::ThinLtoJIT(ArrayRef<std::string> InputFiles,
                       StringRef MainFunctionName,
                       DispatchMaterialization DispatchPolicy,
                       unsigned LookaheadLevels,
                       unsigned NumCompileThreads, unsigned NumLoadThreads,
                       unsigned NumDiscoveryThreads, bool PrintStats,
                       Error &Err) {
  ErrorAsOutParameter ErrAsOutParam(&Err);

  // Populate the module index, so we know which modules exist and we can find
  // the one that defines the main function.
  GlobalIndex = std::make_unique<ModuleIndex>(ES, NumLoadThreads, PrintStats);
  for (StringRef F : InputFiles) {
    if (auto Err = GlobalIndex->add(F))
      ES.reportError(std::move(Err));
  }

  // Find the module that defines the main function.
  auto Path = GlobalIndex->getModulePathForSymbol(MainFunctionName);
  if (!Path) {
    Err = Path.takeError();
    return;
  }

  // Infer target-specific utils from the main module.
  ThreadSafeModule MainModule = GlobalIndex->pickUpSingleModuleBlocking(*Path);
  auto JTMB = setupTargetUtils(MainModule);
  if (!JTMB) {
    Err = JTMB.takeError();
    return;
  }

  // Set up the JIT compile pipeline.
  setupCompilePipeline(std::move(*JTMB), NumCompileThreads,
                       NumDiscoveryThreads, LookaheadLevels, DispatchPolicy);

  // We are restricted to a single dylib currently. Add runtime overrides and
  // symbol generators.
  MainJD = &cantFail(ES.createJITDylib("main"));
  Err = setupJITDylib(MainJD, PrintStats);
  if (Err)
    return;

  // TODO: Ideally we emitted only a synthetic call-through for the entry point,
  // but we picked up the module already and our only code path for this, does
  // the pick-up on materialize().
  PSTATS("[", *Path, "] Submitting whole module");
  VModuleKey K = GlobalIndex->getModuleId(*Path);
  Err = OnDemandLayer->addReexports(*MainJD, std::move(MainModule), K);
}

Expected<JITTargetMachineBuilder>
ThinLtoJIT::setupTargetUtils(ThreadSafeModule &TSM) {
  // Use the module's Triple or fall back to the host machine's default.
  auto JTMB = TSM.withModuleDo([](Module &M) {
    std::string T = M.getTargetTriple();
    return JITTargetMachineBuilder(Triple(T.empty() ? sys::getProcessTriple() : T));
  });

  // Use module's DataLayout or fall back to the host's default.
  Error Err = TSM.withModuleDo([this, &JTMB](Module &M) -> Error {
    DL = DataLayout(&M);
    if (DL.getStringRepresentation().empty()) {
      auto HostDL = JTMB.getDefaultDataLayoutForTarget();
      if (!HostDL)
        return HostDL.takeError();
      DL = std::move(*HostDL);
      if (Error Err = applyDataLayout(M))
        return Err;
    }
    return Error::success();
  });

  if (Err)
    return std::move(Err);

  // Now that we know the target data layout, we can setup the mangler.
  Mangle = std::make_unique<MangleAndInterner>(ES, DL);
  return JTMB;
}

Error ThinLtoJIT::applyDataLayout(ThreadSafeModule &TSM) {
  return TSM.withModuleDo([this](Module &M) {
    return applyDataLayout(M);
  });
}

Error ThinLtoJIT::applyDataLayout(Module &M) {
  if (M.getDataLayout().isDefault())
    M.setDataLayout(DL);

  if (M.getDataLayout() != DL)
    return make_error<IncompatibleDataLayout>(M, DL);

  return Error::success();
}

Error ThinLtoJIT::setupJITDylib(JITDylib *JD, bool PrintStats) {
  // Register symbols for C++ static destructors.
  LocalCXXRuntimeOverrides CXXRuntimeoverrides;
  Error Err = CXXRuntimeoverrides.enable(*JD, *Mangle);
  if (Err)
    return Err;

  // Lookup symbol names in the global ThinLTO module index first
  JD->addGenerator(onTryToGenerate(
      [this](LookupKind K, JITDylib &JD, JITDylibLookupFlags,
             const SymbolLookupSet &Symbols) {
        return generateMissingSymbolsBlocking(K, JD, Symbols);
      }));

  // Then try lookup in the host process.
  char Prefix = DL.getGlobalPrefix();
  auto HostLookup = DynamicLibrarySearchGenerator::GetForCurrentProcess(Prefix);
  if (!HostLookup)
    return HostLookup.takeError();
  JD->addGenerator(std::move(*HostLookup));

  return Error::success();
}

void ThinLtoJIT::setupCompilePipeline(JITTargetMachineBuilder JTMB,
                                      unsigned NumCompileThreads,
                                      unsigned NumDiscoveryThreads,
                                      unsigned LookaheadLevels,
                                      DispatchMaterialization DispatchPolicy) {
  ObjLinkingLayer = std::make_unique<RTDyldObjectLinkingLayer>(
      ES, []() { return std::make_unique<SectionMemoryManager>(); });

  CompileLayer = std::make_unique<IRCompileLayer>(
      ES, *ObjLinkingLayer, std::make_unique<ConcurrentIRCompiler>(JTMB));

  OnDemandLayer = std::make_unique<CompileWholeModuleOnDemandLayer>(
      ES, *CompileLayer, JTMB, *GlobalIndex, DL.getGlobalPrefix(),
      pointerToJITTargetAddress(exitOnLazyCallThroughFailure));

  DiscoveryThreads = std::make_unique<ThreadPool>(NumDiscoveryThreads);
  OnDemandLayer->onNotifyCallThroughToSymbol(
      [this, LookaheadLevels](SymbolStringPtr MangledName) {
        StringRef Name = stripManglePrefix(MangledName, DL.getGlobalPrefix());
        auto Guid = GlobalValue::getGUID(Name);

        GlobalValueSummary *S = GlobalIndex->getSummary(Guid);
        assert(S && "Cannot have call-throughs for foreign symbols");
        assert(isa<FunctionSummary>(S) && "Call-throughs refer to callables");

        // TODO: Record execution trace for stats only if stats requested.
        IFSTATS(GlobalIndex->setModuleReached(S->modulePath()));

        FunctionSummary *Origin = cast<FunctionSummary>(S);
        DiscoveryThreads->async(
            [this, Origin, LookaheadLevels, MangledName]() {
              if (Error Err = runDiscovery(Origin, *MangledName, LookaheadLevels)) {
                LLVM_DEBUG(dbgs() << "[" << *MangledName
                                  << "] Error in discovery\n");
                ES.reportError(std::move(Err));
              }
            });
      });

  // Delegate compilation to the thread pool.
  CompileThreads = std::make_unique<ThreadPool>(NumCompileThreads);
  ES.setDispatchMaterialization(createThreadPoolDispatcher(DispatchPolicy));
}

ExecutionSession::DispatchMaterializationFunction
ThinLtoJIT::createThreadPoolDispatcher(DispatchMaterialization DispatchPolicy) {
  switch (DispatchPolicy) {
    case DispatchMaterialization::Never:
      return [](JITDylib &JD, std::unique_ptr<MaterializationUnit> MU) {
        MU->doMaterialize(JD);
      };
    case DispatchMaterialization::Always:
      return [this](JITDylib &JD, std::unique_ptr<MaterializationUnit> MU) {
        dispatchToThreadpool(JD, std::move(MU));
      };
    case DispatchMaterialization::WholeModules:
      return [this](JITDylib &JD, std::unique_ptr<MaterializationUnit> MU) {
        if (MU->getName() == "<EmitWholeModule>")
          dispatchToThreadpool(JD, std::move(MU));
        else
          MU->doMaterialize(JD);
      };
  }
}

// FIXME: Drop the std::shared_ptr workaround once ThreadPool::async() accepts
// llvm::unique_function to define jobs.
void ThinLtoJIT::dispatchToThreadpool(JITDylib &JD,
                                      std::unique_ptr<MaterializationUnit> MU) {
  auto SharedMU = std::shared_ptr<MaterializationUnit>(std::move(MU));
  CompileThreads->async(
      [MU = std::move(SharedMU), &JD]() { MU->doMaterialize(JD); });
}

Error ThinLtoJIT::runDiscovery(FunctionSummary *Origin, StringRef MangledName,
                               unsigned LookaheadLevels) {
  // Find all module that are in range and stop if there are none.
  ModuleNames LookaheadModules =
      GlobalIndex->findModulesInRange(Origin, LookaheadLevels);

  if (LookaheadModules.empty()) {
    PSTATS("[", MangledName, "] No modules in range");
    return Error::success();
  }

  PSTATS("[", MangledName, "] Lookahead range: ", LookaheadModules);

  // Wait for modules to finish parsing and submit them. Some of them may have
  // been emitted in the meantime, so we don't need to force materialization.
  ModuleNames NonReadyModules;
  for (FetchModuleResult &R : GlobalIndex->fetchNewModules(LookaheadModules)) {
    StringRef Path = R.Path;
    bool NotReady;
    if (Error Err = submitWholeModuleBlocking(std::move(R), NotReady))
      return Err;

    if (NotReady)
      NonReadyModules.push_back(Path);
  }

  PSTATS("[", MangledName, "] Lookahead materializes: ", NonReadyModules);

  JITDylibSearchOrder SearchOrder;
  MainJD->withSearchOrderDo(
      [&](const JITDylibSearchOrder &O) { SearchOrder = O; });

  // For each module, get one symbol that we can lookup in order to force
  // materialization.
  SymbolLookupSet LookaheadSymbols =
      GlobalIndex->createLookupSet(NonReadyModules, *Mangle);

  // Blocking lookup. We are done after this.
  auto SymMap = ES.lookup(SearchOrder, LookaheadSymbols,
                          LookupKind::Static, SymbolState::Ready);

  return SymMap ? Error::success() : SymMap.takeError();
}

Error ThinLtoJIT::submitWholeModuleBlocking(ModuleIndex::FetchModuleResult R,
                                            bool &NotReady) {
  ModuleNames ModulesToMaterialize;

  // Has the module been picked up by someone else in the meantime?
  if (R.State > ModuleState::Parsed) {
    PSTATS("[", R.Path, "] Got picked up while scheduled in discovery");

    // Issue a lookup for this module if it wasn't emitted yet.
    NotReady = (R.State != ModuleState::Emitted);
    return Error::success();
  }

  // Wait for parsing to finish.
  if (R.State < ModuleState::Parsed) {
    if (!is_ready(R.Notifier))
      R.Notifier.wait();
  }

  assert(is_ready(R.Notifier) && "Didn't we just wait for it to happen?");

  // Try to pick up the module.
  if (auto TSM = GlobalIndex->pickUpModule(R.Path)) {
    if (Error Err = applyDataLayout(*TSM))
      return Err;

    PSTATS("[", R.Path, "] Submitting whole module");
    if (Error Err = OnDemandLayer->addWholeModule(
        *MainJD, std::move(*TSM), GlobalIndex->getModuleId(R.Path), true))
      return Err;

    NotReady = true;
  } else {
    // If someone else picked up the module, it's their responsibility.
    PSTATS("[", R.Path, "] Got picked up though discovery was waiting for it. ",
           "This should be rare.");
    NotReady = false;
  }

  return Error::success();
}

Error ThinLtoJIT::submitReexportsBlocking(ModuleIndex::FetchModuleResult R) {
  // Module was picked up by someone else already.
  if (R.State > ModuleState::Parsed) {
    PSTATS("[", R.Path, "] Picked up while requested in generator");
    return Error::success();
  }

  // Wait for parsing to finish.
  if (R.State < ModuleState::Parsed) {
    assert(R.Notifier.valid());
    if (!is_ready(R.Notifier))
      R.Notifier.wait();
  }

  assert(is_ready(R.Notifier) && "Didn't we just wait for it to happen?");

  // Try to pick up the module.
  if (auto TSM = GlobalIndex->pickUpModule(R.Path)) {
    if (Error Err = applyDataLayout(*TSM))
      return Err;

    PSTATS("[", R.Path, "] Submitting reexports");
    return OnDemandLayer->addReexports(*MainJD, std::move(*TSM),
                                        GlobalIndex->getModuleId(R.Path));
  }

  return Error::success();
}

Error ThinLtoJIT::submitCallThroughStubs(StringRef ModulePath,
                                         ModuleIndex::ModuleInfo MI) {
  if (Error Err = OnDemandLayer->addCallThroughStubs(
          *MainJD, ModulePath, std::move(MI.SymbolFlags)))
    return Err;

  GlobalIndex->notifySyntheticCallThroughEmitted(ModulePath, MI.Guids);
  return Error::success();
}

Error ThinLtoJIT::generateMissingSymbolsBlocking(
    LookupKind K, JITDylib &JD, const SymbolLookupSet &Symbols) {
  assert(&JD == MainJD && "So far we support a single JITDylib");

  // Find the modules we need for the requested symbols.
  StringMap<ModuleInfo> ModuleInfoMap =
      GlobalIndex->fetchModuleInfo(Symbols, DL.getGlobalPrefix());

  // For transitive dependencies, we emit synthetic call-through stubs if all
  // requested symbols for one module are callable. It avoids the blocking load
  // of all their modules here in the generator. Those modules that we actually
  // need, will hopefully be discovered and materialized in parallel before
  // calling through the stubs, but we cannot be sure.
  ModuleNames RemainingPaths;
  for (auto &KV : ModuleInfoMap) {
    StringRef Path = KV.first();
    // TODO: Allow to distinguish regular static lookups from lookups for
    // transitive dependencies issued by the linker.
    RemainingPaths.push_back(Path);
  }

  // For direct requests and transitive data dependencies, emit reexports.
  if (!RemainingPaths.empty()) {
    PSTATS("Generator requesting reexports (", K, "): ", RemainingPaths);

    for (FetchModuleResult &R : GlobalIndex->fetchAllModules(RemainingPaths)) {
      if (Error Err = submitReexportsBlocking(std::move(R)))
        return Err;
    }
  }

  return Error::success();
}

ThinLtoJIT::~ThinLtoJIT() {
  // Wait for remaining workloads to finish.
  CompileThreads->wait();
  DiscoveryThreads->wait();
}

} // namespace orc
} // namespace llvm
