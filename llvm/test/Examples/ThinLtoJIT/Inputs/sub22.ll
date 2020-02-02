; ModuleID = 'sub22.s'
source_filename = "sub22.c"

define i32 @sub22(i32 %0) {
  %2 = call i32 @workload(i32 %0)
  ret i32 %2
}

declare i32 @workload(i32)

^0 = module: (path: "sub22.s", hash: (4057258347, 3191877851, 3120355814, 3202334300, 790339932))
^1 = gv: (name: "workload") ; guid = 2904160244401423009
^2 = gv: (name: "sub22", summaries: (function: (module: ^0, flags: (linkage: external, notEligibleToImport: 0, live: 0, dsoLocal: 0, canAutoHide: 0), insts: 2, calls: ((callee: ^1))))) ; guid = 8601446283390290348
