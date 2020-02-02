; ModuleID = 'sub2.s'
source_filename = "sub2.c"

define i32 @sub2(i32 %0) {
  %2 = call i32 @sub21(i32 %0)
  %3 = call i32 @max(i32 0, i32 %2)
  %4 = add nsw i32 %0, 1
  %5 = call i32 @sub22(i32 %4)
  %6 = call i32 @max(i32 %3, i32 %5)
  %7 = add nsw i32 %0, 2
  %8 = call i32 @sub23(i32 %7)
  %9 = call i32 @max(i32 %6, i32 %8)
  ret i32 %9
}
declare i32 @max(i32, i32)
declare i32 @sub21(i32)
declare i32 @sub22(i32)
declare i32 @sub23(i32)

^0 = module: (path: "sub2.s", hash: (1967678436, 1954842929, 2113549141, 2651172409, 3315993195))
^1 = gv: (name: "sub23") ; guid = 1031837926433398383
^2 = gv: (name: "max") ; guid = 8185957744030711343
^3 = gv: (name: "sub22") ; guid = 8601446283390290348
^4 = gv: (name: "sub21") ; guid = 12793086811415836454
^5 = gv: (name: "sub2", summaries: (function: (module: ^0, flags: (linkage: external, notEligibleToImport: 0, live: 0, dsoLocal: 0, canAutoHide: 0), insts: 9, calls: ((callee: ^4), (callee: ^2), (callee: ^3), (callee: ^1))))) ; guid = 15968543173600788063
