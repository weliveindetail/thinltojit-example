#ifndef LLVM_EXAMPLES_THINLTOJIT_THINLTOMODULEINDEX_H
#define LLVM_EXAMPLES_THINLTOJIT_THINLTOMODULEINDEX_H

#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/Mangling.h"
#include "llvm/ExecutionEngine/Orc/ThreadSafeModule.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/ModuleSummaryIndex.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/ThreadPool.h"

#include "Utils.h"

#include <cstdint>
#include <future>
#include <mutex>
#include <set>
#include <vector>

namespace llvm {
namespace orc {

enum class ModuleState : uint8_t {
  Unused = 0,
  Scheduled,
  Parsed,
  PickedUp,
  Submitted,
  Emitted     // How to determine? OnComplete of explicit lookup?
};

// Module names typically reference entries of the ModuleTable in the
// ModuleSummaryIndex. As these entries are immutable, we it is usually safe
// to pass module names as StringRef.
using ModuleNames = std::vector<StringRef>;

class ModuleIndex {
  static constexpr bool HaveGVs = false;
  static constexpr VModuleKey FirstModuleId = 0;

public:
  ModuleIndex(ExecutionSession &ES, unsigned ParseModuleThreads,
              bool PrintStats)
      : ES(ES), CombinedSummaryIndex(HaveGVs), NextModuleId(FirstModuleId),
        ParseModuleWorkers(ParseModuleThreads), PrintStats(PrintStats),
        DummyReadyFuture(makeReadyFuture()) {}
  ~ModuleIndex();

  Error add(StringRef InputPath);

  GlobalValueSummary *getSummary(GlobalValue::GUID Function) const;
  ModuleNames getAllModulePaths() const;
  Expected<StringRef> getModulePathForSymbol(StringRef Name) const;
  ThreadSafeModule pickUpSingleModuleBlocking(StringRef Path);

  VModuleKey getModuleId(StringRef Path) const {
    return CombinedSummaryIndex.getModuleId(Path);
  }

  struct FetchModuleResult {
    StringRef Path;
    ModuleState State;
    std::shared_future<void> Notifier;
  };

  std::vector<FetchModuleResult> fetchAllModules(const ModuleNames &Paths);
  std::vector<FetchModuleResult> fetchNewModules(const ModuleNames &Paths);

  ModuleNames findModulesInRange(FunctionSummary *Summary,
                                 unsigned LookaheadLevels);
  Optional<ThreadSafeModule> pickUpModule(StringRef Path);
  SymbolLookupSet createLookupSet(const ModuleNames &Paths,
                                  MangleAndInterner &Mangle);

  void setModuleSubmitted(StringRef Path, bool Asynchronously);
  void setModuleEmitted(StringRef Path);
  void setModuleReached(StringRef Path);

  bool hasCallThroughStubEmitted(StringRef Path, StringRef SymbolName);
  void notifySyntheticCallThroughEmitted(
      StringRef ModulePath, const std::vector<GlobalValue::GUID> &Guids);


  struct ModuleInfo {
    SymbolFlagsMap SymbolFlags;
    std::vector<GlobalValue::GUID> Guids;
    bool AllCallable{true};
  };

  using DemangleFunction = std::function<StringRef(SymbolStringPtr)>;

  StringMap<ModuleInfo> fetchModuleInfo(const SymbolLookupSet &Symbols,
                                        char GlobalManglePrefix);

private:
  struct ModuleRecord {
    std::mutex AccessLock;
    ModuleState State;
    ThreadSafeModule TSM;
    std::string OneSymbolName;
    std::shared_future<void> NotifyParsed;
    std::shared_future<void> NotifySubmitted;
    std::promise<void> TriggerOnSubmit;
    std::vector<GlobalValue::GUID> SyntheticCallThroughs;
    bool SubmittedAsynchronously;
    bool IsEntryPointModule;
    unsigned ReachedId;
  };

  ExecutionSession &ES;
  ModuleSummaryIndex CombinedSummaryIndex;
  StringMap<ModuleRecord> ModuleRecords;
  mutable std::mutex ModuleIndexLock;

  uint64_t NextModuleId;
  ThreadPool ParseModuleWorkers;
  bool PrintStats;
  std::shared_future<void> DummyReadyFuture;

  template <typename Func>
  auto withModuleRecordDo(StringRef Path, Func &&F)
      -> decltype(F(std::declval<ModuleRecord &>())) {
    std::unique_lock<std::mutex> LockLookup(ModuleIndexLock);
    assert(ModuleRecords.count(Path) && "Unknown entry");
    ModuleRecord &Entry = ModuleRecords[Path];
    std::lock_guard<std::mutex> LockContent(Entry.AccessLock);
    LockLookup.unlock();
    return F(Entry);
  }

  template <typename SelectedKeys, typename Func>
  void withEachModuleRecordDo(SelectedKeys Paths, Func &&F) {
    std::lock_guard<std::mutex> Lock(ModuleIndexLock);
    for (StringRef Path : Paths) {
      runOnModuleRecordPrelocked(Path, std::forward<Func>(F));
    }
  }

  template <typename Func>
  void withAllModuleRecordsDo(Func &&F) {
    std::lock_guard<std::mutex> Lock(ModuleIndexLock);
    for (const auto &KV : ModuleRecords) {
      runOnModuleRecordPrelocked(KV.first(), std::forward<Func>(F));
    }
  }

  template <typename Func>
  void runOnModuleRecordPrelocked(StringRef Path, Func &&F) {
    assert(ModuleRecords.count(Path) && "Unknown entry");
    ModuleRecord &Entry = ModuleRecords[Path];
    std::lock_guard<std::mutex> LockContent(Entry.AccessLock);
    return F(Path, Entry);
  }


  void parseModuleFromFile(std::string Path, ModuleRecord &Entry);
  Expected<ThreadSafeModule> doParseModule(StringRef Path);

  static std::string findSymbolForLookup(const Module &M);

  static void addToWorklist(std::vector<FunctionSummary *> &List,
                            ArrayRef<FunctionSummary::EdgeTy> Calls);
};

//raw_ostream &operator<<(raw_ostream &OS, ModuleState S) {
//  switch (S) {
//    case ModuleState::Unused:
//      OS << "Unused";
//      break;
//    case ModuleState::Scheduled:
//      OS << "Scheduled";
//      break;
//    case ModuleState::Parsed:
//      OS << "Parsed";
//      break;
//    case ModuleState::PickedUp:
//      OS << "PickedUp";
//      break;
//    case ModuleState::Submitted:
//      OS << "Submitted";
//      break;
//    case ModuleState::Emitted:
//      OS << "Emitted";
//      break;
//  }
//  return OS;
//}

inline std::string toString(ModuleState S) {
  switch (S) {
    case ModuleState::Unused: return "Unused";
    case ModuleState::Scheduled: return "Scheduled";
    case ModuleState::Parsed: return "Parsed";
    case ModuleState::PickedUp: return "PickedUp";
    case ModuleState::Submitted: return "Submitted";
    case ModuleState::Emitted: return "Emitted";
  }
}

} // namespace orc
} // namespace llvm

#endif
