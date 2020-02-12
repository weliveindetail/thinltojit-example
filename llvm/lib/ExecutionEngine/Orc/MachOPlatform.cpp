//===------ MachOPlatform.cpp - Utilities for executing MachO in Orc ------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/MachOPlatform.h"

#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Support/BinaryByteStream.h"

namespace llvm {
namespace orc {

void MachOJITDylibInitializers::runModInits() const {
  for (const auto &ModInit : ModInitSections) {
    for (uint64_t I = 0; I != ModInit.NumPtrs; ++I) {
      auto *InitializerAddr = jitTargetAddressToPointer<uintptr_t *>(
          ModInit.Address + (I * sizeof(uintptr_t)));
      auto *Initializer =
          jitTargetAddressToFunction<void (*)()>(*InitializerAddr);
      Initializer();
    }
  }
}

void MachOJITDylibInitializers::dump() const {
  for (auto &Extent : ModInitSections)
    dbgs() << formatv("{0:x16}", Extent.Address) << " -- "
           << formatv("{0:x16}", Extent.Address + 8 * Extent.NumPtrs) << "\n";
}

MachOPlatform::MachOPlatform(
    ExecutionSession &ES, ObjectLinkingLayer &ObjLinkingLayer,
    std::unique_ptr<MemoryBuffer> StandardSymbolsObject)
    : ES(ES), ObjLinkingLayer(ObjLinkingLayer),
      StandardSymbolsObject(std::move(StandardSymbolsObject)) {
  ObjLinkingLayer.addPlugin(std::make_unique<InitScraperPlugin>(*this));
}

Error MachOPlatform::setupJITDylib(JITDylib &JD) {
  auto ObjBuffer = MemoryBuffer::getMemBuffer(
      StandardSymbolsObject->getMemBufferRef(), false);
  return ObjLinkingLayer.add(JD, std::move(ObjBuffer));
}

Error MachOPlatform::notifyAdding(JITDylib &JD, const MaterializationUnit &MU) {
  const auto &InitSym = MU.getInitializerSymbol();
  if (!InitSym)
    return Error::success();

  std::lock_guard<std::mutex> Lock(PlatformMutex);
  RegisteredInitSymbols[&JD].add(InitSym);
  return Error::success();
}

Error MachOPlatform::notifyRemoving(JITDylib &JD, VModuleKey K) {
  llvm_unreachable("Not supported yet");
}

void MachOPlatform::notifyReady(JITDylib &JD, SymbolNameVector Symbols) {
  // We immediately release symbols to clients. In a standard MachO process
  // it's legal to dlsym a symbol as soon as its loaded, and before any
  // initializers have been run.
  notifySymbolsInitialized(JD, Symbols);
}

Expected<MachOPlatform::InitializerSequence>
MachOPlatform::getInitializerSequence(JITDylib &JD) {

  std::vector<JITDylib *> DFSLinkOrder;

  while (true) {
    // Lock the platform while we search for any initializer symbols to
    // look up.
    DenseMap<JITDylib *, SymbolLookupSet> NewInitSymbols;
    {
      std::lock_guard<std::mutex> Lock(PlatformMutex);
      DFSLinkOrder = getDFSLinkOrder(JD);

      for (auto *InitJD : DFSLinkOrder) {
        auto RISItr = RegisteredInitSymbols.find(InitJD);
        if (RISItr != RegisteredInitSymbols.end()) {
          NewInitSymbols[InitJD] = std::move(RISItr->second);
          RegisteredInitSymbols.erase(RISItr);
        }
      }
    }

    if (NewInitSymbols.empty())
      break;

    // Outside the lock, issue the lookup.
    if (auto R = lookupInitSymbols(JD.getExecutionSession(), NewInitSymbols))
      ; // Nothing to do in the success case.
    else
      return R.takeError();
  }

  // Lock again to collect the initializers.
  InitializerSequence FullInitSeq;
  {
    std::lock_guard<std::mutex> Lock(PlatformMutex);
    for (auto *InitJD : reverse(DFSLinkOrder)) {
      auto ISItr = InitSeqs.find(InitJD);
      if (ISItr != InitSeqs.end()) {
        FullInitSeq.emplace_back(InitJD, std::move(ISItr->second));
        InitSeqs.erase(ISItr);
      }
    }
  }

  return FullInitSeq;
}

Expected<MachOPlatform::DeinitializerSequence>
MachOPlatform::getDeinitializerSequence(JITDylib &JD) {
  std::vector<JITDylib *> DFSLinkOrder = getDFSLinkOrder(JD);

  DeinitializerSequence FullDeinitSeq;
  {
    std::lock_guard<std::mutex> Lock(PlatformMutex);
    for (auto *DeinitJD : DFSLinkOrder) {
      FullDeinitSeq.emplace_back(DeinitJD, MachOJITDylibDeinitializers());
    }
  }

  return FullDeinitSeq;
}

std::vector<JITDylib *> MachOPlatform::getDFSLinkOrder(JITDylib &JD) {
  std::vector<JITDylib *> Result, WorkStack({&JD});
  DenseSet<JITDylib *> Visited;

  while (!WorkStack.empty()) {
    auto *NextJD = WorkStack.back();
    WorkStack.pop_back();
    if (Visited.count(NextJD))
      continue;
    Visited.insert(NextJD);
    Result.push_back(NextJD);
    NextJD->withSearchOrderDo([&](const JITDylibSearchOrder &SO) {
      for (auto &KV : SO)
        WorkStack.push_back(KV.first);
    });
  }

  return Result;
}

static Expected<MachOJITDylibInitializers::SectionExtent>
getSectionExtent(jitlink::LinkGraph &G, StringRef SectionName) {
  auto *Sec = G.findSectionByName(SectionName);
  if (!Sec)
    return MachOJITDylibInitializers::SectionExtent();
  jitlink::SectionRange R(*Sec);
  if (R.getSize() % G.getPointerSize() != 0)
    return make_error<StringError>(SectionName + " section size is not a "
                                                 "multiple of the pointer size",
                                   inconvertibleErrorCode());
  return MachOJITDylibInitializers::SectionExtent(
      R.getStart(), R.getSize() / G.getPointerSize());
}

void MachOPlatform::InitScraperPlugin::modifyPassConfig(
    MaterializationResponsibility &MR, const Triple &TT,
    jitlink::PassConfiguration &Config) {

  Config.PrePrunePasses.push_back([](jitlink::LinkGraph &G) -> Error {
    // If there's a mod_inits section, add a live anon symbol to prevent
    // it from being dead-stripped.
    // FIXME: MachOLinkGraphBuilder should probably do this by default.
    if (auto *ModInits = G.findSectionByName("__mod_init_func")) {
      auto ModInitsBlocks = ModInits->blocks();
      if (!llvm::empty(ModInitsBlocks))
        G.addAnonymousSymbol(**ModInitsBlocks.begin(), 0, 0, false, true);
    }
    return Error::success();
  });

  Config.PostFixupPasses.push_back(
      [this, &JD = MR.getTargetJITDylib()](jitlink::LinkGraph &G) -> Error {
        MachOJITDylibInitializers::SectionExtent ModInits;
        if (auto ModInitExtentOrErr = getSectionExtent(G, "__mod_init_func"))
          ModInits = std::move(*ModInitExtentOrErr);
        else
          return ModInitExtentOrErr.takeError();

        if (ModInits.Address)
          MP.registerModInits(JD, std::move(ModInits));

        return Error::success();
      });
}

} // End namespace orc.
} // End namespace llvm.
