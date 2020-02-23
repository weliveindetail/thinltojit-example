#include "ModuleIndex.h"

#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/SymbolStringPool.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Support/raw_ostream.h"

#include "Errors.h"
#include "StatsUtils.h"

#include <memory>
#include <string>

#define DEBUG_TYPE "thinltojit"

namespace llvm {
namespace orc {

Error ModuleIndex::add(StringRef InputPath) {
  auto Buffer = errorOrToExpected(MemoryBuffer::getFile(InputPath));
  if (!Buffer)
    return Buffer.takeError();

  // This can either be a regular bitcode module or a module index file.
  // The former will add one, the latter will add multiple modules summaries.
  Error ParseErr = readModuleSummaryIndex((*Buffer)->getMemBufferRef(),
                                          CombinedSummaryIndex, NextModuleId);
  if (ParseErr)
    return ParseErr;

#ifndef NDEBUG
  auto Paths = getAllModulePaths();
  unsigned TotalPaths = Paths.size();
  std::sort(Paths.begin(), Paths.end());
  Paths.erase(std::unique(Paths.begin(), Paths.end()), Paths.end());
  assert(TotalPaths == Paths.size() && "Module paths must be unique");
#endif

  StringMap<ModuleRecord>::iterator It;
  bool Inserted;

  // Add entries for all new modules.
  std::lock_guard<std::mutex> Lock(ModuleIndexLock);
  for (const auto &Entry : CombinedSummaryIndex.modulePaths()) {
    std::tie(It, Inserted) = ModuleRecords.try_emplace(Entry.first());
    if (Inserted)
      ++NextModuleId;
  }

  return Error::success();
}

ModuleNames ModuleIndex::getAllModulePaths() const {
  std::lock_guard<std::mutex> Lock(ModuleIndexLock);
  const auto &ModulePathStringTable = CombinedSummaryIndex.modulePaths();

  ModuleNames Paths;
  Paths.resize(ModulePathStringTable.size());
  auto IdxFromId = [](VModuleKey K) { return K - FirstModuleId; };

  for (const auto &KV : ModulePathStringTable) {
    unsigned Idx = IdxFromId(KV.second.first);
    assert(Paths[Idx].empty() && "IDs are unique and continuous");
    Paths[Idx] = KV.first();
  }

  return Paths;
}

GlobalValueSummary *
ModuleIndex::getSummary(GlobalValue::GUID Function) const {
  ValueInfo VI = CombinedSummaryIndex.getValueInfo(Function);
  if (!VI || VI.getSummaryList().empty())
    return nullptr;

  // There can be more than one symbol with the same GUID, in the case of same-
  // named locals in different but same-named source files that were compiled in
  // their respective directories (so the source file name and resulting GUID is
  // the same). We avoid this by checking that module paths are unique upon
  // add().
  //
  // TODO: We can still get duplicates on symbols declared with
  // attribute((weak)), a GNU extension supported by gcc and clang.
  // We should support it by looking for a symbol in the current module
  // or in the same module as the caller.
  assert(VI.getSummaryList().size() == 1 && "Weak symbols not yet supported");

  return VI.getSummaryList().front().get()->getBaseObject();
}


Expected<StringRef>
ModuleIndex::getModulePathForSymbol(StringRef Name) const {
  auto Guid = GlobalValue::getGUID(Name);
  if (GlobalValueSummary *S = getSummary(Guid))
    return S->modulePath();

  return make_error<SummaryNotFound>(Name.str(), Guid, *this);
}

Expected<ThreadSafeModule> ModuleIndex::doParseModule(StringRef Path) {
  SMDiagnostic Err;
  auto Ctx = std::make_unique<LLVMContext>();

  if (std::unique_ptr<Module> M = parseIRFile(Path, Err, *Ctx))
    return ThreadSafeModule(std::move(M), std::move(Ctx));

  return make_error<SourceManagerDiagnostic>(std::move(Err));
}

void ModuleIndex::addToWorklist(
    std::vector<FunctionSummary *> &List,
    ArrayRef<FunctionSummary::EdgeTy> Calls) {
  for (const auto &Edge : Calls) {
    const auto &SummaryList = Edge.first.getSummaryList();
    if (!SummaryList.empty()) {
      GlobalValueSummary *S = SummaryList.front().get()->getBaseObject();
      assert(isa<FunctionSummary>(S) && "Callees must be functions");
      List.push_back(cast<FunctionSummary>(S));
    }
  }
}

ModuleNames ModuleIndex::findModulesInRange(FunctionSummary *Summary,
                                            unsigned LookaheadLevels) {
  // Populate initial worklist
  std::vector<FunctionSummary *> Worklist;
  addToWorklist(Worklist, Summary->calls());
  unsigned Distance = 0;

  std::set<StringRef> Paths;
  while (++Distance < LookaheadLevels) {
    // Process current worklist and populate a new one.
    std::vector<FunctionSummary *> NextWorklist;
    for (FunctionSummary *F : Worklist) {
      Paths.insert(F->modulePath());
      addToWorklist(NextWorklist, F->calls());
    }
    Worklist = std::move(NextWorklist);
  }

  // Process the last worklist without filling a new one.
  for (FunctionSummary *F : Worklist) {
    Paths.insert(F->modulePath());
  }

  // Drop the origin's module.
  Paths.erase(Summary->modulePath());
  return std::vector<StringRef>{Paths.begin(), Paths.end()};
}

ThreadSafeModule ModuleIndex::pickUpSingleModuleBlocking(StringRef Path) {
  FetchModuleResult R = fetchAllModules({Path}).front();

  // At the moment we support one unique entry point. It's the module we
  // call this function on the first time. Record this info for stats.
  static bool IsEntryPointModule = true;
  if (IsEntryPointModule) {
    ModuleRecords[R.Path].IsEntryPointModule = true;
    IsEntryPointModule = false;
  }

  assert(R.State >= ModuleState::Scheduled &&
         "Module parsing should have been initiated");

  // Module may have been picked up by someone else already. Block until we
  // receive a NotifySubmitted signal.
  if (R.State > ModuleState::Parsed) {
    assert(R.State != ModuleState::Emitted && "TODO: Introduce NotifyEmitted");
    assert(R.Notifier.valid());
    if (!is_ready(R.Notifier))
      R.Notifier.wait();
    return ThreadSafeModule();
  }

  // Wait for parsing to finish.
  if (R.State < ModuleState::Parsed) {
    assert(R.Notifier.valid());
    if (!is_ready(R.Notifier))
      R.Notifier.wait();
  }

  assert(is_ready(R.Notifier) && "Didn't we just wait for it to happen?");

  // Try to pick up the module.
  if (auto TSM = pickUpModule(Path)) {
    assert(*TSM && "We shouldn't get a null-module on success");
    return std::move(*TSM);
  }

  // Module was picked up by someone else in the meantime.
  LLVM_DEBUG(dbgs() << "Unlikely race condition\n");
  return ThreadSafeModule();
}

bool ModuleIndex::hasCallThroughStubEmitted(StringRef Path,
                                            StringRef SymbolName) {
  return withModuleRecordDo(Path, [SymbolName](ModuleRecord &Entry) -> bool {
    auto SymbolGuid = GlobalValue::getGUID(SymbolName);
    for (auto Guid : Entry.SyntheticCallThroughs) {
      if (Guid == SymbolGuid)
        return true;
    }
    return false;
  });
}

std::string ModuleIndex::findSymbolForLookup(const Module &M) {
  StringRef NonFunctionGV;
  for (const GlobalValue &G : M.global_values()) {
    // Skip globals that don't generate symbols.
    if (!G.hasName() || G.isDeclaration() || G.hasLocalLinkage() ||
        G.hasAvailableExternallyLinkage() || G.hasAppendingLinkage())
      continue;

    // Prefer functions. When transitive dependencies are looked up, emitting
    // lazy call-through stubs for the requested symbols is sufficient, if and
    // only if all requested symbols for a module refer to callable entities.
    if (isa<Function>(&G))
      return G.getName().str();

    if (NonFunctionGV.empty())
      NonFunctionGV = G.getName();
  }

  // TODO: How to materialize modules without exported symbols? Does such a
  // module even make sense?
  if (NonFunctionGV.empty()) {
    LLVM_DEBUG(dbgs() << "No exported symbol in module " << M.getName()
                      << "\n");
  }

  // No exported functions in this module. Return the fallback name.
  return NonFunctionGV.str();
}

void ModuleIndex::parseModuleFromFile(std::string Path,
                                      ModuleRecord &Entry) {
  auto TSM = doParseModule(Path);
  if (!TSM) {
    ES.reportError(TSM.takeError());
    return;
  }

  std::lock_guard<std::mutex> Lock(Entry.AccessLock);
  Entry.OneSymbolName = TSM->withModuleDo([](const Module &M) {
    return findSymbolForLookup(M);
  });
  assert(Entry.State == ModuleState::Scheduled && "Invalid state");
  Entry.State = ModuleState::Parsed;
  Entry.TSM = std::move(*TSM);

  PSTATS("[", Path , "] Finished parsing");
}

std::vector<ModuleIndex::FetchModuleResult>
ModuleIndex::fetchAllModules(const ModuleNames &Paths) {
  // The ThreadPool worker to parse the module (and signal NotifyParsed).
  auto Worker = [this](std::string Path, ModuleRecord *Entry) {
    std::unique_lock<std::mutex> Lock(Entry->AccessLock);
    if (Entry->State == ModuleState::Scheduled) {
      Lock.unlock(); // Release the lock while parsing the module from file.
      parseModuleFromFile(Path, *Entry);
    }
  };

  std::vector<FetchModuleResult> Results;
  withEachModuleRecordDo(Paths, [&, this](StringRef Path, ModuleRecord &Entry) {
    switch (Entry.State) {
      case ModuleState::Unused:
        Entry.State = ModuleState::Scheduled;
        PSTATS("[", Path , "] Schedule parsing");
        Entry.NotifyParsed = ParseModuleWorkers.async(Worker, Path.str(), &Entry);
        Results.push_back({Path, Entry.State, Entry.NotifyParsed});
        break;
      case ModuleState::Scheduled:
        assert(Entry.NotifyParsed.valid());
        Results.push_back({Path, Entry.State, Entry.NotifyParsed});
        break;
      case ModuleState::Parsed:
        // Call pickUpModule() right away to pick up the module. We don't need
        // a ready-signal in this case, so we pass a dummy future object.
        Results.push_back({Path, Entry.State, DummyReadyFuture});
        break;
      case ModuleState::PickedUp:
        assert(Entry.NotifySubmitted.valid());
        Results.push_back({Path, Entry.State, Entry.NotifySubmitted});
        break;
      case ModuleState::Submitted:
      case ModuleState::Emitted:
        // Skip emitted modules.
        return;
    }
  });

//  std::sort(Results.begin(), Results.end(),
//            [](const FetchModuleResult &LHS, const FetchModuleResult &RHS) {
//              if (std::get<1>(LHS) == ModuleState::Parsed)
//                return true;
//            });

  return Results;
}

std::vector<ModuleIndex::FetchModuleResult>
ModuleIndex::fetchNewModules(const ModuleNames &Paths) {
  // The ThreadPool worker to parse the module (and signal NotifyParsed).
  auto Worker = [this](std::string Path, ModuleRecord *Entry) {
    std::unique_lock<std::mutex> Lock(Entry->AccessLock);
    if (Entry->State == ModuleState::Scheduled) {
      Lock.unlock(); // Release the lock while parsing the module from file.
      parseModuleFromFile(Path, *Entry);
    }
  };

  std::vector<FetchModuleResult> Results;
  withEachModuleRecordDo(Paths, [&, this](StringRef Path, ModuleRecord &Entry) {
    if (Entry.State == ModuleState::Unused) {
      PSTATS("[", Path , "] Schedule parsing");
      Entry.State = ModuleState::Scheduled;
      Entry.NotifyParsed = ParseModuleWorkers.async(Worker, Path.str(), &Entry);
      Results.push_back({Path, Entry.State, Entry.NotifyParsed});
    }
  });

  return Results;
}

Optional<ThreadSafeModule> ModuleIndex::pickUpModule(StringRef Path) {
  return ES.runSessionLocked([this, Path]() -> Optional<ThreadSafeModule> {
    PSTATS("[", Path , "] Picking up module");
    return withModuleRecordDo(Path, [](ModuleRecord &Entry) -> Optional<ThreadSafeModule> {
      // Bail out if the module was picked up by someone else already.
      if (Entry.NotifySubmitted.valid()) {
        assert(Entry.State > ModuleState::Parsed);
        return None;
      }

      assert(Entry.State == ModuleState::Parsed && "Invalid state for pickup");
      assert(Entry.TSM && "No module to pick up?");

      // Take the module from the entry and overwrite the moved-from state with
      // a null-module. Then issue the NotifySubmitted future and return the module.
      ThreadSafeModule Result = std::move(Entry.TSM);
      Entry.TSM = ThreadSafeModule();
      Entry.State = ModuleState::PickedUp;
      Entry.NotifySubmitted = Entry.TriggerOnSubmit.get_future();
      return std::move(Result);
    });
  });
}

void ModuleIndex::notifySyntheticCallThroughEmitted(
    StringRef Path, const std::vector<GlobalValue::GUID> &Guids) {
#ifndef NDEBUG
  std::string Buffer;
  raw_string_ostream OS(Buffer);
  bool PrependSeperator = false;
  for (const auto &G : Guids) {
    OS << (PrependSeperator ? ", " : "") << G;
    PrependSeperator = true;
  }
  LLVM_DEBUG(dbgs() << "Recorded GUIDs for CallThroughs: " << OS.str() << "\n");
#endif

  withModuleRecordDo(Path, [&Guids](ModuleRecord &Entry) {
    for (const auto &G : Guids)
      Entry.SyntheticCallThroughs.push_back(G);
  });
}

void ModuleIndex::setModuleSubmitted(StringRef Path, bool Asynchronously) {
  withModuleRecordDo(Path, [Asynchronously](ModuleRecord &Entry) {
    assert(Entry.State == ModuleState::PickedUp && "Invalid state");
    assert(Entry.NotifySubmitted.valid() && "Future was not initialized");
    assert(!is_ready(Entry.NotifySubmitted) && "Cannot submit twice");

    Entry.State = ModuleState::Submitted;
    Entry.SubmittedAsynchronously = Asynchronously;
    Entry.TriggerOnSubmit.set_value(); // NotifySubmitted -> ready
  });
  PSTATS("[", Path , "] Submitted");
}

void ModuleIndex::setModuleEmitted(StringRef Path) {
  withModuleRecordDo(Path, [](ModuleRecord &Entry) {
    assert(Entry.State == ModuleState::Submitted && "Invalid state");
    assert(is_ready(Entry.NotifySubmitted) && "Cannot submit twice");
    Entry.State = ModuleState::Emitted;
  });
  PSTATS("[", Path , "] Emitted");
}

void ModuleIndex::setModuleReached(StringRef Path) {
  withModuleRecordDo(Path, [](ModuleRecord &Entry) {
    static unsigned ReachedIdx = 0;
    Entry.ReachedId = ++ReachedIdx; // 0 == Unreached
  });
}

SymbolLookupSet ModuleIndex::createLookupSet(const ModuleNames &Paths,
                                             MangleAndInterner &Mangle) {
  SymbolLookupSet LookaheadSymbols;
  withEachModuleRecordDo(Paths, [&](StringRef Path, ModuleRecord &Entry) {
    assert(Entry.State >= ModuleState::PickedUp);
    if (Entry.State < ModuleState::Emitted)
      LookaheadSymbols.add(Mangle(Entry.OneSymbolName));
  });
  return LookaheadSymbols;
}

// Use ThinLTO module summaries to group incoming symbols by their defining
// modules. Foreign symbols (that have no summary) are ignored.
StringMap<ModuleIndex::ModuleInfo>
ModuleIndex::fetchModuleInfo(const SymbolLookupSet &Symbols,
                             char GlobalManglePrefix) {
  StringMap<ModuleInfo> MIs;
  for (const auto &KV : Symbols) {
    StringRef Name = stripManglePrefix(KV.first, GlobalManglePrefix);
    auto Guid = GlobalValue::getGUID(Name);

    // Append symbols we know. Ignore foreign symbols.
    if (GlobalValueSummary *S = getSummary(Guid)) {
      ModuleInfo &MI = MIs[S->modulePath()];

      MI.SymbolFlags[KV.first] = JITSymbolFlags::fromSummary(S);
      MI.Guids.push_back(Guid);

      if (!isa<FunctionSummary>(S))
        MI.AllCallable = false;
    }
  }
  return MIs;
}

ModuleIndex::~ModuleIndex() {
  if (PrintStats) {
    dbgs() << "\nModule Index Stats\n";
    dbgs() << "Path            State       Reached Submitted   Synthetic Call-Throughs\n";
    withAllModuleRecordsDo([](StringRef Path, ModuleRecord &Entry) {
      std::string Buffer(80, ' ');
      std::string PathCropped = Path.substr(0, 15).str();
      Buffer.replace(0, PathCropped.size(), PathCropped);
      std::string StateStr = toString(Entry.State);
      Buffer.replace(16, StateStr.size(), StateStr);
      if (Entry.State >= ModuleState::Submitted) {
        if (Entry.ReachedId > 0) {
          std::string ReachedIdStr = std::to_string(Entry.ReachedId - 1);
          Buffer.replace(28, ReachedIdStr.size(), ReachedIdStr);
        }
        if (Entry.IsEntryPointModule) {
          Buffer.replace(36, 11, "Entry-point");
        } else if (Entry.SubmittedAsynchronously) {
          Buffer.replace(36, 9, "Discovery");
        } else {
          Buffer.replace(36, 9, "Generator");
        }
      }
      std::string CallThroughsStr =
          std::to_string(Entry.SyntheticCallThroughs.size());
      Buffer.replace(48, CallThroughsStr.size(), CallThroughsStr);

      dbgs() << Buffer << "\n";
    });
  }

  // Fulfill all promises before shutting down.
  withAllModuleRecordsDo([](StringRef Path, ModuleRecord &Entry) {
    if (Entry.State > ModuleState::Unused) {
      if (!is_ready(Entry.NotifySubmitted))
        Entry.TriggerOnSubmit.set_value();
    }
  });
}

} // namespace orc
} // namespace llvm
