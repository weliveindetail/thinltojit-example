; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt < %s -indvars -indvars-predicate-loops=0 -S | FileCheck %s
target datalayout = "e-p:32:32:32-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:32:64-f32:32:32-f64:32:64-v64:64:64-v128:128:128-a0:0:64-f80:128:128"
target triple = "i386-apple-darwin10.0"

; We make sinking here, Changed flag should be set properly.
define i32 @test(i32 %a, i32 %b, i32 %N) {
; CHECK-LABEL: @test(
; CHECK-NEXT:  entry:
; CHECK-NEXT:    br label [[LOOP:%.*]]
; CHECK:       loop:
; CHECK-NEXT:    [[IV:%.*]] = phi i32 [ 0, [[ENTRY:%.*]] ], [ [[IV_NEXT:%.*]], [[LOOP]] ]
; CHECK-NEXT:    [[IV_NEXT]] = add nuw nsw i32 [[IV]], 1
; CHECK-NEXT:    [[CMP:%.*]] = icmp slt i32 [[IV_NEXT]], [[N:%.*]]
; CHECK-NEXT:    br i1 [[CMP]], label [[LOOP]], label [[EXIT:%.*]]
; CHECK:       exit:
; CHECK-NEXT:    [[ADD:%.*]] = add i32 [[A:%.*]], [[B:%.*]]
; CHECK-NEXT:    ret i32 [[ADD]]
;
entry:
  %add = add i32 %a, %b
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add i32 %iv, 1
  %cmp = icmp slt i32 %iv.next, %N
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %add
}
