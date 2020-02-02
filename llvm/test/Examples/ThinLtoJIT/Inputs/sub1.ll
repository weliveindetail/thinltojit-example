; ModuleID = 'sub1.s'
source_filename = "sub1.c"

define i32 @sub1(i32 %0) {
  %2 = call i32 @workload(i32 %0)
  ret i32 %2
}

declare i32 @workload(i32)

^0 = module: (path: "sub1.s", hash: (716800708, 1140043336, 1672117802, 4083982944, 497481660))
^1 = gv: (name: "workload") ; guid = 2904160244401423009
^2 = gv: (name: "sub1", summaries: (function: (module: ^0, flags: (linkage: external, notEligibleToImport: 0, live: 0, dsoLocal: 0, canAutoHide: 0), insts: 2, calls: ((callee: ^1))))) ; guid = 6667511020869341723
