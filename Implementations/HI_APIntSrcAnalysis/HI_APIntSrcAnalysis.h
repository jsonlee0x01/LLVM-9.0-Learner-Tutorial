#ifndef _HI_APIntSrcAnalysis
#define _HI_APIntSrcAnalysis
// related headers should be included.
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <utility>
#include "HI_print.h"
#include "HI_SysExec.h"
#include "clang/AST/Type.h"
#include "clang/Driver/Options.h"
#include "clang/AST/AST.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Tooling.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include <stdio.h>
#include <string>
#include <ios>
#include <stdlib.h>
#include <map>
#include <set>
#include <vector>
#include <sstream>
#include "llvm/Support/raw_ostream.h"

using namespace clang;




//                         declare a rewriter
//                               |  pass the pointer to
//                  call         V
// frontend action  --->   the creator
//         |                     |  create / pass the rewriter
//         |   Src Code          V
//         ------------->   AST consumer
//                               |
//                               |  generate AST
//                               V
//                            Visitor (visit the nodes in AST and do the rewritting)


class HI_APIntSrcAnalysis_Visitor : public RecursiveASTVisitor<HI_APIntSrcAnalysis_Visitor> 
{
private:
    ASTContext *astContext; // used for getting additional AST info
    CompilerInstance *CI;
    Rewriter *R;
public:
    PrintingPolicy PP()
    {
        return PrintingPolicy(CI->getLangOpts());
    }

    explicit HI_APIntSrcAnalysis_Visitor(CompilerInstance *_CI, Rewriter *_R): astContext(&(_CI->getASTContext())) 
    {  // initialize private members
        CI = _CI;
        R = _R;
        R->setSourceMgr(astContext->getSourceManager(), astContext->getLangOpts());
        parseLog = new llvm::raw_fd_ostream("parseLog", ErrInfo, llvm::sys::fs::F_None);
        tmp_stream = new llvm::raw_string_ostream(tmp_stream_str);
    }

    ~HI_APIntSrcAnalysis_Visitor()
    {
        parseLog->flush();
        delete parseLog;
    }


    virtual bool VisitVarDecl(VarDecl *VD) 
    { 
        *parseLog << "find VarDecl: VarName: ["  << VD->getNameAsString() << "] DeclKind:[" 
            << VD->getDeclKindName()<< "] Type: ["<< VD->getType().getAsString() << "] at Loc: [" 
            << VD->getBeginLoc().printToString(CI->getSourceManager()) <<"]\n";
        *parseLog << "    ---  detailed information of the type\n";
        printTypeInfo(VD->getType().getTypePtr());
        if (isAPInt(VD))
        {
            R->InsertText(VD->getBeginLoc(), "// "+VD->getNameAsString() +" is ap int type ("+getAPIntName(VD)+").\n");
        }
        return true;
    }

    virtual bool VisitFunctionDecl(FunctionDecl *func) 
    {
        std::string funcName;
        funcName = func->getNameInfo().getName().getAsString();
        if (func->isReferenced())
        {
            R->InsertText(func->getBody()->getBeginLoc(), "// used function\n");
        }
        parseLog->flush();
        return true;
    }


    virtual bool VisitStmt(Stmt *st) 
    {        
        return true;
    }
    
    virtual bool VisitType(Type *T) 
    {
        return true;
    }

    void printTypeInfo(const Type *T);
    bool isAPInt(VarDecl *VD);
    std::string getAPIntName(VarDecl *VD);

    std::error_code ErrInfo;
    raw_ostream *parseLog;

    llvm::raw_string_ostream *tmp_stream;
    std::string tmp_stream_str;

};



class HI_APIntSrcAnalysis_ASTConsumer : public ASTConsumer 
{
private:
    HI_APIntSrcAnalysis_Visitor *visitor; // doesn't have to be private
    Rewriter *R;
public:
    // override the constructor in order to pass CI
    explicit HI_APIntSrcAnalysis_ASTConsumer(CompilerInstance *CI,Rewriter *_R): visitor(new HI_APIntSrcAnalysis_Visitor(CI,_R)) {R = _R;}// initialize the visitor

    // override this to call our HI_APIntSrcAnalysis_Visitor on the entire source file
    virtual void HandleAnalysislationUnit(ASTContext &Context) 
    {
        /* we can use ASTContext to get the AnalysislationUnitDecl, which is
             a single Decl that collectively represents the entire source file */
        visitor->TraverseDecl(Context.getTranslationUnitDecl());
    }
};


// According the official template of Clang, this is a creater with newASTConsumer(), which
// will generator a AST consumer. We can first create a rewriter and pass the pointer of the
// rewriter to the creator. Finally,  we can pass the rewriter pointer to the inner visitor.
// rewriter ptr -> creator -> frontend-action -> ASTconsumer -> Visitor
class HI_APIntSrcAnalysis_Creator
{
private:
    Rewriter *R;
public:
    
    // override the constructor in order to pass the rewriter to the ASTConsumer
    explicit HI_APIntSrcAnalysis_Creator(Rewriter *_R) { R = _R;}// initialize the creator, pass the pointer of rewriter
    std::unique_ptr<ASTConsumer> newASTConsumer(CompilerInstance &CI) 
    {
        return llvm::make_unique<HI_APIntSrcAnalysis_ASTConsumer>(&CI,R);
    }
};




// According the official template of Clang, this is a creater with newASTConsumer(), which
// will generator a AST consumer. We can first create a rewriter and pass the pointer of the
// rewriter to the creator. Finally,  we can pass the rewriter pointer to the inner visitor.
// rewriter ptr -> creator -> frontend-action -> ASTconsumer -> Visitor
template <typename FactoryT>
inline std::unique_ptr<tooling::FrontendActionFactory> HI_Rewrite_newFrontendActionFactory(
    FactoryT *ConsumerFactory, tooling::SourceFileCallbacks *Callbacks) {



  class FrontendActionFactoryAdapter : public tooling::FrontendActionFactory {
  public:
    explicit FrontendActionFactoryAdapter(FactoryT *ConsumerFactory,
                                          tooling::SourceFileCallbacks *Callbacks)
        : ConsumerFactory(ConsumerFactory), Callbacks(Callbacks) {}

    FrontendAction *create() override {
      return new ConsumerFactoryAdaptor(ConsumerFactory, Callbacks);
    }



  private:
    class ConsumerFactoryAdaptor : public ASTFrontendAction {
    public:
      ConsumerFactoryAdaptor(FactoryT *ConsumerFactory,
                             tooling::SourceFileCallbacks *Callbacks)
          : ConsumerFactory(ConsumerFactory), Callbacks(Callbacks) {}

      std::unique_ptr<ASTConsumer>
      CreateASTConsumer(CompilerInstance &CI, StringRef file)  override { // we cannot modify the declaration of function (name+argument)
        return ConsumerFactory->newASTConsumer(CI);   // <====  This line is different from the  official template, we use newASTConsumer to pass CI
      }

    protected:
      bool BeginSourceFileAction(CompilerInstance &CI) override {
        if (!ASTFrontendAction::BeginSourceFileAction(CI))
          return false;
        if (Callbacks)
          return Callbacks->handleBeginSource(CI);
        return true;
      }

      void EndSourceFileAction() override {
        if (Callbacks)
          Callbacks->handleEndSource();
        ASTFrontendAction::EndSourceFileAction();
      }

    private:
      FactoryT *ConsumerFactory;
      tooling::SourceFileCallbacks *Callbacks;
    };
    FactoryT *ConsumerFactory;
    tooling::SourceFileCallbacks *Callbacks;
  };

  return std::unique_ptr<tooling::FrontendActionFactory>(
      new FrontendActionFactoryAdapter(ConsumerFactory, Callbacks));
}




#endif