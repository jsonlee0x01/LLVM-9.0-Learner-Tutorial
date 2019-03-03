#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/raw_ostream.h"
#include "HI_print.h"
#include "HI_VarWidthReduce.h"
#include <stdio.h>
#include <string>
#include <ios>
#include <stdlib.h>

using namespace llvm;



/*
    1.Analysis: check the value range of the instructions in the source code and determine the bitwidth
    2.Forward Process: check the bitwidth of operands and output of an instruction, trunc/ext the operands, update the bitwidth of the instruction
    3.Check Redundancy: Some instructions could be truncated to be an operand, but itself is actually updated with the same bitwidth with the truncation.
    4.Validation Check: Check whether there is any binary operation with operands in different types.
*/
bool HI_VarWidthReduce::runOnFunction(Function &F) // The runOnModule declaration will overide the virtual one in ModulePass, which will be executed for each Module.
{
    const DataLayout &DL = F.getParent()->getDataLayout();
    SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    
    if (Function_id.find(&F)==Function_id.end())  // traverse functions and assign function ID
    {
        Function_id[&F] = ++Function_Counter;
    }
    bool changed = 0;

    // 1.Analysis: check the value range of the instructions in the source code and determine the bitwidth
    Bitwidth_Analysis(&F);

    // 2.Forward Process: check the bitwidth of operands and output of an instruction, trunc/ext the operands, update the bitwidth of the instruction
    changed |= InsturctionUpdate_WidthCast(&F);

    // 3.Check Redundancy: Some instructions could be truncated to be an operand, but itself is actually updated with the same bitwidth with the truncation.
    changed |= RedundantCastRemove(&F);

    // 4.Validation Check: Check whether there is any binary operation with operands in different types.
    VarWidthReduce_Validation(&F);


    if (changed)    
        *VarWidthChangeLog << "THE IR CODE IS CHANGED\n";    
    else    
        *VarWidthChangeLog << "THE IR CODE IS NOT CHANGED\n";
    
    return changed;
}


char HI_VarWidthReduce::ID = 0;  // the ID for pass should be initialized but the value does not matter, since LLVM uses the address of this variable as label instead of its value.

void HI_VarWidthReduce::getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addRequired<TargetTransformInfoWrapperPass>();
    AU.addRequired<LoopInfoWrapperPass>();
    // AU.addRequired<LazyValueInfoWrapperPass>();    
    // AU.setPreservesCFG();
}


// Analysis: check the value range of the instructions in the source code and determine the bitwidth
void HI_VarWidthReduce::Bitwidth_Analysis(Function *F) 
{
    const DataLayout &DL = F->getParent()->getDataLayout();
    SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    for (BasicBlock &B : *F) 
    {
        for (Instruction &I: B) 
        {
            if (I.getType()->isIntegerTy())
            {
                Instruction_id[&I] = ++Instruction_Counter;
                KnownBits tmp_KB = computeKnownBits(&I,DL); 
                const SCEV *tmp_S = SE->getSCEV(&I);

                // LLVM-Provided Value Range Evaluation (may be wrong with HLS because it takes array entries as memory address,
                // but in HLS, array entries are just memory ports. Operations, e.g. addition, with memory port are just to get the right address)
                ConstantRange tmp_CR1 = SE->getSignedRange(tmp_S); 

                // HI Value Range Evaluation, take the array entries as ZERO offsets and will not effect the result of the value range of address   
                ConstantRange tmp_CR2 = HI_getSignedRangeRef(tmp_S);

                *VarWidthChangeLog << I << "---- Ori-CR: "<<tmp_CR1 << "(bw=" << I.getType()->getIntegerBitWidth() <<") ---- HI-CR:"<<tmp_CR2 << "(bw=" ;

                if (I.mayReadFromMemory()) // if the instruction is actually a load instruction, the bitwidth should be the bitwidth of memory bitwidth
                {
                    Instruction_BitNeeded[&I] = I.getType()->getIntegerBitWidth();
                    *VarWidthChangeLog << "        ----  this could be a load inst.\n";
                }
                else  // otherwise, extract the bitwidth from the value range
                {
                    Instruction_BitNeeded[&I] = bitNeededFor(tmp_CR2) ;
                }                
                 *VarWidthChangeLog << Instruction_BitNeeded[&I] <<")\n";
                *VarWidthChangeLog << "\n\n\n";
            }
        }    
        *VarWidthChangeLog << "\n";
    }
}


// Forward Process: check the bitwidth of operands and output of an instruction, trunc/ext the operands, update the bitwidth of the instruction
bool HI_VarWidthReduce::InsturctionUpdate_WidthCast(Function *F) 
{
    const DataLayout &DL = F->getParent()->getDataLayout();
    SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
    bool changed = 0;
    for (BasicBlock &B : *F) 
    {
        bool take_action = 1;
        while(take_action)
        {
            take_action = 0;
            for (Instruction &I: B)  // TODO: try to improve the way to handle instructions if you want to remove some of them
            {
                if (Instruction_id.find(&I) != Instruction_id.end())
                {
                    Instruction_id.erase(&I);
                    *VarWidthChangeLog <<"\n\n\n find target instrction " <<*I.getType() <<":" << I ;
                    //VarWidthChangeLog->flush(); 

                    // bypass cast operations
                    if (CastInst *CastI = dyn_cast<CastInst>(&I))
                        continue;

                    
                    if (I.getType()->isIntegerTy())
                    {
                        changed = 1;
                        *VarWidthChangeLog <<"\n" <<*I.getType() <<":" << I ;
                        *VarWidthChangeLog << "------- under processing (targetBW="<<Instruction_BitNeeded[&I]<<", curBW="<< (cast<IntegerType>(I.getType()))->getBitWidth()<<") ";   
                        //VarWidthChangeLog->flush(); 

                        // bypass load instructions
                        if (I.mayReadFromMemory())
                        {
                            *VarWidthChangeLog << "                         ------->  this could be a load inst (bypass).\n";
                            continue;
                        }
                        
                        // for comp instruction, we just need to ensure that, the two operands are in the same type
                        if (ICmpInst* ICMP_I = dyn_cast<ICmpInst>(&I))
                        {                            
                            if (cast<IntegerType>(ICMP_I->getOperand(0)->getType())->getIntegerBitWidth() == cast<IntegerType>(ICMP_I->getOperand(1)->getType())->getIntegerBitWidth())
                            {
                                *VarWidthChangeLog << "\n                         -------> Inst: " << I << "  ---needs no update req="<< Instruction_BitNeeded[&I] << " user width=" <<(cast<IntegerType>(I.getType()))->getBitWidth() << " \n";  
                                continue;
                            }
                        }

                        // check whether all the elements (input+output) in the same type.
                        if (Instruction_BitNeeded[&I] == (cast<IntegerType>(I.getType()))->getBitWidth())
                        {
                            bool neq = 0;
                            for (int i = 0; i < I.getNumOperands(); ++i)
                            {
                                neq |= Instruction_BitNeeded[&I] != (cast<IntegerType>(I.getOperand(i)->getType()))->getBitWidth();
                            }
                            if (!neq)
                            {
                                *VarWidthChangeLog << "\n                         -------> Inst: " << I << "  ---needs no update req="<< Instruction_BitNeeded[&I] << " user width=" <<(cast<IntegerType>(I.getType()))->getBitWidth() << " \n";  
                                continue;
                            }
                        }                      
                        


                        //VarWidthChangeLog->flush(); 
                        
                        // process different operations with corresponding consideration/procedure
                        if (BinaryOperator* BOI = dyn_cast<BinaryOperator>(&I))
                        {
                            BOI_WidthCast(BOI);
                            take_action = 1;
                            changed = 1;    
                            break;
                        }
                        else if (ICmpInst* ICMP_I = dyn_cast<ICmpInst>(&I))
                        {
                            ICMP_WidthCast(ICMP_I);
                            take_action = 1;
                            changed = 1;    
                            break;
                        }
                        else if (PHINode* PHI_I = dyn_cast<PHINode>(&I))
                        {
                            PHI_WidthCast(PHI_I);
                            take_action = 1;
                            changed = 1;    
                            break;
                        }
                        else
                        {
                            *VarWidthChangeLog << "and it is not a binary operator.(bypass)\n";
                        }                        
                    }
                }
                else
                {
                    *VarWidthChangeLog <<"\n\n\n find non-target instrction " <<*I.getType() <<":" << I ;
                    //VarWidthChangeLog->flush(); 
                }
            }
        }    
        *VarWidthChangeLog << "\n";
        //VarWidthChangeLog->flush(); 
    }
    return changed;
}


// Check Redundancy: Some instructions could be truncated to be an operand, but itself is actually updated with the same bitwidth with the truncation.
bool HI_VarWidthReduce::RedundantCastRemove(Function *F)
{
    bool changed = 0;
    *VarWidthChangeLog << "==============================================\n==============================================\n\n\n\n\n\n";
    for (BasicBlock &B : *F) 
    {
        bool rmflag = 1;
        while (rmflag)
        {
            rmflag = 0;
            for (Instruction &I: B) 
            {
                *VarWidthChangeLog << "                         ------->checking redunctan CastI: " << I  <<"\n";
                if (CastInst * CastI = dyn_cast<CastInst>(&I))         
                {
                    if (CastI->getOpcode()!=Instruction::Trunc && CastI->getOpcode()!=Instruction::ZExt && CastI->getOpcode()!=Instruction::SExt)
                    {
                        // Cast Instrctions are more than TRUNC/EXT
                        continue;
                    }
                    // If bitwidth(A)==bitwidth(B) in TRUNT/EXT A to B, then it is not necessary to do the instruction
                    if (CastI->getType()->getIntegerBitWidth() == I.getOperand(0)->getType()->getIntegerBitWidth())
                    {
                        *VarWidthChangeLog << "                         ------->remove redunctan CastI: " << *CastI  <<"\n";
                        *VarWidthChangeLog << "                         ------->replace CastI with its operand 0: " << *I.getOperand(0)  <<"\n";
                        //VarWidthChangeLog->flush(); 
                        ReplaceUsesUnsafe(&I,I.getOperand(0));
                        I.eraseFromParent();
                        rmflag = 1;
                        changed = 1;
                        break;
                    }
                }            
            }   
        } 
    }
    return changed;
}


// Validation Check: Check whether there is any binary operation with operands in different types.
void HI_VarWidthReduce::VarWidthReduce_Validation(Function *F)
{
    // In some other passes in LLVM, if a insturction has operands in different types, errors could be generated
    for (BasicBlock &B : *F) 
    {
        bool take_action = 1;
        while(take_action)
        {
            take_action = 0;
            for (Instruction &I: B) 
            {
                *VarWidthChangeLog << "checking Instruction width: " << I << " ";
                if (I.getType()->isIntegerTy())
                {
                    const SCEV *tmp_S = SE->getSCEV(&I);
                    ConstantRange tmp_CR1 = SE->getSignedRange(tmp_S);
                    *VarWidthChangeLog << "CR-bw=" << tmp_CR1.getBitWidth() << " type-bw="<<I.getType()->getIntegerBitWidth() <<"\n";
                    if (tmp_CR1.getBitWidth() != I.getType()->getIntegerBitWidth())
                        *VarWidthChangeLog << "Bit width error!!!\n";
                }
                else
                {
                    *VarWidthChangeLog << "is not an integer type.\n ";
                }
            }
        }    
        *VarWidthChangeLog << "\n";
        //VarWidthChangeLog->flush(); 
    }

    *VarWidthChangeLog << "==============================================\n==============================================\n\n\n\n\n\n";
    for (BasicBlock &B : *F) 
    {
        *VarWidthChangeLog << B.getName() <<"\n";
        for (Instruction &I: B) 
        {
            *VarWidthChangeLog << "   " << I<<"\n";  
        }    
        *VarWidthChangeLog << "-------------------\n";
    }

    //VarWidthChangeLog->flush(); 
    return;
}


// The replaceAllUsesWith function requires that the new use in the user has the same type with the original use.
// Therefore, a new function to replace uses of an instruction is implemented
void HI_VarWidthReduce::ReplaceUsesUnsafe(Instruction *from, Value *to) 
{
    *VarWidthChangeLog << "            ------  replacing  " << *from << " in its user\n";
    while (!from->use_empty()) 
    {
        User* tmp_user = from->use_begin()->getUser();
        *VarWidthChangeLog << "            ------  replacing the original inst in " << *from->use_begin()->getUser() << " with " << *to <<"\n";
        from->use_begin()->set(to);
        *VarWidthChangeLog << "            ------  new user => " << *tmp_user << "\n";
        *VarWidthChangeLog << "            ------  from->getNumUses() "<< from->getNumUses() << "\n";
    }
}


// compute the bitwidth needed for the specific constant range
unsigned int HI_VarWidthReduce::bitNeededFor(ConstantRange CR)
{
    if (CR.isFullSet())
        return CR.getBitWidth();
    if (CR.getLower().isNonNegative())
    {
        // do no consider the leading zero, if the range is non-negative
        unsigned int lowerNeedBits = CR.getLower().getActiveBits();
        unsigned int upperNeedBits = CR.getUpper().getActiveBits();

        if (lowerNeedBits > upperNeedBits) 
            return lowerNeedBits;
        else
            return upperNeedBits;
    }
    else
    {
        // consider the leading zero/ones, if the range is negative
        unsigned int lowerNeedBits = CR.getLower().getMinSignedBits();
        unsigned int upperNeedBits = CR.getUpper().getMinSignedBits();

        if (lowerNeedBits > upperNeedBits) 
            return lowerNeedBits;
        else
            return upperNeedBits;
    }   
}


// Forward Process of BinaryOperator: check the bitwidth of operands and output of an instruction, trunc/ext the operands, update the bitwidth of the instruction
void HI_VarWidthReduce::BOI_WidthCast(BinaryOperator *BOI)
{
    Instruction &I = *(cast<Instruction>(BOI));
    const SCEV *tmp_S = SE->getSCEV(&I);
    ConstantRange tmp_CR = HI_getSignedRangeRef(tmp_S);
    Value *ResultPtr = &I;

    *VarWidthChangeLog << "and its operands are:\n";
    //VarWidthChangeLog->flush(); 

    // check whether an instruction involve PTI operation
    for (int i = 0; i < I.getNumOperands(); ++i)
    {
        if (PtrToIntInst *PTI_I = dyn_cast<PtrToIntInst>(I.getOperand(i))) 
        {
            // if this instruction involve operands from pointer, 
            // we meed to ensure the operands have the same width of the pointer
            Instruction_BitNeeded[&I] = (cast<IntegerType>(I.getType()))->getBitWidth();
        }
    }

    for (int i = 0; i < I.getNumOperands(); ++i) // check the operands to see whether a TRUNC/EXT is necessary
    {
        *VarWidthChangeLog << "                         ------->  op#"<<i <<"==>"<<*I.getOperand(i)<<"\n";
        //VarWidthChangeLog->flush(); 
        if (ConstantInt *C_I = dyn_cast<ConstantInt>(I.getOperand(i)))                                
        {
            *VarWidthChangeLog << "                         ------->  op#"<<i<<" "<<*C_I<<" is a constant.\n";
            //VarWidthChangeLog->flush(); 
            Type *NewTy_C = IntegerType::get(I.getType()->getContext(), Instruction_BitNeeded[&I]);
            Constant *New_C = ConstantInt::get(NewTy_C,*C_I->getValue().getRawData());
            *VarWidthChangeLog << "                         ------->  update"<<I<<" to ";
            //VarWidthChangeLog->flush(); 
            I.setOperand(i,New_C);
            *VarWidthChangeLog <<I<<"\n";
        }
        else
        {
            if (Instruction *Op_I = dyn_cast<Instruction>(I.getOperand(i)))
            {
                *VarWidthChangeLog << "                         ------->  op#"<<i<<" "<<*Op_I<<" is an instruction\n";
                //VarWidthChangeLog->flush(); 
                IRBuilder<> Builder( Op_I->getNextNode());
                std::string regNameS = "bcast"+std::to_string(changed_id);
                changed_id++; 
                // create a net type with specific bitwidth                  
                Type *NewTy_OP = IntegerType::get(I.getType()->getContext(), Instruction_BitNeeded[&I]);
                if (tmp_CR.getLower().isNegative())
                {                                                
                    ResultPtr = Builder.CreateSExtOrTrunc(Op_I, NewTy_OP,regNameS.c_str()); // process the operand with SExtOrTrunc if it is signed.
                }
                else
                {
                    ResultPtr = Builder.CreateZExtOrTrunc(Op_I, NewTy_OP,regNameS.c_str()); // process the operand with ZExtOrTrunc if it is unsigned.                                           
                }
                *VarWidthChangeLog << "                         ------->  update"<<I<<" to ";
                //VarWidthChangeLog->flush(); 
                I.setOperand(i,ResultPtr);
                *VarWidthChangeLog <<I<<"\n";
            }

        }
        //VarWidthChangeLog->flush();                                 
    }
    //VarWidthChangeLog->flush(); 
    *VarWidthChangeLog << "                         ------->  op0 type = "<<*BOI->getOperand(0)->getType()<<"\n";
    *VarWidthChangeLog << "                         ------->  op1 type = "<<*BOI->getOperand(1)->getType()<<"\n";
    
    // re-create the instruction to update the type(bitwidth) of it, otherwise, although the operans are changed, the output of instrcution will be remained.
    std::string regNameS = "new"+std::to_string(changed_id);
    BinaryOperator *newBOI = BinaryOperator::Create(BOI->getOpcode(), BOI->getOperand(0), BOI->getOperand(1), "HI."+BOI->getName()+regNameS,BOI); 
    *VarWidthChangeLog << "                         ------->  new_BOI = "<<*newBOI<<"\n";
    // BOI->replaceAllUsesWith(newBOI) ;
    ReplaceUsesUnsafe(BOI, newBOI) ;
    *VarWidthChangeLog << "                         ------->  accomplish replacement of original instruction in uses.\n";
    //VarWidthChangeLog->flush(); 
    I.eraseFromParent();
    *VarWidthChangeLog << "                         ------->  accomplish erasing of original instruction.\n";
    //VarWidthChangeLog->flush(); 
}



// Forward Process of ICmpInst: check the bitwidth of operands and output of an instruction, trunc/ext the operands, update the bitwidth of the instruction
void HI_VarWidthReduce::ICMP_WidthCast(ICmpInst *ICMP_I)
{
    Instruction &I = *(cast<Instruction>(ICMP_I));
    const SCEV *tmp_S = SE->getSCEV(&I);
    ConstantRange tmp_CR = HI_getSignedRangeRef(tmp_S);
    Value *ResultPtr = &I;

    *VarWidthChangeLog << "and its operands are:\n";
    //VarWidthChangeLog->flush(); 

    // check whether an instruction involve PTI operation
    for (int i = 0; i < I.getNumOperands(); ++i)
    {
        if (PtrToIntInst *PTI_I = dyn_cast<PtrToIntInst>(I.getOperand(i))) 
        {
            // if this instruction involve operands from pointer, 
            // we meed to ensure the operands have the same width of the pointer
            Instruction_BitNeeded[&I] = (cast<IntegerType>(I.getType()))->getBitWidth();
        }
    }

    for (int i = 0; i < I.getNumOperands(); ++i) // check the operands to see whether a TRUNC/EXT is necessary
    {
        *VarWidthChangeLog << "                         ------->  op#"<<i <<"==>"<<*I.getOperand(i)<<"\n";
        //VarWidthChangeLog->flush(); 
        if (ConstantInt *C_I = dyn_cast<ConstantInt>(I.getOperand(i)))                                
        {
            *VarWidthChangeLog << "                         ------->  op#"<<i<<" "<<*C_I<<" is a constant.\n";
            //VarWidthChangeLog->flush(); 
            //  if (C_I->getBitWidth()!= Instruction_BitNeeded[User_I])
            Type *NewTy_C = IntegerType::get(I.getType()->getContext(), Instruction_BitNeeded[&I]);
            Constant *New_C = ConstantInt::get(NewTy_C,*C_I->getValue().getRawData());
            *VarWidthChangeLog << "                         ------->  update"<<I<<" to ";
            //VarWidthChangeLog->flush(); 
            I.setOperand(i,New_C);
            *VarWidthChangeLog <<I<<"\n";
        }
        else
        {
            if (Instruction *Op_I = dyn_cast<Instruction>(I.getOperand(i)))
            {
                *VarWidthChangeLog << "                         ------->  op#"<<i<<" "<<*Op_I<<" is an instruction\n";
                //VarWidthChangeLog->flush(); 
                IRBuilder<> Builder( Op_I->getNextNode());
                std::string regNameS = "bcast"+std::to_string(changed_id);
                changed_id++;                   

                // create a net type with specific bitwidth
                Type *NewTy_OP = IntegerType::get(I.getType()->getContext(), Instruction_BitNeeded[&I]);
                if (tmp_CR.getLower().isNegative())
                {                                                
                    ResultPtr = Builder.CreateSExtOrTrunc(Op_I, NewTy_OP,regNameS.c_str());// process the operand with SExtOrTrunc if it is signed.
                }
                else
                {
                    ResultPtr = Builder.CreateZExtOrTrunc(Op_I, NewTy_OP,regNameS.c_str());// process the operand with ZExtOrTrunc if it is unsigned.                                            
                }
                *VarWidthChangeLog << "                         ------->  update"<<I<<" to ";
                //VarWidthChangeLog->flush(); 
                I.setOperand(i,ResultPtr);
                *VarWidthChangeLog <<I<<"\n";
            }

        }
        //VarWidthChangeLog->flush();                                 
    }
    //VarWidthChangeLog->flush(); 
    *VarWidthChangeLog << "                         ------->  op0 type = "<<*ICMP_I->getOperand(0)->getType()<<"\n";
    *VarWidthChangeLog << "                         ------->  op1 type = "<<*ICMP_I->getOperand(1)->getType()<<"\n";
    
    // re-create the instruction to update the type(bitwidth) of it, otherwise, although the operans are changed, the output of instrcution will be remained.
    std::string regNameS = "new"+std::to_string(changed_id);
    ICmpInst *newCMP = new ICmpInst(
        ICMP_I,  ///< Where to insert
        ICMP_I->getPredicate(),  ///< The predicate to use for the comparison
        ICMP_I->getOperand(0),      ///< The left-hand-side of the expression
        ICMP_I->getOperand(1),      ///< The right-hand-side of the expression
        "HI."+ICMP_I->getName()+regNameS  ///< Name of the instruction
    ); 
    *VarWidthChangeLog << "                         ------->  new_CMP = "<<*newCMP<<"\n";
    // BOI->replaceAllUsesWith(newBOI) ;
    ReplaceUsesUnsafe(ICMP_I, newCMP) ;
    *VarWidthChangeLog << "                         ------->  accomplish replacement of original instruction in uses.\n";
    //VarWidthChangeLog->flush(); 
    I.eraseFromParent();
    *VarWidthChangeLog << "                         ------->  accomplish erasing of original instruction.\n";
    //VarWidthChangeLog->flush(); 
}


// Forward Process of PHI: check the bitwidth of operands and output of an instruction, trunc/ext the operands, update the bitwidth of the instruction
void HI_VarWidthReduce::PHI_WidthCast(PHINode *PHI_I)
{
    Instruction &I = *(cast<Instruction>(PHI_I));
    const SCEV *tmp_S = SE->getSCEV(&I);
    ConstantRange tmp_CR = HI_getSignedRangeRef(tmp_S);
    Value *ResultPtr = &I;
    *VarWidthChangeLog << "and its operands are:\n";
    //VarWidthChangeLog->flush(); 

    // check whether an instruction involve PTI operation
    for (int i = 0; i < I.getNumOperands(); ++i)
    {
        if (PtrToIntInst *PTI_I = dyn_cast<PtrToIntInst>(I.getOperand(i))) 
        {
            // if this instruction involve operands from pointer, 
            // we meed to ensure the operands have the same width of the pointer
            Instruction_BitNeeded[&I] = (cast<IntegerType>(I.getType()))->getBitWidth();
        }
    }

    for (int i = 0; i < I.getNumOperands(); ++i) // check the operands to see whether a TRUNC/EXT is necessary
    {
        *VarWidthChangeLog << "                         ------->  op#"<<i <<"==>"<<*I.getOperand(i)<<"\n";
        //VarWidthChangeLog->flush(); 
        if (ConstantInt *C_I = dyn_cast<ConstantInt>(I.getOperand(i)))                                
        {
            *VarWidthChangeLog << "                         ------->  op#"<<i<<" "<<*C_I<<" is a constant.\n";
            //VarWidthChangeLog->flush(); 
            //  if (C_I->getBitWidth()!= Instruction_BitNeeded[User_I])
            Type *NewTy_C = IntegerType::get(I.getType()->getContext(), Instruction_BitNeeded[&I]);
            Constant *New_C = ConstantInt::get(NewTy_C,*C_I->getValue().getRawData());
            *VarWidthChangeLog << "                         ------->  update"<<I<<" to ";
            //VarWidthChangeLog->flush(); 
            I.setOperand(i,New_C);
            *VarWidthChangeLog <<I<<"\n";
        }
        else
        {
            if (Instruction *Op_I = dyn_cast<Instruction>(I.getOperand(i)))
            {
                *VarWidthChangeLog << "                         ------->  op#"<<i<<" "<<*Op_I<<" is an instruction\n";
                //VarWidthChangeLog->flush(); 
                IRBuilder<> Builder( Op_I->getNextNode());
                std::string regNameS = "bcast"+std::to_string(changed_id);
                changed_id++;       
                // create a net type with specific bitwidth            
                Type *NewTy_OP = IntegerType::get(I.getType()->getContext(), Instruction_BitNeeded[&I]);
                if (tmp_CR.getLower().isNegative())
                {                                                
                    ResultPtr = Builder.CreateSExtOrTrunc(Op_I, NewTy_OP,regNameS.c_str());// process the operand with SExtOrTrunc if it is signed.
                }
                else
                {
                    ResultPtr = Builder.CreateZExtOrTrunc(Op_I, NewTy_OP,regNameS.c_str()); // process the operand with ZExtOrTrunc if it is unsigned.                                            
                }
                *VarWidthChangeLog << "                         ------->  update"<<I<<" to ";
                //VarWidthChangeLog->flush(); 
                I.setOperand(i,ResultPtr);
                *VarWidthChangeLog <<I<<"\n";
            }

        }
        //VarWidthChangeLog->flush();                                 
    }
    //VarWidthChangeLog->flush(); 
    *VarWidthChangeLog << "                         ------->  op0 type = "<<*PHI_I->getOperand(0)->getType()<<"\n";
    *VarWidthChangeLog << "                         ------->  op1 type = "<<*PHI_I->getOperand(1)->getType()<<"\n";
    Type *NewTy_PHI = IntegerType::get(I.getType()->getContext(), Instruction_BitNeeded[&I]);
    
    
    // re-create the instruction to update the type(bitwidth) of it, otherwise, although the operans are changed, the output of instrcution will be remained.
    std::string regNameS = "new"+std::to_string(changed_id);
    PHINode *new_PHI = PHINode::Create(NewTy_PHI, 0, "HI."+PHI_I->getName()+regNameS,PHI_I);
    for (int i = 0; i < I.getNumOperands(); ++i)
    {
        new_PHI->addIncoming(PHI_I->getIncomingValue(i),PHI_I->getIncomingBlock(i));
    }
    *VarWidthChangeLog << "                         ------->  new_PHI_I = "<<*new_PHI<<"\n";
    
    // BOI->replaceAllUsesWith(newBOI) ;
    ReplaceUsesUnsafe(PHI_I, new_PHI) ;
    *VarWidthChangeLog << "                         ------->  accomplish replacement of original instruction in uses.\n";
    //VarWidthChangeLog->flush(); 
    I.eraseFromParent();
    *VarWidthChangeLog << "                         ------->  accomplish erasing of original instruction.\n";
    //VarWidthChangeLog->flush(); 
}

// Determine the range for a particular SCEV, but bypass the operands generated from PtrToInt Instruction, considering the actual 
// implementation in HLS
const ConstantRange HI_VarWidthReduce::HI_getSignedRangeRef(const SCEV *S) 
{

    *VarWidthChangeLog << "        ------  HI_getSignedRangeRef handling SECV: " << *S->getType() << "\n";
    ConstantRange tmp_CR1 = SE->getSignedRange(S);
    if (!tmp_CR1.isFullSet())
    {
        *VarWidthChangeLog << "        ------  HI_getSignedRangeRef: it is not full-set " << tmp_CR1 << "\n";
        return tmp_CR1;
    }
    DenseMap<const SCEV *, ConstantRange> &Cache = SignedRanges;
    *VarWidthChangeLog << "        ------  handling full-set SECV: " << *S->getType() << "\n";
    // See if we've computed this range already.
    DenseMap<const SCEV *, ConstantRange>::iterator I = Cache.find(S);
    if (I != Cache.end())
        return I->second;

    if (const SCEVConstant *C = dyn_cast<SCEVConstant>(S))
        return setRange(C, ConstantRange(C->getAPInt()));

    unsigned BitWidth = SE->getTypeSizeInBits(S->getType());
    ConstantRange ConservativeResult(BitWidth, /*isFullSet=*/true);

    // If the value has known zeros, the maximum value will have those known zeros
    // as well.
    uint32_t TZ = SE->GetMinTrailingZeros(S);
    if (TZ != 0) 
    {
        ConservativeResult = ConstantRange(
            APInt::getSignedMinValue(BitWidth),
            APInt::getSignedMaxValue(BitWidth).ashr(TZ).shl(TZ) + 1);
    }

    if (const SCEVAddExpr *Add = dyn_cast<SCEVAddExpr>(S)) 
    {
        *VarWidthChangeLog << "        ------  Add\n";
        ConstantRange X = HI_getSignedRangeRef(Add->getOperand(0));
        for (unsigned i = 1, e = Add->getNumOperands(); i != e; ++i)
            if (bypassPTI(Add->getOperand(i))) 
                continue;
            else
                X = X.add(HI_getSignedRangeRef(Add->getOperand(i)));
        *VarWidthChangeLog << "            ------  handling full-set SECV new range: " << X << "\n";
        return setRange(Add, ConservativeResult.intersectWith(X));
    }

    if (const SCEVMulExpr *Mul = dyn_cast<SCEVMulExpr>(S)) 
    {
        *VarWidthChangeLog << "        ------  Mul\n";
        ConstantRange X = HI_getSignedRangeRef(Mul->getOperand(0));
        for (unsigned i = 1, e = Mul->getNumOperands(); i != e; ++i)
            if (bypassPTI(Mul->getOperand(i))) 
                continue; 
            else
                X = X.multiply(HI_getSignedRangeRef(Mul->getOperand(i)));
          *VarWidthChangeLog << "            ------  handling full-set SECV new range: " << X << "\n";
        return setRange(Mul, ConservativeResult.intersectWith(X));
    }

    if (const SCEVSMaxExpr *SMax = dyn_cast<SCEVSMaxExpr>(S)) 
    {
        *VarWidthChangeLog << "        ------  SMax\n";
        ConstantRange X = HI_getSignedRangeRef(SMax->getOperand(0));
        for (unsigned i = 1, e = SMax->getNumOperands(); i != e; ++i)
            if (bypassPTI(SMax->getOperand(i))) 
                continue;
            else
                X = X.smax(HI_getSignedRangeRef(SMax->getOperand(i)));
        *VarWidthChangeLog << "          ------  handling full-set SECV new range: " << X << "\n";
        return setRange(SMax, ConservativeResult.intersectWith(X));
    }

    if (const SCEVUMaxExpr *UMax = dyn_cast<SCEVUMaxExpr>(S)) 
    {
        *VarWidthChangeLog << "        ------  UMax\n";
        ConstantRange X = HI_getSignedRangeRef(UMax->getOperand(0));
        for (unsigned i = 1, e = UMax->getNumOperands(); i != e; ++i)
            if (bypassPTI(UMax->getOperand(i))) 
                continue;
            else
                X = X.umax(HI_getSignedRangeRef(UMax->getOperand(i)));
        *VarWidthChangeLog << "          ------  handling full-set SECV new range: " << X << "\n";
        return setRange(UMax, ConservativeResult.intersectWith(X));
    }

    if (const SCEVUDivExpr *UDiv = dyn_cast<SCEVUDivExpr>(S)) 
    {
        *VarWidthChangeLog << "        ------  UDiv\n";
        if (bypassPTI(UDiv->getLHS())) return ConservativeResult;
        if (bypassPTI(UDiv->getRHS())) return ConservativeResult;
        ConstantRange X = HI_getSignedRangeRef(UDiv->getLHS());
        ConstantRange Y = HI_getSignedRangeRef(UDiv->getRHS());
        return setRange(UDiv,
                        ConservativeResult.intersectWith(X.udiv(Y)));
    }

    if (const SCEVZeroExtendExpr *ZExt = dyn_cast<SCEVZeroExtendExpr>(S)) 
    {
        *VarWidthChangeLog << "        ------  ZExt\n";
        if (bypassPTI(ZExt->getOperand())) return ConservativeResult;
        ConstantRange X = HI_getSignedRangeRef(ZExt->getOperand());
        return setRange(ZExt,
                        ConservativeResult.intersectWith(X.zeroExtend(BitWidth)));
    }

    if (const SCEVSignExtendExpr *SExt = dyn_cast<SCEVSignExtendExpr>(S)) 
    {
        *VarWidthChangeLog << "        ------  SExt\n";
        if (bypassPTI(SExt->getOperand())) return ConservativeResult;
        ConstantRange X = HI_getSignedRangeRef(SExt->getOperand());
        return setRange(SExt,
                        ConservativeResult.intersectWith(X.signExtend(BitWidth)));
    }

    if (const SCEVTruncateExpr *Trunc = dyn_cast<SCEVTruncateExpr>(S)) 
    {
        *VarWidthChangeLog << "        ------  Trunc\n";
        if (bypassPTI(Trunc->getOperand())) return ConservativeResult;
        ConstantRange X = HI_getSignedRangeRef(Trunc->getOperand());
        return setRange(Trunc,
                        ConservativeResult.intersectWith(X.truncate(BitWidth)));
    }

    if (const SCEVAddRecExpr *AddRec = dyn_cast<SCEVAddRecExpr>(S)) 
    {
        *VarWidthChangeLog << "        ------  SCEVAddRecExpr\n";
    }

    if (const SCEVUnknown *U = dyn_cast<SCEVUnknown>(S)) 
    {
        *VarWidthChangeLog << "        ------  SCEVUnknown\n";
        if (PtrToIntInst *PTI = dyn_cast<PtrToIntInst>(U->getValue()))
        {   

        }

    }
    *VarWidthChangeLog << "        ------  Out of Scope\n";
    return setRange(S, std::move(ConservativeResult));
}

// cache constant range for those evaluated SCEVs
const ConstantRange &HI_VarWidthReduce::setRange(const SCEV *S,  ConstantRange CR) 
{
    DenseMap<const SCEV *, ConstantRange> &Cache = SignedRanges;

    auto Pair = Cache.try_emplace(S, std::move(CR));
    if (!Pair.second)
        Pair.first->second = std::move(CR);
    return Pair.first->second;
}


// check whether we should bypass the PtrToInt Instruction
bool HI_VarWidthReduce::bypassPTI(const SCEV *S)
{
    if (const SCEVUnknown *U = dyn_cast<SCEVUnknown>(S))
        if (PtrToIntInst *PTI = dyn_cast<PtrToIntInst>(U->getValue()))
        {
            *VarWidthChangeLog << "            ------  bypassing range evaluation for PtrToIntInst: " << *U->getValue() << "\n";
            return true;
        }
    return false;
}
