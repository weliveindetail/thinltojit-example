; ModuleID = 'workload.s'
source_filename = "workload.c"

define i32 @max(i32 %0, i32 %1) {
  %3 = icmp sgt i32 %0, %1
  %4 = select i1 %3, i32 %0, i32 %1
  ret i32 %4
}

define i32 @workload(i32 %0) {
  %2 = icmp slt i32 %0, 64
  %3 = select i1 %2, i32 %0, i32 64
  %4 = icmp sgt i32 %3, 1
  br i1 %4, label %5, label %12

5:
  %6 = phi i32 [ %10, %5 ], [ 1, %1 ]
  %7 = phi i64 [ %9, %5 ], [ 1, %1 ]
  %8 = phi i64 [ %7, %5 ], [ 0, %1 ]
  %9 = add i64 %7, %8
  %10 = add nuw nsw i32 %6, 1
  %11 = icmp slt i32 %10, %3
  br i1 %11, label %5, label %12

12:
  %13 = phi i64 [ 1, %1 ], [ %9, %5 ]
  %14 = call fastcc i32 @fact(i64 %13)
  ret i32 %14
}

define internal fastcc i32 @fact(i64 %0) {
  %2 = icmp ult i64 %0, 2
  br i1 %2, label %26, label %3

3:
  %4 = phi i32 [ %23, %22 ], [ 0, %1 ]
  %5 = phi i64 [ %24, %22 ], [ 2, %1 ]
  %6 = urem i64 %0, %5
  %7 = icmp eq i64 %6, 0
  br i1 %7, label %8, label %22

8:
  %9 = lshr i64 %5, 1
  %10 = icmp ult i64 %5, 4
  br i1 %10, label %18, label %14

11:
  %12 = add nuw i64 %15, 1
  %13 = icmp ult i64 %15, %9
  br i1 %13, label %14, label %18

14:
  %15 = phi i64 [ %12, %11 ], [ 2, %8 ]
  %16 = urem i64 %5, %15
  %17 = icmp eq i64 %16, 0
  br i1 %17, label %18, label %11

18:
  %19 = phi i1 [ true, %8 ], [ true, %11 ], [ false, %14 ]
  %20 = trunc i64 %5 to i32
  %21 = select i1 %19, i32 %20, i32 %4
  br label %22

22:
  %23 = phi i32 [ %21, %18 ], [ %4, %3 ]
  %24 = add i64 %5, 1
  %25 = icmp ugt i64 %24, %0
  br i1 %25, label %26, label %3

26:
  %27 = phi i32 [ 0, %1 ], [ %23, %22 ]
  ret i32 %27
}

^0 = module: (path: "workload.s", hash: (3100996161, 646883963, 3338709300, 3710134105, 1603428348))
^1 = gv: (name: "workload", summaries: (function: (module: ^0, flags: (linkage: external, notEligibleToImport: 0, live: 0, dsoLocal: 0, canAutoHide: 0), insts: 14, funcFlags: (readNone: 1, readOnly: 0, noRecurse: 1, returnDoesNotAlias: 0, noInline: 0, alwaysInline: 0), calls: ((callee: ^3))))) ; guid = 2904160244401423009
^2 = gv: (name: "max", summaries: (function: (module: ^0, flags: (linkage: external, notEligibleToImport: 0, live: 0, dsoLocal: 0, canAutoHide: 0), insts: 3, funcFlags: (readNone: 1, readOnly: 0, noRecurse: 1, returnDoesNotAlias: 0, noInline: 0, alwaysInline: 0)))) ; guid = 8185957744030711343
^3 = gv: (name: "fact", summaries: (function: (module: ^0, flags: (linkage: internal, notEligibleToImport: 0, live: 0, dsoLocal: 1, canAutoHide: 0), insts: 27, funcFlags: (readNone: 1, readOnly: 0, noRecurse: 1, returnDoesNotAlias: 0, noInline: 0, alwaysInline: 0)))) ; guid = 17650377645318605574
