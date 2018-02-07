/*
   american fuzzy lop - LLVM-mode instrumentation pass
   ---------------------------------------------------

   Written by Laszlo Szekeres <lszekeres@google.com> and
              Michal Zalewski <lcamtuf@google.com>

   LLVM integration design comes from Laszlo Szekeres. C bits copied-and-pasted
   from afl-as.c are Michal's fault.

   Copyright 2015, 2016 Google Inc. All rights reserved.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at:

     http://www.apache.org/licenses/LICENSE-2.0

   This library is plugged into LLVM when invoking clang through afl-clang-fast.
   It tells the compiler to add code roughly equivalent to the bits discussed
   in ../afl-as.h.

 */

#define AFL_LLVM_PASS

#include "../config.h"
#include "../debug.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"

using namespace llvm;

namespace {

  class AFLCoverage : public ModulePass {

    public:

      static char ID;
      AFLCoverage() : ModulePass(ID) { }

      bool runOnModule(Module &M) override;

      // StringRef getPassName() const override {
      //  return "American Fuzzy Lop Instrumentation";
      // }

  };

  class ExecutionIndexing : public ModulePass {
    public:
      static char ID;
      ExecutionIndexing() : ModulePass(ID) { }
      bool runOnModule(Module &M) override;
  };


}


char AFLCoverage::ID = 0;


bool AFLCoverage::runOnModule(Module &M) {

  LLVMContext &C = M.getContext();

  IntegerType *Int8Ty  = IntegerType::getInt8Ty(C);
  IntegerType *Int32Ty = IntegerType::getInt32Ty(C);

  /* Show a banner */

  char be_quiet = 0;

  if (isatty(2) && !getenv("AFL_QUIET")) {

    SAYF(cCYA "afl-llvm-pass " cBRI VERSION cRST " by <lszekeres@google.com>\n");

  } else be_quiet = 1;

  /* Decide instrumentation ratio */

  char* inst_ratio_str = getenv("AFL_INST_RATIO");
  unsigned int inst_ratio = 100;

  if (inst_ratio_str) {

    if (sscanf(inst_ratio_str, "%u", &inst_ratio) != 1 || !inst_ratio ||
        inst_ratio > 100)
      FATAL("Bad value of AFL_INST_RATIO (must be between 1 and 100)");

  }

  /* Get globals for the SHM region and the previous location. Note that
     __afl_prev_loc is thread-local. */

  GlobalVariable *AFLMapPtr =
      new GlobalVariable(M, PointerType::get(Int8Ty, 0), false,
                         GlobalValue::ExternalLinkage, 0, "__afl_area_ptr");

  GlobalVariable *AFLPrevLoc = new GlobalVariable(
      M, Int32Ty, false, GlobalValue::ExternalLinkage, 0, "__afl_prev_loc",
      0, GlobalVariable::GeneralDynamicTLSModel, 0, false);

  /* Instrument all the things! */

  int inst_blocks = 0;

  for (auto &F : M)
    for (auto &BB : F) {

      BasicBlock::iterator IP = BB.getFirstInsertionPt();
      IRBuilder<> IRB(&(*IP));

      if (AFL_R(100) >= inst_ratio) continue;

      /* Make up cur_loc */

      unsigned int cur_loc = AFL_R(MAP_SIZE);

      ConstantInt *CurLoc = ConstantInt::get(Int32Ty, cur_loc);

      /* Load prev_loc */

      LoadInst *PrevLoc = IRB.CreateLoad(AFLPrevLoc);
      PrevLoc->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
      Value *PrevLocCasted = IRB.CreateZExt(PrevLoc, IRB.getInt32Ty());

      /* Load SHM pointer */

      LoadInst *MapPtr = IRB.CreateLoad(AFLMapPtr);
      MapPtr->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
      Value *MapPtrIdx =
          IRB.CreateGEP(MapPtr, IRB.CreateXor(PrevLocCasted, CurLoc));

      /* Update bitmap */

      LoadInst *Counter = IRB.CreateLoad(MapPtrIdx);
      Counter->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));
      Value *Incr = IRB.CreateAdd(Counter, ConstantInt::get(Int8Ty, 1));
      IRB.CreateStore(Incr, MapPtrIdx)
          ->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

      /* Set prev_loc to cur_loc >> 1 */

      StoreInst *Store =
          IRB.CreateStore(ConstantInt::get(Int32Ty, cur_loc >> 1), AFLPrevLoc);
      Store->setMetadata(M.getMDKindID("nosanitize"), MDNode::get(C, None));

      inst_blocks++;

    }

  /* Say something nice. */

  if (!be_quiet) {

    if (!inst_blocks) WARNF("No instrumentation targets found.");
    else OKF("Instrumented %u locations (%s mode, ratio %u%%).",
             inst_blocks, getenv("AFL_HARDEN") ? "hardened" :
             ((getenv("AFL_USE_ASAN") || getenv("AFL_USE_MSAN")) ?
              "ASAN/MSAN" : "non-hardened"), inst_ratio);

  }

  return true;

}


static void registerAFLPass(const PassManagerBuilder &,
                            legacy::PassManagerBase &PM) {

  PM.add(new AFLCoverage());

}


static RegisterStandardPasses RegisterAFLPass(
    PassManagerBuilder::EP_OptimizerLast, registerAFLPass);

static RegisterStandardPasses RegisterAFLPass0(
    PassManagerBuilder::EP_EnabledOnOptLevel0, registerAFLPass);


static void registerEIPass(const PassManagerBuilder &,
                            legacy::PassManagerBase &PM) {
  PM.add(new ExecutionIndexing());
}


static RegisterStandardPasses RegisterEIPass(
    PassManagerBuilder::EP_OptimizerLast, registerEIPass);

char ExecutionIndexing::ID = 0;

bool ExecutionIndexing::runOnModule(Module& M) {
  LLVMContext &C = M.getContext();

  IntegerType *Int8Ty  = IntegerType::getInt8Ty(C);
  IntegerType *Int32Ty = IntegerType::getInt32Ty(C);
  Type *VoidTy = Type::getVoidTy(C);
  PointerType *CharPtrTy = PointerType::getUnqual(Int8Ty);
  Constant *NullCharPtr = ConstantPointerNull::get(CharPtrTy);
  
  Function* EIPushCallFunc = Function::Create(FunctionType::get(VoidTy,
      ArrayRef<Type*>({Int32Ty, CharPtrTy}), false), 
      GlobalVariable::ExternalLinkage, "__afl_ei_push_call", &M);
  
  Function* EIPopReturnFunc = Function::Create(FunctionType::get(VoidTy,
      false), GlobalVariable::ExternalLinkage,
      "__afl_ei_pop_return", &M);

  Function* FRead = M.getFunction("fread");
  Function* WrappedFRead = FRead == NULL ? NULL :
    Function::Create(FRead->getFunctionType(), GlobalValue::ExternalLinkage,
        "__afl_ei_fread", &M);
  
  for (auto &F : M) {
    for (auto &BB : F) {
      IRBuilder<> IRB(&BB);
      // Use a while-loop because we may erase from iterators
      auto it = BB.begin();
      while (it != BB.end()) {
        Instruction& Insn = *it;
        /* Instrument all call instructions */
        if (auto Call = dyn_cast<CallInst>(&Insn)) { 
          // This could have been done with InstVisitor#visitCallInst()
          Function* CalledFunc = Call->getCalledFunction();
          

          /* Insert call to push onto execution indexing stack */
          unsigned int call_site_id = AFL_R(MAP_SIZE);
          ConstantInt* CallSiteId = ConstantInt::get(Int32Ty, call_site_id);
          Value* CalledFuncName = (CalledFunc && CalledFunc->hasName()) ? 
            IRB.CreateGlobalStringPtr(CalledFunc->getName()) : NullCharPtr;
          IRB.SetInsertPoint(&BB, it);
          IRB.CreateCall(EIPushCallFunc, ArrayRef<Value*>({CallSiteId, CalledFuncName}));

          /* Replace calls to fread() with ei_fread() */
          if (FRead != NULL && FRead == Call->getCalledFunction()) {
            // First get all the args
            std::vector<Value*> args;
            for (auto &arg : Call->arg_operands()) {
              args.push_back(arg);
            }
            // Create a call to the wrapped fread()
            CallInst* WrappedCall = IRB.CreateCall(WrappedFRead, ArrayRef<Value*>(args));
            // Replace uses of the return value (nbytes) to the new return value
            Call->replaceAllUsesWith(WrappedCall);
            // Remove the original call to avoid duplicate calls to fread()
            it = Call->eraseFromParent();
          } else {
            it++; // Only increment if we do not delete the Call
          }
          
          /* Insert call to pop from execution indexing stack */
          IRB.SetInsertPoint(&BB, it); // `it` now points after the original call
          IRB.CreateCall(EIPopReturnFunc);
        } else {
          it++; // For non-call instructions, just keep moving
        }
      }
    }
  }

  return true;

}
