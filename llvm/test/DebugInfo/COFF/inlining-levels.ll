; RUN: llc -mcpu=core2 -mtriple=i686-pc-win32 < %s -filetype=obj | llvm-readobj --codeview | FileCheck %s --check-prefix=OBJ

; This LL file was generated by running 'clang -O1 -g -gcodeview' on the
; following code:
; int volatile x;
; static inline void f() {
;   ++x;
; }
; static inline void g() {
;   f();
; }
; static inline void h() {
;   g();
; }
; int main() {
;   h();
; }

; OBJ: Subsection [
; OBJ:   SubSectionType: Symbols (0xF1)
; OBJ:   {{.*}}Proc{{.*}}Sym {
; OBJ:   InlineSiteSym {
; OBJ:     Inlinee: h (0x1002)
; OBJ:   }
; OBJ:   InlineSiteSym {
; OBJ:     Inlinee: g (0x1003)
; OBJ:   }
; OBJ:   InlineSiteSym {
; OBJ:     Inlinee: f (0x1004)
; OBJ:   }
; OBJ:   InlineSiteEnd {
; OBJ:   }
; OBJ:   InlineSiteEnd {
; OBJ:   }
; OBJ:   InlineSiteEnd {
; OBJ:   }
; OBJ:   ProcEnd
; OBJ: ]

; ModuleID = 't.cpp'
source_filename = "test/DebugInfo/COFF/inlining-levels.ll"
target datalayout = "e-m:w-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-pc-windows-msvc18.0.0"

@"\01?x@@3HC" = global i32 0, align 4, !dbg !0

; Function Attrs: norecurse nounwind uwtable
define i32 @main() #0 !dbg !12 {
entry:
  %0 = load volatile i32, i32* @"\01?x@@3HC", align 4, !dbg !15, !tbaa !24
  %inc.i.i.i = add nsw i32 %0, 1, !dbg !15
  store volatile i32 %inc.i.i.i, i32* @"\01?x@@3HC", align 4, !dbg !15, !tbaa !24
  ret i32 0, !dbg !28
}

attributes #0 = { norecurse nounwind uwtable "disable-tail-calls"="false" "less-precise-fpmad"="false" "frame-pointer"="none" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+fxsr,+mmx,+sse,+sse2" "unsafe-fp-math"="false" "use-soft-float"="false" }

!llvm.dbg.cu = !{!2}
!llvm.module.flags = !{!8, !9, !10}
!llvm.ident = !{!11}

!0 = !DIGlobalVariableExpression(var: !1, expr: !DIExpression())
!1 = !DIGlobalVariable(name: "x", linkageName: "\01?x@@3HC", scope: !2, file: !3, line: 1, type: !6, isLocal: false, isDefinition: true)
!2 = distinct !DICompileUnit(language: DW_LANG_C_plus_plus, file: !3, producer: "clang version 3.9.0 ", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, enums: !4, globals: !5)
!3 = !DIFile(filename: "t.cpp", directory: "D:\5Csrc\5Cllvm\5Cbuild")
!4 = !{}
!5 = !{!0}
!6 = !DIDerivedType(tag: DW_TAG_volatile_type, baseType: !7)
!7 = !DIBasicType(name: "int", size: 32, align: 32, encoding: DW_ATE_signed)
!8 = !{i32 2, !"CodeView", i32 1}
!9 = !{i32 2, !"Debug Info Version", i32 3}
!10 = !{i32 1, !"PIC Level", i32 2}
!11 = !{!"clang version 3.9.0 "}
!12 = distinct !DISubprogram(name: "main", scope: !3, file: !3, line: 12, type: !13, isLocal: false, isDefinition: true, scopeLine: 12, flags: DIFlagPrototyped, isOptimized: true, unit: !2, retainedNodes: !4)
!13 = !DISubroutineType(types: !14)
!14 = !{!7}
!15 = !DILocation(line: 4, column: 3, scope: !16, inlinedAt: !19)
!16 = distinct !DISubprogram(name: "f", linkageName: "\01?f@@YAXXZ", scope: !3, file: !3, line: 2, type: !17, isLocal: true, isDefinition: true, scopeLine: 2, flags: DIFlagPrototyped, isOptimized: true, unit: !2, retainedNodes: !4)
!17 = !DISubroutineType(types: !18)
!18 = !{null}
!19 = distinct !DILocation(line: 7, column: 3, scope: !20, inlinedAt: !21)
!20 = distinct !DISubprogram(name: "g", linkageName: "\01?g@@YAXXZ", scope: !3, file: !3, line: 6, type: !17, isLocal: true, isDefinition: true, scopeLine: 6, flags: DIFlagPrototyped, isOptimized: true, unit: !2, retainedNodes: !4)
!21 = distinct !DILocation(line: 10, column: 3, scope: !22, inlinedAt: !23)
!22 = distinct !DISubprogram(name: "h", linkageName: "\01?h@@YAXXZ", scope: !3, file: !3, line: 9, type: !17, isLocal: true, isDefinition: true, scopeLine: 9, flags: DIFlagPrototyped, isOptimized: true, unit: !2, retainedNodes: !4)
!23 = distinct !DILocation(line: 13, column: 3, scope: !12)
!24 = !{!25, !25, i64 0}
!25 = !{!"int", !26, i64 0}
!26 = !{!"omnipotent char", !27, i64 0}
!27 = !{!"Simple C/C++ TBAA"}
!28 = !DILocation(line: 14, column: 1, scope: !12)

