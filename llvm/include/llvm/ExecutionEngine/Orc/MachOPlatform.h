//===-- MachOPlatform.h - Utilities for executing MachO in Orc --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Utilities for executing JIT'd MachO in Orc.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_ORC_MACHOPLATFORM_H
#define LLVM_EXECUTIONENGINE_ORC_MACHOPLATFORM_H

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/ExecutionUtils.h"
#include "llvm/ExecutionEngine/Orc/ObjectLinkingLayer.h"

#include <future>
#include <thread>
#include <vector>

namespace llvm {
namespace orc {

class MachOJITDylibInitializers {
public:
  struct SectionExtent {
    SectionExtent() = default;
    SectionExtent(JITTargetAddress Address, uint64_t NumPtrs)
        : Address(Address), NumPtrs(NumPtrs) {}
    JITTargetAddress Address = 0;
    uint64_t NumPtrs = 0;
  };

  void addModInitsSection(SectionExtent ModInit) {
    ModInitSections.push_back(std::move(ModInit));
  }

  void runModInits() const;

  void dump() const;

private:
  using RawPointerSectionList = std::vector<SectionExtent>;

  RawPointerSectionList ModInitSections;
};

class MachOJITDylibDeinitializers {};

/// Mediates between MachO initialization and ExecutionSession state.
class MachOPlatform : public Platform {
public:
  using InitializerSequence =
      std::vector<std::pair<JITDylib *, MachOJITDylibInitializers>>;

  using DeinitializerSequence =
      std::vector<std::pair<JITDylib *, MachOJITDylibDeinitializers>>;

  MachOPlatform(ExecutionSession &ES, ObjectLinkingLayer &ObjLinkingLayer,
                std::unique_ptr<MemoryBuffer> StandardSymbolsObject);

  ExecutionSession &getExecutionSession() const { return ES; }

  Error setupJITDylib(JITDylib &JD) override;
  Error notifyAdding(JITDylib &JD, const MaterializationUnit &MU) override;
  Error notifyRemoving(JITDylib &JD, VModuleKey K) override;
  void notifyReady(JITDylib &JD, SymbolNameVector Symbols) override;

  Expected<InitializerSequence> getInitializerSequence(JITDylib &JD);

  Expected<DeinitializerSequence> getDeinitializerSequence(JITDylib &JD);

private:
  // This ObjectLinkingLayer plugin scans JITLink graphs for __mod_init_func,
  // __objc_classlist and __sel_ref sections and records their extents so that
  // they can be run in the target process.
  class InitScraperPlugin : public ObjectLinkingLayer::Plugin {
  public:
    InitScraperPlugin(MachOPlatform &MP) : MP(MP) {}

    void modifyPassConfig(MaterializationResponsibility &MR, const Triple &TT,
                          jitlink::PassConfiguration &Config);

  private:
    MachOPlatform &MP;
  };

  static std::vector<JITDylib *> getDFSLinkOrder(JITDylib &JD);

  void registerModInits(JITDylib &JD,
                        MachOJITDylibInitializers::SectionExtent ModInits) {
    std::lock_guard<std::mutex> Lock(PlatformMutex);
    InitSeqs[&JD].addModInitsSection(std::move(ModInits));
  }

  std::mutex PlatformMutex;
  ExecutionSession &ES;
  ObjectLinkingLayer &ObjLinkingLayer;
  std::unique_ptr<MemoryBuffer> StandardSymbolsObject;

  DenseMap<JITDylib *, SymbolLookupSet> RegisteredInitSymbols;
  DenseMap<JITDylib *, MachOJITDylibInitializers> InitSeqs;
};

} // end namespace orc
} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_ORC_MACHOPLATFORM_H
