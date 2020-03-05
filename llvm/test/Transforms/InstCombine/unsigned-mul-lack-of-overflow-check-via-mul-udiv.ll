; NOTE: Assertions have been autogenerated by utils/update_test_checks.py
; RUN: opt %s -instcombine -S | FileCheck %s

; Fold
;   ((%x * %y) u/ %x) == %y
; to
;   @llvm.umul.with.overflow(%x, %y) + extractvalue + not

define i1 @t0_basic(i8 %x, i8 %y) {
; CHECK-LABEL: @t0_basic(
; CHECK-NEXT:    [[UMUL:%.*]] = call { i8, i1 } @llvm.umul.with.overflow.i8(i8 [[X:%.*]], i8 [[Y:%.*]])
; CHECK-NEXT:    [[UMUL_OV:%.*]] = extractvalue { i8, i1 } [[UMUL]], 1
; CHECK-NEXT:    [[UMUL_NOT_OV:%.*]] = xor i1 [[UMUL_OV]], true
; CHECK-NEXT:    ret i1 [[UMUL_NOT_OV]]
;
  %t0 = mul i8 %x, %y
  %t1 = udiv i8 %t0, %x
  %r = icmp eq i8 %t1, %y
  ret i1 %r
}

define <2 x i1> @t1_vec(<2 x i8> %x, <2 x i8> %y) {
; CHECK-LABEL: @t1_vec(
; CHECK-NEXT:    [[UMUL:%.*]] = call { <2 x i8>, <2 x i1> } @llvm.umul.with.overflow.v2i8(<2 x i8> [[X:%.*]], <2 x i8> [[Y:%.*]])
; CHECK-NEXT:    [[UMUL_OV:%.*]] = extractvalue { <2 x i8>, <2 x i1> } [[UMUL]], 1
; CHECK-NEXT:    [[UMUL_NOT_OV:%.*]] = xor <2 x i1> [[UMUL_OV]], <i1 true, i1 true>
; CHECK-NEXT:    ret <2 x i1> [[UMUL_NOT_OV]]
;
  %t0 = mul <2 x i8> %x, %y
  %t1 = udiv <2 x i8> %t0, %x
  %r = icmp eq <2 x i8> %t1, %y
  ret <2 x i1> %r
}

declare i8 @gen8()

define i1 @t2_commutative(i8 %x) {
; CHECK-LABEL: @t2_commutative(
; CHECK-NEXT:    [[Y:%.*]] = call i8 @gen8()
; CHECK-NEXT:    [[UMUL:%.*]] = call { i8, i1 } @llvm.umul.with.overflow.i8(i8 [[X:%.*]], i8 [[Y]])
; CHECK-NEXT:    [[UMUL_OV:%.*]] = extractvalue { i8, i1 } [[UMUL]], 1
; CHECK-NEXT:    [[UMUL_NOT_OV:%.*]] = xor i1 [[UMUL_OV]], true
; CHECK-NEXT:    ret i1 [[UMUL_NOT_OV]]
;
  %y = call i8 @gen8()
  %t0 = mul i8 %y, %x ; swapped
  %t1 = udiv i8 %t0, %x
  %r = icmp eq i8 %t1, %y
  ret i1 %r
}

define i1 @t3_commutative(i8 %x) {
; CHECK-LABEL: @t3_commutative(
; CHECK-NEXT:    [[Y:%.*]] = call i8 @gen8()
; CHECK-NEXT:    [[UMUL:%.*]] = call { i8, i1 } @llvm.umul.with.overflow.i8(i8 [[X:%.*]], i8 [[Y]])
; CHECK-NEXT:    [[UMUL_OV:%.*]] = extractvalue { i8, i1 } [[UMUL]], 1
; CHECK-NEXT:    [[UMUL_NOT_OV:%.*]] = xor i1 [[UMUL_OV]], true
; CHECK-NEXT:    ret i1 [[UMUL_NOT_OV]]
;
  %y = call i8 @gen8()
  %t0 = mul i8 %y, %x ; swapped
  %t1 = udiv i8 %t0, %x
  %r = icmp eq i8 %t1, %y
  ret i1 %r
}

define i1 @t4_commutative(i8 %x) {
; CHECK-LABEL: @t4_commutative(
; CHECK-NEXT:    [[Y:%.*]] = call i8 @gen8()
; CHECK-NEXT:    [[UMUL:%.*]] = call { i8, i1 } @llvm.umul.with.overflow.i8(i8 [[X:%.*]], i8 [[Y]])
; CHECK-NEXT:    [[UMUL_OV:%.*]] = extractvalue { i8, i1 } [[UMUL]], 1
; CHECK-NEXT:    [[UMUL_NOT_OV:%.*]] = xor i1 [[UMUL_OV]], true
; CHECK-NEXT:    ret i1 [[UMUL_NOT_OV]]
;
  %y = call i8 @gen8()
  %t0 = mul i8 %y, %x ; swapped
  %t1 = udiv i8 %t0, %x
  %r = icmp eq i8 %y, %t1 ; swapped
  ret i1 %r
}

; Extra-use tests

declare void @use8(i8)

define i1 @t5_extrause0(i8 %x, i8 %y) {
; CHECK-LABEL: @t5_extrause0(
; CHECK-NEXT:    [[UMUL:%.*]] = call { i8, i1 } @llvm.umul.with.overflow.i8(i8 [[X:%.*]], i8 [[Y:%.*]])
; CHECK-NEXT:    [[UMUL_VAL:%.*]] = extractvalue { i8, i1 } [[UMUL]], 0
; CHECK-NEXT:    [[UMUL_OV:%.*]] = extractvalue { i8, i1 } [[UMUL]], 1
; CHECK-NEXT:    [[UMUL_NOT_OV:%.*]] = xor i1 [[UMUL_OV]], true
; CHECK-NEXT:    call void @use8(i8 [[UMUL_VAL]])
; CHECK-NEXT:    ret i1 [[UMUL_NOT_OV]]
;
  %t0 = mul i8 %x, %y
  call void @use8(i8 %t0)
  %t1 = udiv i8 %t0, %x
  %r = icmp eq i8 %t1, %y
  ret i1 %r
}

define i1 @t6_extrause1(i8 %x, i8 %y) {
; CHECK-LABEL: @t6_extrause1(
; CHECK-NEXT:    [[T0:%.*]] = mul i8 [[X:%.*]], [[Y:%.*]]
; CHECK-NEXT:    [[T1:%.*]] = udiv i8 [[T0]], [[X]]
; CHECK-NEXT:    call void @use8(i8 [[T1]])
; CHECK-NEXT:    [[R:%.*]] = icmp eq i8 [[T1]], [[Y]]
; CHECK-NEXT:    ret i1 [[R]]
;
  %t0 = mul i8 %x, %y
  %t1 = udiv i8 %t0, %x
  call void @use8(i8 %t1)
  %r = icmp eq i8 %t1, %y
  ret i1 %r
}

define i1 @t7_extrause2(i8 %x, i8 %y) {
; CHECK-LABEL: @t7_extrause2(
; CHECK-NEXT:    [[T0:%.*]] = mul i8 [[X:%.*]], [[Y:%.*]]
; CHECK-NEXT:    call void @use8(i8 [[T0]])
; CHECK-NEXT:    [[T1:%.*]] = udiv i8 [[T0]], [[X]]
; CHECK-NEXT:    call void @use8(i8 [[T1]])
; CHECK-NEXT:    [[R:%.*]] = icmp eq i8 [[T1]], [[Y]]
; CHECK-NEXT:    ret i1 [[R]]
;
  %t0 = mul i8 %x, %y
  call void @use8(i8 %t0)
  %t1 = udiv i8 %t0, %x
  call void @use8(i8 %t1)
  %r = icmp eq i8 %t1, %y
  ret i1 %r
}

; Negative tests

define i1 @n8_different_x(i8 %x0, i8 %x1, i8 %y) {
; CHECK-LABEL: @n8_different_x(
; CHECK-NEXT:    [[T0:%.*]] = mul i8 [[X0:%.*]], [[Y:%.*]]
; CHECK-NEXT:    [[T1:%.*]] = udiv i8 [[T0]], [[X1:%.*]]
; CHECK-NEXT:    [[R:%.*]] = icmp eq i8 [[T1]], [[Y]]
; CHECK-NEXT:    ret i1 [[R]]
;
  %t0 = mul i8 %x0, %y
  %t1 = udiv i8 %t0, %x1
  %r = icmp eq i8 %t1, %y
  ret i1 %r
}

define i1 @n9_different_y(i8 %x, i8 %y0, i8 %y1) {
; CHECK-LABEL: @n9_different_y(
; CHECK-NEXT:    [[T0:%.*]] = mul i8 [[X:%.*]], [[Y0:%.*]]
; CHECK-NEXT:    [[T1:%.*]] = udiv i8 [[T0]], [[X]]
; CHECK-NEXT:    [[R:%.*]] = icmp eq i8 [[T1]], [[Y1:%.*]]
; CHECK-NEXT:    ret i1 [[R]]
;
  %t0 = mul i8 %x, %y0
  %t1 = udiv i8 %t0, %x
  %r = icmp eq i8 %t1, %y1
  ret i1 %r
}

define i1 @n10_wrong_pred(i8 %x, i8 %y) {
; CHECK-LABEL: @n10_wrong_pred(
; CHECK-NEXT:    [[T0:%.*]] = mul i8 [[X:%.*]], [[Y:%.*]]
; CHECK-NEXT:    [[T1:%.*]] = udiv i8 [[T0]], [[X]]
; CHECK-NEXT:    [[R:%.*]] = icmp ult i8 [[T1]], [[Y]]
; CHECK-NEXT:    ret i1 [[R]]
;
  %t0 = mul i8 %x, %y
  %t1 = udiv i8 %t0, %x
  %r = icmp ult i8 %t1, %y
  ret i1 %r
}
