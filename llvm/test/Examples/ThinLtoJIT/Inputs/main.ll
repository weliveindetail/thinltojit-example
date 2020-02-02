; ModuleID = 'main.s'
source_filename = "main.c"

define i32 @main(i32 %0, i8** nocapture readonly %1) {
  %3 = alloca i8*, align 8
  %4 = icmp sgt i32 %0, 1
  br i1 %4, label %5, label %14

5:
  %6 = bitcast i8** %3 to i8*
  call void @llvm.lifetime.start.p0i8(i64 8, i8* nonnull %6)
  %7 = getelementptr inbounds i8*, i8** %1, i64 1
  %8 = load i8*, i8** %7, align 8
  %9 = call i64 @strtol(i8* %8, i8** nonnull %3, i32 10)
  %10 = load i8*, i8** %3, align 8
  %11 = load i8, i8* %10, align 1
  %12 = icmp eq i8 %11, 0
  %13 = xor i1 %12, true
  call void @llvm.lifetime.end.p0i8(i64 8, i8* nonnull %6)
  br i1 %12, label %14, label %24

14:
  %15 = phi i64 [ %9, %5 ], [ 40, %2 ]
  %16 = call i32 @sub1(i32 3)
  %17 = call i32 @max(i32 0, i32 %16)
  %18 = trunc i64 %15 to i32
  %19 = call i32 @workload(i32 %18)
  %20 = call i32 @max(i32 %17, i32 %19)
  %21 = call i32 @sub2(i32 4)
  %22 = call i32 @max(i32 %20, i32 %21)
  %23 = icmp slt i32 %22, 1
  br label %24

24:
  %25 = phi i1 [ %23, %14 ], [ %13, %5 ]
  %26 = zext i1 %25 to i32
  ret i32 %26
}

declare void @llvm.lifetime.start.p0i8(i64 immarg, i8* nocapture)
declare i64 @strtol(i8* readonly, i8** nocapture, i32)
declare void @llvm.lifetime.end.p0i8(i64 immarg, i8* nocapture)
declare i32 @max(i32, i32)
declare i32 @sub1(i32)
declare i32 @workload(i32)
declare i32 @sub2(i32)

^0 = module: (path: "main.s", hash: (527116715, 3332069392, 338859151, 688235912, 2536759033))
^1 = gv: (name: "workload") ; guid = 2904160244401423009
^2 = gv: (name: "llvm.lifetime.start.p0i8") ; guid = 3657761528566682672
^3 = gv: (name: "sub1") ; guid = 6667511020869341723
^4 = gv: (name: "max") ; guid = 8185957744030711343
^5 = gv: (name: "strtol") ; guid = 10216030853587034628
^6 = gv: (name: "llvm.lifetime.end.p0i8") ; guid = 14311549039910520616
^7 = gv: (name: "main", summaries: (function: (module: ^0, flags: (linkage: external, notEligibleToImport: 0, live: 0, dsoLocal: 0, canAutoHide: 0), insts: 27, calls: ((callee: ^5), (callee: ^3), (callee: ^4), (callee: ^1), (callee: ^8))))) ; guid = 15822663052811949562
^8 = gv: (name: "sub2") ; guid = 15968543173600788063
