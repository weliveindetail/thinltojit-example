; ModuleID = 'sub23.s'
source_filename = "sub23.c"

define i32 @sub23(i32 %0) {
  %2 = call i32 @workload(i32 %0)
  ret i32 %2
}

declare i32 @workload(i32)

^0 = module: (path: "sub23.s", hash: (1187217899, 2239201331, 3030240015, 3752221195, 912650771))
^1 = gv: (name: "sub23", summaries: (function: (module: ^0, flags: (linkage: external, notEligibleToImport: 0, live: 0, dsoLocal: 0, canAutoHide: 0), insts: 2, calls: ((callee: ^2))))) ; guid = 1031837926433398383
^2 = gv: (name: "workload") ; guid = 2904160244401423009
