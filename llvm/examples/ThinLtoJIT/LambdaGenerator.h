//===- CompileWholeModuleOnDemandLayer.h ------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// TODO
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXAMPLES_THINLTOJIT_LAMBDAGENERATOR_H
#define LLVM_EXAMPLES_THINLTOJIT_LAMBDAGENERATOR_H

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/Support/Error.h"

#include <functional>
#include <memory>

namespace llvm {
namespace orc {

template <typename Lambda>
class LambdaGenerator : public JITDylib::DefinitionGenerator {
public:
  LambdaGenerator(Lambda TryToGenerate)
      : TryToGenerate(std::move(TryToGenerate)) {}

protected:
  Error tryToGenerate(LookupKind K, JITDylib &JD,
                      JITDylibLookupFlags JDLookupFlags,
                      const SymbolLookupSet &Symbols) override {
    return TryToGenerate(K, JD, JDLookupFlags, Symbols);
  }

private:
  Lambda TryToGenerate;
};

template <typename TryToGenerateFunction>
std::unique_ptr<LambdaGenerator<TryToGenerateFunction>>
onTryToGenerate(TryToGenerateFunction TryToGenerate) {
  return std::make_unique<LambdaGenerator<TryToGenerateFunction>>(
      std::move(TryToGenerate));
}

}
}

#endif
