; ModuleID = 'sub21.s'
source_filename = "sub21.c"

define i32 @sub21(i32 %0) {
  %2 = call i32 @workload(i32 %0)
  ret i32 %2
}

declare i32 @workload(i32)

^0 = module: (path: "sub21.s", hash: (2324013215, 2786549134, 2291471355, 584671689, 2308799624))
^1 = gv: (name: "workload") ; guid = 2904160244401423009
^2 = gv: (name: "sub21", summaries: (function: (module: ^0, flags: (linkage: external, notEligibleToImport: 0, live: 0, dsoLocal: 0, canAutoHide: 0), insts: 2, calls: ((callee: ^1))))) ; guid = 12793086811415836454
