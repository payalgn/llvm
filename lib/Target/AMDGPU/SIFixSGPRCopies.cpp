//===-- SIFixSGPRCopies.cpp - Remove potential VGPR => SGPR copies --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Copies from VGPR to SGPR registers are illegal and the register coalescer
/// will sometimes generate these illegal copies in situations like this:
///
///  Register Class <vsrc> is the union of <vgpr> and <sgpr>
///
/// BB0:
///   %vreg0 <sgpr> = SCALAR_INST
///   %vreg1 <vsrc> = COPY %vreg0 <sgpr>
///    ...
///    BRANCH %cond BB1, BB2
///  BB1:
///    %vreg2 <vgpr> = VECTOR_INST
///    %vreg3 <vsrc> = COPY %vreg2 <vgpr>
///  BB2:
///    %vreg4 <vsrc> = PHI %vreg1 <vsrc>, <BB#0>, %vreg3 <vrsc>, <BB#1>
///    %vreg5 <vgpr> = VECTOR_INST %vreg4 <vsrc>
///
///
/// The coalescer will begin at BB0 and eliminate its copy, then the resulting
/// code will look like this:
///
/// BB0:
///   %vreg0 <sgpr> = SCALAR_INST
///    ...
///    BRANCH %cond BB1, BB2
/// BB1:
///   %vreg2 <vgpr> = VECTOR_INST
///   %vreg3 <vsrc> = COPY %vreg2 <vgpr>
/// BB2:
///   %vreg4 <sgpr> = PHI %vreg0 <sgpr>, <BB#0>, %vreg3 <vsrc>, <BB#1>
///   %vreg5 <vgpr> = VECTOR_INST %vreg4 <sgpr>
///
/// Now that the result of the PHI instruction is an SGPR, the register
/// allocator is now forced to constrain the register class of %vreg3 to
/// <sgpr> so we end up with final code like this:
///
/// BB0:
///   %vreg0 <sgpr> = SCALAR_INST
///    ...
///    BRANCH %cond BB1, BB2
/// BB1:
///   %vreg2 <vgpr> = VECTOR_INST
///   %vreg3 <sgpr> = COPY %vreg2 <vgpr>
/// BB2:
///   %vreg4 <sgpr> = PHI %vreg0 <sgpr>, <BB#0>, %vreg3 <sgpr>, <BB#1>
///   %vreg5 <vgpr> = VECTOR_INST %vreg4 <sgpr>
///
/// Now this code contains an illegal copy from a VGPR to an SGPR.
///
/// In order to avoid this problem, this pass searches for PHI instructions
/// which define a <vsrc> register and constrains its definition class to
/// <vgpr> if the user of the PHI's definition register is a vector instruction.
/// If the PHI's definition class is constrained to <vgpr> then the coalescer
/// will be unable to perform the COPY removal from the above example  which
/// ultimately led to the creation of an illegal COPY.
//===----------------------------------------------------------------------===//

#include "AMDGPU.h"
#include "AMDGPUSubtarget.h"
#include "SIInstrInfo.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

using namespace llvm;

#define DEBUG_TYPE "si-fix-sgpr-copies"

namespace {

class SIFixSGPRCopies : public MachineFunctionPass {

  MachineDominatorTree *MDT;

public:
  static char ID;

  SIFixSGPRCopies() : MachineFunctionPass(ID) { }

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return "SI Fix SGPR copies"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineDominatorTree>();
    AU.addPreserved<MachineDominatorTree>();
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // End anonymous namespace

INITIALIZE_PASS_BEGIN(SIFixSGPRCopies, DEBUG_TYPE,
                     "SI Fix SGPR copies", false, false)
INITIALIZE_PASS_DEPENDENCY(MachinePostDominatorTree)
INITIALIZE_PASS_END(SIFixSGPRCopies, DEBUG_TYPE,
                     "SI Fix SGPR copies", false, false)


char SIFixSGPRCopies::ID = 0;

char &llvm::SIFixSGPRCopiesID = SIFixSGPRCopies::ID;

FunctionPass *llvm::createSIFixSGPRCopiesPass() {
  return new SIFixSGPRCopies();
}

static bool hasVGPROperands(const MachineInstr &MI, const SIRegisterInfo *TRI) {
  const MachineRegisterInfo &MRI = MI.getParent()->getParent()->getRegInfo();
  for (unsigned i = 0, e = MI.getNumOperands(); i != e; ++i) {
    if (!MI.getOperand(i).isReg() ||
        !TargetRegisterInfo::isVirtualRegister(MI.getOperand(i).getReg()))
      continue;

    if (TRI->hasVGPRs(MRI.getRegClass(MI.getOperand(i).getReg())))
      return true;
  }
  return false;
}

static std::pair<const TargetRegisterClass *, const TargetRegisterClass *>
getCopyRegClasses(const MachineInstr &Copy,
                  const SIRegisterInfo &TRI,
                  const MachineRegisterInfo &MRI) {
  unsigned DstReg = Copy.getOperand(0).getReg();
  unsigned SrcReg = Copy.getOperand(1).getReg();

  const TargetRegisterClass *SrcRC =
    TargetRegisterInfo::isVirtualRegister(SrcReg) ?
    MRI.getRegClass(SrcReg) :
    TRI.getPhysRegClass(SrcReg);

  // We don't really care about the subregister here.
  // SrcRC = TRI.getSubRegClass(SrcRC, Copy.getOperand(1).getSubReg());

  const TargetRegisterClass *DstRC =
    TargetRegisterInfo::isVirtualRegister(DstReg) ?
    MRI.getRegClass(DstReg) :
    TRI.getPhysRegClass(DstReg);

  return std::make_pair(SrcRC, DstRC);
}

static bool isVGPRToSGPRCopy(const TargetRegisterClass *SrcRC,
                             const TargetRegisterClass *DstRC,
                             const SIRegisterInfo &TRI) {
  return TRI.isSGPRClass(DstRC) && TRI.hasVGPRs(SrcRC);
}

static bool isSGPRToVGPRCopy(const TargetRegisterClass *SrcRC,
                             const TargetRegisterClass *DstRC,
                             const SIRegisterInfo &TRI) {
  return TRI.isSGPRClass(SrcRC) && TRI.hasVGPRs(DstRC);
}

// Distribute an SGPR->VGPR copy of a REG_SEQUENCE into a VGPR REG_SEQUENCE.
//
// SGPRx = ...
// SGPRy = REG_SEQUENCE SGPRx, sub0 ...
// VGPRz = COPY SGPRy
//
// ==>
//
// VGPRx = COPY SGPRx
// VGPRz = REG_SEQUENCE VGPRx, sub0
//
// This exposes immediate folding opportunities when materializing 64-bit
// immediates.
static bool foldVGPRCopyIntoRegSequence(MachineInstr &MI,
                                        const SIRegisterInfo *TRI,
                                        const SIInstrInfo *TII,
                                        MachineRegisterInfo &MRI) {
  assert(MI.isRegSequence());

  unsigned DstReg = MI.getOperand(0).getReg();
  if (!TRI->isSGPRClass(MRI.getRegClass(DstReg)))
    return false;

  if (!MRI.hasOneUse(DstReg))
    return false;

  MachineInstr &CopyUse = *MRI.use_instr_begin(DstReg);
  if (!CopyUse.isCopy())
    return false;

  const TargetRegisterClass *SrcRC, *DstRC;
  std::tie(SrcRC, DstRC) = getCopyRegClasses(CopyUse, *TRI, MRI);

  if (!isSGPRToVGPRCopy(SrcRC, DstRC, *TRI))
    return false;

  // TODO: Could have multiple extracts?
  unsigned SubReg = CopyUse.getOperand(1).getSubReg();
  if (SubReg != AMDGPU::NoSubRegister)
    return false;

  MRI.setRegClass(DstReg, DstRC);

  // SGPRx = ...
  // SGPRy = REG_SEQUENCE SGPRx, sub0 ...
  // VGPRz = COPY SGPRy

  // =>
  // VGPRx = COPY SGPRx
  // VGPRz = REG_SEQUENCE VGPRx, sub0

  MI.getOperand(0).setReg(CopyUse.getOperand(0).getReg());

  for (unsigned I = 1, N = MI.getNumOperands(); I != N; I += 2) {
    unsigned SrcReg = MI.getOperand(I).getReg();
    unsigned SrcSubReg = MI.getOperand(I).getSubReg();

    const TargetRegisterClass *SrcRC = MRI.getRegClass(SrcReg);
    assert(TRI->isSGPRClass(SrcRC) &&
           "Expected SGPR REG_SEQUENCE to only have SGPR inputs");

    SrcRC = TRI->getSubRegClass(SrcRC, SrcSubReg);
    const TargetRegisterClass *NewSrcRC = TRI->getEquivalentVGPRClass(SrcRC);

    unsigned TmpReg = MRI.createVirtualRegister(NewSrcRC);

    BuildMI(*MI.getParent(), &MI, MI.getDebugLoc(), TII->get(AMDGPU::COPY), TmpReg)
      .addOperand(MI.getOperand(I));

    MI.getOperand(I).setReg(TmpReg);
  }

  CopyUse.eraseFromParent();
  return true;
}

static bool phiHasVGPROperands(const MachineInstr &PHI,
                               const MachineRegisterInfo &MRI,
                               const SIRegisterInfo *TRI,
                               const SIInstrInfo *TII) {

  for (unsigned i = 1; i < PHI.getNumOperands(); i += 2) {
    unsigned Reg = PHI.getOperand(i).getReg();
    if (TRI->hasVGPRs(MRI.getRegClass(Reg)))
      return true;
  }
  return false;
}
static bool phiHasBreakDef(const MachineInstr &PHI,
                           const MachineRegisterInfo &MRI,
                           SmallSet<unsigned, 8> &Visited) {

  for (unsigned i = 1; i < PHI.getNumOperands(); i += 2) {
    unsigned Reg = PHI.getOperand(i).getReg();
    if (Visited.count(Reg))
      continue;

    Visited.insert(Reg);

    MachineInstr *DefInstr = MRI.getUniqueVRegDef(Reg);
    assert(DefInstr);
    switch (DefInstr->getOpcode()) {
    default:
      break;
    case AMDGPU::SI_BREAK:
    case AMDGPU::SI_IF_BREAK:
    case AMDGPU::SI_ELSE_BREAK:
      return true;
    case AMDGPU::PHI:
      if (phiHasBreakDef(*DefInstr, MRI, Visited))
        return true;
    }
  }
  return false;
}

static bool hasTerminatorThatModifiesExec(const MachineBasicBlock &MBB,
                                          const TargetRegisterInfo &TRI) {
  for (MachineBasicBlock::const_iterator I = MBB.getFirstTerminator(),
       E = MBB.end(); I != E; ++I) {
    if (I->modifiesRegister(AMDGPU::EXEC, &TRI))
      return true;
  }
  return false;
}

bool SIFixSGPRCopies::runOnMachineFunction(MachineFunction &MF) {
  const SISubtarget &ST = MF.getSubtarget<SISubtarget>();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const SIRegisterInfo *TRI = ST.getRegisterInfo();
  const SIInstrInfo *TII = ST.getInstrInfo();
  MDT = &getAnalysis<MachineDominatorTree>();

  SmallVector<MachineInstr *, 16> Worklist;

  for (MachineFunction::iterator BI = MF.begin(), BE = MF.end();
                                                  BI != BE; ++BI) {

    MachineBasicBlock &MBB = *BI;
    for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end();
         I != E; ++I) {
      MachineInstr &MI = *I;

      switch (MI.getOpcode()) {
      default:
        continue;
      case AMDGPU::COPY: {
        // If the destination register is a physical register there isn't really
        // much we can do to fix this.
        if (!TargetRegisterInfo::isVirtualRegister(MI.getOperand(0).getReg()))
          continue;

        const TargetRegisterClass *SrcRC, *DstRC;
        std::tie(SrcRC, DstRC) = getCopyRegClasses(MI, *TRI, MRI);
        if (isVGPRToSGPRCopy(SrcRC, DstRC, *TRI)) {
          DEBUG(dbgs() << "Fixing VGPR -> SGPR copy: " << MI);
          TII->moveToVALU(MI);
        }

        break;
      }
      case AMDGPU::PHI: {
        unsigned Reg = MI.getOperand(0).getReg();
        if (!TRI->isSGPRClass(MRI.getRegClass(Reg)))
          break;

        // We don't need to fix the PHI if the common dominator of the
        // two incoming blocks terminates with a uniform branch.
        if (MI.getNumExplicitOperands() == 5) {
          MachineBasicBlock *MBB0 = MI.getOperand(2).getMBB();
          MachineBasicBlock *MBB1 = MI.getOperand(4).getMBB();

          MachineBasicBlock *NCD = MDT->findNearestCommonDominator(MBB0, MBB1);
          if (NCD && !hasTerminatorThatModifiesExec(*NCD, *TRI)) {
            DEBUG(dbgs() << "Not fixing PHI for uniform branch: " << MI << '\n');
            break;
          }
        }

        // If a PHI node defines an SGPR and any of its operands are VGPRs,
        // then we need to move it to the VALU.
        //
        // Also, if a PHI node defines an SGPR and has all SGPR operands
        // we must move it to the VALU, because the SGPR operands will
        // all end up being assigned the same register, which means
        // there is a potential for a conflict if different threads take
        // different control flow paths.
        //
        // For Example:
        //
        // sgpr0 = def;
        // ...
        // sgpr1 = def;
        // ...
        // sgpr2 = PHI sgpr0, sgpr1
        // use sgpr2;
        //
        // Will Become:
        //
        // sgpr2 = def;
        // ...
        // sgpr2 = def;
        // ...
        // use sgpr2
        //
        // The one exception to this rule is when one of the operands
        // is defined by a SI_BREAK, SI_IF_BREAK, or SI_ELSE_BREAK
        // instruction.  In this case, there we know the program will
        // never enter the second block (the loop) without entering
        // the first block (where the condition is computed), so there
        // is no chance for values to be over-written.

        SmallSet<unsigned, 8> Visited;
        if (phiHasVGPROperands(MI, MRI, TRI, TII) ||
            !phiHasBreakDef(MI, MRI, Visited)) {
          DEBUG(dbgs() << "Fixing PHI: " << MI);
          TII->moveToVALU(MI);
        }
        break;
      }
      case AMDGPU::REG_SEQUENCE: {
        if (TRI->hasVGPRs(TII->getOpRegClass(MI, 0)) ||
            !hasVGPROperands(MI, TRI)) {
          foldVGPRCopyIntoRegSequence(MI, TRI, TII, MRI);
          continue;
        }

        DEBUG(dbgs() << "Fixing REG_SEQUENCE: " << MI);

        TII->moveToVALU(MI);
        break;
      }
      case AMDGPU::INSERT_SUBREG: {
        const TargetRegisterClass *DstRC, *Src0RC, *Src1RC;
        DstRC = MRI.getRegClass(MI.getOperand(0).getReg());
        Src0RC = MRI.getRegClass(MI.getOperand(1).getReg());
        Src1RC = MRI.getRegClass(MI.getOperand(2).getReg());
        if (TRI->isSGPRClass(DstRC) &&
            (TRI->hasVGPRs(Src0RC) || TRI->hasVGPRs(Src1RC))) {
          DEBUG(dbgs() << " Fixing INSERT_SUBREG: " << MI);
          TII->moveToVALU(MI);
        }
        break;
      }
      }
    }
  }

  return true;
}
