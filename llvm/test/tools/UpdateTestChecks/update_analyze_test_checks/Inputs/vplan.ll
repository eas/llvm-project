; RUN: opt -passes=loop-vectorize -disable-output  < %s -vplan-print-after=printAfterInitialConstruction 2>&1 | FileCheck %s
; REQUIRES: asserts

define void @foo(ptr %ptr, i64 %n) {
entry:
  br label %header

header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %header ]
  %gep = getelementptr i64, ptr %ptr, i64 %iv
  store i64 %iv, ptr %gep
  %iv.next = add nsw i64 %iv, 1
  %exitcond = icmp slt i64 %iv.next, %n
  br i1 %exitcond, label %header, label %exit

exit:
  ret void
}

define void @bar(ptr %ptr, i64 %n) {
entry:
  br label %header

header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %gep = getelementptr i64, ptr %ptr, i64 %iv
  %c = icmp sle i64 %iv, 42
  br i1 %c, label %if, label %latch

if:
  store i64 %iv, ptr %gep
  br label %latch

latch:
  %iv.next = add nsw i64 %iv, 1
  %exitcond = icmp slt i64 %iv.next, %n
  br i1 %exitcond, label %header, label %exit

exit:
  ret void
}
