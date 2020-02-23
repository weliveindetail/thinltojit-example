set(LLVM_LINK_COMPONENTS
  Core
  IRReader
  OrcJIT
  ExecutionEngine
  Support
  nativecodegen
  Analysis
  Passes
  )

add_llvm_example(ThinLtoJIT
  main.cpp
  Errors.cpp
  ModuleIndex.cpp
  OnDemandLayer.cpp
  StatsUtils.cpp
  ThinLtoJIT.cpp
  )

target_compile_definitions(ThinLtoJIT PRIVATE -DTHINLTOJIT_STATS)
