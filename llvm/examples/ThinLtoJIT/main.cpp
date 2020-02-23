#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/TargetSelect.h"

#include "ThinLtoJIT.h"
#include "StatsUtils.h"

#include <string>
#include <vector>

using namespace llvm;
using namespace llvm::orc;

static cl::list<std::string>
    InputFiles(cl::Positional, cl::OneOrMore,
               cl::desc("<bitcode files or global index>"));

static cl::list<std::string> InputArgs("args", cl::Positional,
                                       cl::desc("<program arguments>..."),
                                       cl::ZeroOrMore, cl::PositionalEatsArgs);

static cl::opt<unsigned> LoadThreads("load-threads", cl::Optional,
                                     cl::desc("Threads for parsing modules"),
                                     cl::init(2));

static cl::opt<unsigned> CompileThreads("compile-threads", cl::Optional,
                                        cl::desc("Threads for compiling modules"),
                                        cl::init(4));

static cl::opt<unsigned> DiscoveryThreads("discovery-threads", cl::Optional,
                                          cl::desc("Threads for discovery"),
                                          cl::init(4));

static cl::opt<DispatchMaterialization> DispatchPolicy(
    "dispatch-compilation",
    cl::desc("Which compile jobs to delegate the thread pool"),
    cl::init(DispatchMaterialization::WholeModules),
    cl::values(clEnumValN(DispatchMaterialization::Never, "never",
                          "Never dispatch materialization (compile-threads has "
                          "no effect)"),
               clEnumValN(DispatchMaterialization::WholeModules,
                          "whole-modules", "Only dispatch materialization of "
                          "full modules (default)"),
               clEnumValN(DispatchMaterialization::Always, "orc-lazy",
                          "Always dispatch materialization, including creation "
                          "of reexports and call-through stubs")));

static cl::opt<unsigned>
    LookaheadLevels("lookahead", cl::Optional,
                    cl::desc("Calls to look ahead of execution"), cl::init(4));

static cl::opt<bool> PrintStats("print-stats",
                                cl::desc("Print module stats on shutdown"),
                                cl::init(false));

//  cl::opt<JITKind> UseJITKind(
//      "jit-kind", cl::desc("Choose underlying JIT kind."),
//      cl::init(JITKind::MCJIT),
//      cl::values(clEnumValN(JITKind::MCJIT, "mcjit", "MCJIT"),
//                 clEnumValN(JITKind::OrcMCJITReplacement, "orc-mcjit",
//                            "Orc-based MCJIT replacement "
//                            "(deprecated)"),
//                 clEnumValN(JITKind::OrcLazy, "orc-lazy",
//                            "Orc-based lazy JIT.")));

int main(int argc, char *argv[]) {
  InitLLVM X(argc, argv);
  InitializeNativeTarget();
  InitializeNativeTargetAsmPrinter();
  cl::ParseCommandLineOptions(argc, argv, "ThinLtoJIT");

  Error Err = Error::success();
  auto atLeastOne = [](unsigned N) { return std::max(1u, N); };

  ThinLtoJIT Jit(InputFiles, "main", DispatchPolicy,
                 atLeastOne(LookaheadLevels), atLeastOne(CompileThreads),
                 atLeastOne(LoadThreads), atLeastOne(DiscoveryThreads),
                 PrintStats, Err);
  if (Err) {
    logAllUnhandledErrors(std::move(Err), errs(), "[ThinLtoJIT] ");
    exit(1);
  }

  ExitOnError ExitOnErr;
  ExitOnErr.setBanner("[ThinLtoJIT] ");

  int RetVal = ExitOnErr(Jit.call("main", InputArgs));
  //IFSTATS(Jit.dump(dbgs()));

  return RetVal;
}
