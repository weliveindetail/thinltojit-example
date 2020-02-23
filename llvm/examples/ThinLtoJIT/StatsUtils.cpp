#include "llvm/Support/ManagedStatic.h"

#include "StatsUtils.h"

using namespace std::chrono;

namespace llvm {

static ManagedStatic<Timer> GlobalTimer;

Timer::Timer() : Startup(high_resolution_clock::now()) {}

//static
double Timer::elapsed() {
  auto Elapsed = high_resolution_clock::now() - GlobalTimer->Startup;
  return std::max(0.0, duration_cast<duration<double>>(Elapsed).count());
}

}