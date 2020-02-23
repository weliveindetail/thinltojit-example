#ifndef LLVM_EXAMPLES_THINLTOJIT_STATSUTILS_H
#define LLVM_EXAMPLES_THINLTOJIT_STATSUTILS_H

#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <set>
#include <vector>

#ifdef THINLTOJIT_STATS
#define IFSTATS(X) do { X; } while (false)
#else
#define IFSTATS(X)
#endif

#define PSTATS(...) IFSTATS( \
  PrintBuffered(dbgs(), format("%4.5f: ", Timer::elapsed()), __VA_ARGS__, "\n"))

namespace llvm {

class Timer {
public:
  Timer();
  static double elapsed();
private:
  std::chrono::high_resolution_clock::time_point Startup;
};

template <typename ...Args>
void PrintBuffered(raw_ostream& OS, Args&&... args) {
  std::string Buffer;
  raw_string_ostream SOS(Buffer);
  PrintUnpack(SOS, args...);
  OS << SOS.str();
}

template <typename Arg, typename... Args>
void PrintUnpack(raw_ostream& OS, Arg&& arg, Args&&... args) {
  OS << std::forward<Arg>(arg);
  PrintUnpack(OS, args...);
}

inline void PrintUnpack(raw_ostream& OS) {}

inline raw_ostream &operator<<(raw_ostream &OS, const std::vector<StringRef> &Ss) {
  bool PrependSeperator = false;
  for (StringRef S : Ss) {
    OS << (PrependSeperator ? ", " : "") << S;
    PrependSeperator = true;
  }
  return OS;
}

inline raw_ostream &operator<<(raw_ostream &OS, const std::set<StringRef> &Ss) {
  bool PrependSeperator = false;
  for (StringRef S : Ss) {
    OS << (PrependSeperator ? ", " : "") << S;
    PrependSeperator = true;
  }
  return OS;
}

}

#endif
