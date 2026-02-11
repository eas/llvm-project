//===- llvm/unittests/Transforms/Vectorize/VPlanPatternMatchTest.cpp ------===//
//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../lib/Transforms/Vectorize/VPlanPatternMatch.h"
#include "../lib/Transforms/Vectorize/LoopVectorizationPlanner.h"
#include "../lib/Transforms/Vectorize/VPlan.h"
#include "../lib/Transforms/Vectorize/VPlanHelpers.h"
#include "VPlanTestBase.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "gtest/gtest.h"

namespace llvm {

namespace {
using VPPatternMatchTest = VPlanTestBase;

TEST_F(VPPatternMatchTest, ScalarIVSteps) {
  VPlan &Plan = getPlan();
  VPBasicBlock *VPBB = Plan.createVPBasicBlock("");
  VPBuilder Builder(VPBB);

  IntegerType *I64Ty = IntegerType::get(C, 64);
  VPIRValue *StartV = Plan.getConstantInt(I64Ty, 0);
  auto *CanonicalIVPHI = new VPCanonicalIVPHIRecipe(StartV, DebugLoc());
  Builder.insert(CanonicalIVPHI);

  VPValue *Inc = Plan.getOrAddLiveIn(ConstantInt::get(I64Ty, 1));
  VPValue *VF = &Plan.getVF();
  VPValue *Steps = Builder.createScalarIVSteps(
      Instruction::Add, nullptr, CanonicalIVPHI, Inc, VF, DebugLoc());

  VPValue *Inc2 = Plan.getOrAddLiveIn(ConstantInt::get(I64Ty, 2));
  VPValue *Steps2 = Builder.createScalarIVSteps(
      Instruction::Add, nullptr, CanonicalIVPHI, Inc2, VF, DebugLoc());

  using namespace VPlanPatternMatch;

  ASSERT_TRUE(match(Steps, m_ScalarIVSteps(m_Specific(CanonicalIVPHI),
                                           m_SpecificInt(1), m_Specific(VF))));
  ASSERT_FALSE(
      match(Steps2, m_ScalarIVSteps(m_Specific(CanonicalIVPHI),
                                    m_SpecificInt(1), m_Specific(VF))));
  ASSERT_TRUE(match(Steps2, m_ScalarIVSteps(m_Specific(CanonicalIVPHI),
                                            m_SpecificInt(2), m_Specific(VF))));
}

TEST_F(VPPatternMatchTest, GetElementPtr) {
  VPlan &Plan = getPlan();
  VPBasicBlock *VPBB = Plan.createVPBasicBlock("entry");
  VPBuilder Builder(VPBB);

  IntegerType *I64Ty = IntegerType::get(C, 64);
  VPValue *One = Plan.getOrAddLiveIn(ConstantInt::get(I64Ty, 1));
  VPValue *Two = Plan.getOrAddLiveIn(ConstantInt::get(I64Ty, 2));
  VPValue *Ptr =
      Plan.getOrAddLiveIn(Constant::getNullValue(PointerType::get(C, 0)));

  VPInstruction *PtrAdd = Builder.createPtrAdd(Ptr, One);
  VPInstruction *WidePtrAdd = Builder.createWidePtrAdd(Ptr, Two);

  using namespace VPlanPatternMatch;
  ASSERT_TRUE(
      match(PtrAdd, m_GetElementPtr(m_Specific(Ptr), m_SpecificInt(1))));
  ASSERT_FALSE(
      match(PtrAdd, m_GetElementPtr(m_Specific(Ptr), m_SpecificInt(2))));
  ASSERT_TRUE(
      match(WidePtrAdd, m_GetElementPtr(m_Specific(Ptr), m_SpecificInt(2))));
  ASSERT_FALSE(
      match(WidePtrAdd, m_GetElementPtr(m_Specific(Ptr), m_SpecificInt(1))));
}

TEST_F(VPPatternMatchTest, Lambda) {
  VPlan &Plan = getPlan();
  VPBasicBlock *VPBB = Plan.createVPBasicBlock("entry");
  VPBuilder Builder(VPBB);

  IntegerType *I64Ty = IntegerType::get(C, 64);
  VPValue *One = Plan.getOrAddLiveIn(ConstantInt::get(I64Ty, 1));
  VPValue *Two = Plan.getOrAddLiveIn(ConstantInt::get(I64Ty, 2));
  VPValue *Ptr =
      Plan.getOrAddLiveIn(Constant::getNullValue(PointerType::get(C, 0)));

  VPInstruction *PtrOne = Builder.createPtrAdd(Ptr, One);
  VPInstruction *PtrTwo = Builder.createPtrAdd(Ptr, Two);

  VPValue *Three = Builder.createAdd(One, Two);
  VPInstruction *PtrThree = Builder.createPtrAdd(PtrOne, Three);

  using VPlanPatternMatch::m_Lambda;

  // Double lambda to define a new "matcher" that can be used multiple times.
  auto m_Specific = [](VPValue *Specific) {
    return m_Lambda([=](auto *V) { return V == Specific; } );
  };

  ASSERT_TRUE(match(One, m_Specific(One)));
  ASSERT_FALSE(match(Two, m_Specific(One)));

  // Or can "inline" for a single-use matcher.
  VPValue *Captured = nullptr;
  ASSERT_TRUE(match(One, m_Lambda([&Captured](auto *V) {
                      Captured = V;
                      return true;
                    })));
  ASSERT_EQ(One, Captured);

  // Multi-use leaf (no operands), just one lambda suffices.
  auto m_PtrOne = m_Lambda([One](auto *V) {
    auto *R = dyn_cast<VPInstruction>(V);
    return R && R->getOpcode() == VPInstruction::PtrAdd &&
           R->getOperand(1) == One;
  });
  ASSERT_TRUE(match(PtrOne, m_PtrOne));
  ASSERT_FALSE(match(PtrTwo, m_PtrOne));

  // Combining plus some unnecessary C++ magic complications..
  auto m_Ptr = [](auto &&IdxMatcher) {
    return m_Lambda([IdxMatcher = std::move(IdxMatcher)](auto *V) {
      auto *R = dyn_cast<VPInstruction>(V);
      return R && R->getOpcode() == VPInstruction::PtrAdd &&
             match(R->getOperand(1), IdxMatcher);
    });
  };

  ASSERT_TRUE(match(PtrOne, m_Ptr(m_Specific(One))));
  ASSERT_FALSE(match(PtrOne, m_Ptr(m_Specific(Two))));

  // Even more complicated plus mixed usage with "normal" matchers.
  auto m_Bind = [](auto *&BindTo, auto Matcher) {
    return m_Lambda([Matcher, &BindTo](auto *V) {
      if (!match(V, Matcher))
        return false;
      BindTo = cast<
          std::remove_pointer_t<std::remove_reference_t<decltype(BindTo)>>>(V);
      return true;
    });
  };

  VPInstruction *CapturedThree = nullptr;
  auto m_PtrThree = m_Ptr(m_Bind(
      CapturedThree, VPlanPatternMatch::m_c_Add(m_Specific(Two), m_Specific(One))));
  ASSERT_FALSE(match(PtrOne, m_PtrThree));
  ASSERT_EQ(CapturedThree, nullptr);
  ASSERT_TRUE(match(PtrThree, m_PtrThree));
  ASSERT_EQ(CapturedThree, Three);
}
} // namespace
} // namespace llvm
