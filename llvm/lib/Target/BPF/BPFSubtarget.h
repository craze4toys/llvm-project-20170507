//===-- BPFSubtarget.h - Define Subtarget for the BPF -----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declares the BPF specific subclass of TargetSubtargetInfo.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_BPF_BPFSUBTARGET_H
#define LLVM_LIB_TARGET_BPF_BPFSUBTARGET_H

#include "BPFFrameLowering.h"
#include "BPFISelLowering.h"
#include "BPFInstrInfo.h"
#include "llvm/Target/TargetSelectionDAGInfo.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetSubtargetInfo.h"

#define GET_SUBTARGETINFO_HEADER
#include "BPFGenSubtargetInfo.inc"

namespace llvm {
class StringRef;

class BPFSubtarget : public BPFGenSubtargetInfo {
  virtual void anchor();
  BPFInstrInfo InstrInfo;
  BPFFrameLowering FrameLowering;
  BPFTargetLowering TLInfo;
  TargetSelectionDAGInfo TSInfo;

public:
  // This constructor initializes the data members to match that
  // of the specified triple.
  BPFSubtarget(const Triple &TT, const std::string &CPU, const std::string &FS,
               const TargetMachine &TM);

  // ParseSubtargetFeatures - Parses features string setting specified
  // subtarget options.  Definition of function is auto generated by tblgen.
  void ParseSubtargetFeatures(StringRef CPU, StringRef FS);

  const BPFInstrInfo *getInstrInfo() const override { return &InstrInfo; }
  const BPFFrameLowering *getFrameLowering() const override {
    return &FrameLowering;
  }
  const BPFTargetLowering *getTargetLowering() const override {
    return &TLInfo;
  }
  const TargetSelectionDAGInfo *getSelectionDAGInfo() const override {
    return &TSInfo;
  }
  const TargetRegisterInfo *getRegisterInfo() const override {
    return &InstrInfo.getRegisterInfo();
  }
};
} // namespace llvm

#endif
