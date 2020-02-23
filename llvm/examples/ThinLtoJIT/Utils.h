#ifndef LLVM_EXECUTIONENGINE_ORC_UTILS_H
#define LLVM_EXECUTIONENGINE_ORC_UTILS_H

#include "llvm/ADT/Triple.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/OrcABISupport.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/SymbolStringPool.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/raw_ostream.h"

#include <chrono>
#include <future>
#include <memory>

namespace llvm {
namespace orc {

template <typename R>
bool is_ready(const std::shared_future<R> &F) {
  return F.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
}

inline std::shared_future<void> makeReadyFuture() {
  std::promise<void> P;
  std::shared_future<void> F = P.get_future();
  P.set_value();
  assert(is_ready(F));
  return F;
}

inline StringRef stripManglePrefix(SymbolStringPtr Symbol, char Prefix) {
  StringRef Name = *Symbol;
  bool Strip = (Prefix != '\0' && Name[0] == Prefix);
  return Strip ? StringRef(Name.data() + 1, Name.size() - 1) : Name;
}

template <typename Derived, typename ...Args>
Expected<std::unique_ptr<Derived>>
createCallThroughManager(const Triple &T, Args&&... A) {
  switch (T.getArch()) {
  default:
    return make_error<StringError>(
        std::string("No callback manager available for ") + T.str(),
        inconvertibleErrorCode());

  case Triple::aarch64:
  case Triple::aarch64_32:
    return Derived::template Create<OrcAArch64>(std::forward<Args>(A)...);

  case Triple::x86:
    return Derived::template Create<OrcI386>(std::forward<Args>(A)...);

  case Triple::mips:
    return Derived::template Create<OrcMips32Be>(std::forward<Args>(A)...);

  case Triple::mipsel:
    return Derived::template Create<OrcMips32Le>(std::forward<Args>(A)...);

  case Triple::mips64:
  case Triple::mips64el:
    return Derived::template Create<OrcMips64>(std::forward<Args>(A)...);

  case Triple::x86_64:
    if (T.getOS() == Triple::OSType::Win32)
      return Derived::template Create<OrcX86_64_Win32>(
          std::forward<Args>(A)...);
    else
      return Derived::template Create<OrcX86_64_SysV>(
          std::forward<Args>(A)...);
  }
}

}
}

#endif
