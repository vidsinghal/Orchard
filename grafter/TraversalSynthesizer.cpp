//===--- TraversalSynthesizer.cpp
//---------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT fo details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "TraversalSynthesizer.h"

#define FUSE_CAP 2
#define diff_CAP 4
using namespace std;
#include <string>

std::map<clang::FunctionDecl *, int> TraversalSynthesizer::FunDeclToNameId =
    std::map<clang::FunctionDecl *, int>();
std::map<std::vector<clang::CallExpr *>, string> TraversalSynthesizer::Stubs =
    std::map<std::vector<clang::CallExpr *>, string>();
std::unordered_map<const CXXRecordDecl *, std::set<std::string>> InsertedStubs =
    std::unordered_map<const CXXRecordDecl *, std::set<std::string>>();
int TraversalSynthesizer::Count = 1;

std::string toBinaryString(unsigned Input) {
  string Output;
  while (Input != 0) {
    int Tmp = Input % 2;
    Output = to_string(Tmp) + Output;
    Input = Input / 2;
  }
  if (Output == "")
    Output = "0";
  Output = "0b" + Output;
  return Output;
}

unsigned TraversalSynthesizer::getNumberOfParticipatingTraversals(
    const std::vector<bool> &ParticipatingTraversals) const {
  unsigned Count = 0;
  for (bool Entry : ParticipatingTraversals) {
    if (Entry)
      Count++;
  }
  return Count;
}

int TraversalSynthesizer::getFirstParticipatingTraversal(
    const std::vector<bool> &ParticipatingTraversals) const {
  for (int i = 0; i < ParticipatingTraversals.size(); i++) {
    if (ParticipatingTraversals[i] == true) {
      return i;
    }
  }
  return -1;
}

bool TraversalSynthesizer::isGenerated(
    const vector<clang::CallExpr *> &ParticipatingTraversals, bool HasVirtual,
    const clang::CXXRecordDecl *TraversedType) {
  auto NewFunctionId =
      createName(ParticipatingTraversals, HasVirtual, TraversedType);
  return SynthesizedFunctions.count(NewFunctionId);
}

bool TraversalSynthesizer::isGenerated(
    const vector<clang::FunctionDecl *> &ParticipatingTraversals) {
  auto NewFunctionId = createName(ParticipatingTraversals);
  return SynthesizedFunctions.count(NewFunctionId);
}

std::string TraversalSynthesizer::createName(
    const std::vector<clang::CallExpr *> &ParticipatingTraversals,
    bool HasVirtual, const clang::CXXRecordDecl *TraversedType) {

  std::vector<clang::FunctionDecl *> Temp;
  Temp.resize(ParticipatingTraversals.size());
  transform(ParticipatingTraversals.begin(), ParticipatingTraversals.end(),
            Temp.begin(), [&](clang::CallExpr *CallExpr) {
              auto *CalleeDecl =
                  dyn_cast<clang::FunctionDecl>(CallExpr->getCalleeDecl())
                      ->getDefinition();

              auto *CalleeInfo = FunctionsFinder::getFunctionInfo(CalleeDecl);

              if (CalleeInfo->isVirtual())
                return (clang::FunctionDecl *)CalleeInfo->getDeclAsCXXMethod()
                    ->getCorrespondingMethodInClass(TraversedType)
                    ->getDefinition();
              else
                return CalleeDecl;
            });

  return createName(Temp);
}

std::string TraversalSynthesizer::createName(
    const std::vector<clang::FunctionDecl *> &ParticipatingTraversals) {

  std::string Output = string("_fuse_") + "_";

  for (auto *FuncDecl : ParticipatingTraversals) {
    FuncDecl = FuncDecl->getDefinition();
    if (!FunDeclToNameId.count(FuncDecl)) {
      FunDeclToNameId[FuncDecl] = Count++;
      LLVM_DEBUG(Logger::getStaticLogger().logInfo(
          "Function:" + FuncDecl->getQualifiedNameAsString() + "==>" +
          to_string(Count - 1) + "\n"));
    }

    Output += +"F" + std::to_string(FunDeclToNameId[FuncDecl]);
  }
  return Output;
}

int TraversalSynthesizer::getFunctionId(clang::FunctionDecl *Decl) {
  assert(FunDeclToNameId.count(Decl));

  return FunDeclToNameId[Decl];
}

string StringReplace(std::string str, const std::string &from,
                     const std::string &to) {
  size_t start_pos = str.find(from);
  if (start_pos == std::string::npos)
    return str;
  str.replace(start_pos, from.length(), to);
  return str;
}
void TraversalSynthesizer::
    setBlockSubPart(/*
string &Decls,*/ std::string &BlockPart,
                    const std::vector<clang::FunctionDecl *>
                        &ParticipatingTraversalsDecl,
                    const int BlockId,
                    std::unordered_map<int, vector<DG_Node *>> &Statements,
                    bool HasCXXCall) {
  StatementPrinter Printer;

  for (int TraversalIndex = 0;
       TraversalIndex < ParticipatingTraversalsDecl.size(); TraversalIndex++) {
    std::string Declarations = "";

    if (!Statements.count(TraversalIndex))
      continue;

    auto *Decl = ParticipatingTraversalsDecl[TraversalIndex];

    string NextLabel = "_label_B" + to_string(BlockId) + +"F" +
                       to_string(TraversalIndex) + "_Exit";

    bool DumpNextLabel = false;
    string BlockBody = "";
    for (DG_Node *Statement : Statements[TraversalIndex]) {
      // toplevel declaration statements need to be seen by the whole function
      // since we are conditionally executing the block, we need to move the
      // declarations before the if condition but keep initializations in its
      // place

      if (Statement->getStatementInfo()->Stmt->getStmtClass() ==
          clang::Stmt::DeclStmtClass) {

        auto *CurrentDecl =
            dyn_cast<clang::DeclStmt>(Statement->getStatementInfo()->Stmt);
        for (auto *D : CurrentDecl->decls()) {
          auto *VarDecl = dyn_cast<clang::VarDecl>(D);

          // add the  declaration at the top of the block body
          Declarations +=
              StringReplace(VarDecl->getType().getAsString(), "const", "") +
              " ";
          Declarations += "_f" + to_string(TraversalIndex) + "_" +
                          VarDecl->getNameAsString() + ";\n";

          if (VarDecl->hasInit()) {
            BlockBody +=
                "_f" + to_string(TraversalIndex) + "_" +
                VarDecl->getNameAsString() + "=" +
                Printer.printStmt(
                    VarDecl->getInit(), ASTCtx->getSourceManager(),
                    FunctionsFinder::getFunctionInfo(Decl)->isGlobal()
                        ? Decl->getParamDecl(0)
                        : nullptr,
                    NextLabel, TraversalIndex, HasCXXCall, HasCXXCall) +
                ";\n";
          }
        }

      } else {
        // Nullptr is passed as root decl TODO:
        BlockBody += Printer.printStmt(
            Statement->getStatementInfo()->Stmt, ASTCtx->getSourceManager(),
            FunctionsFinder::getFunctionInfo(Decl)->isGlobal()
                ? Decl->getParamDecl(0)
                : nullptr,
            NextLabel, TraversalIndex, HasCXXCall, HasCXXCall);
      }
    }
    BlockPart += Declarations;

    if (BlockBody.compare("") != 0) {
      BlockPart += "if (truncate_flags &" +
                   toBinaryString((1 << TraversalIndex)) + ") {\n";
      BlockPart += BlockBody;
      BlockPart += "}\n";
      DumpNextLabel = true;
    }
    if (DumpNextLabel)
      BlockPart += NextLabel + ":\n";
  }
}

void TraversalSynthesizer::setCallPart(
    std::string &CallPartText,
    const std::vector<clang::CallExpr *> &ParticipatingCallExpr,
    const std::vector<clang::FunctionDecl *> &ParticipatingTraversalsDecl,
    DG_Node *CallNode, FusedTraversalWritebackInfo *WriteBackInfo,
    bool HasCXXCall, int isParallel) {
  CallPartText = "";
  StatementPrinter Printer;

  std::vector<DG_Node *> NextCallNodes;
  if (CallNode->isMerged())
    NextCallNodes = CallNode->getMergeInfo()->getCallsOrdered();
  else
    NextCallNodes.push_back(CallNode);

  if (!NextCallNodes.size())
    return;

  // The call should be executed iff one of at least of the participating nodes
  // is active
  unsigned int ConditionBitMask = 0;

  for (DG_Node *Node : NextCallNodes)
    ConditionBitMask |=
        (1 << Node->getTraversalId() /*should return the index*/);

  string CallConditionText = "if ( (truncate_flags & " +
                             toBinaryString(ConditionBitMask) + ") )/*call*/";

  // CallPartText += "HI!";
  CallPartText += CallConditionText + "{\n\t";
  // Adjust truncate flags of the new called function

  string AdjustedFlagCode = "unsigned int AdjustedTruncateFlags = 0 ;\n";

  for (auto it = NextCallNodes.rbegin(); it != NextCallNodes.rend(); ++it) {
    AdjustedFlagCode += "AdjustedTruncateFlags <<= 1;\n";
    AdjustedFlagCode += "AdjustedTruncateFlags |=("
                        " 0b01 & (truncate_flags >>" +
                        to_string((*it)->getTraversalId()) + "));\n";
    ;
  }

  /***************************************************************************************************************************************************/
  /*NOTE: This if condition part has been commented to enable code generation
     for parallel calls uncomment this if you need to use grafter in fused ->
     paralle mode with more optimization*/

  // add an argument to enable disable this optimization
  // if (NextCallNodes.size() == 1) {            //remove if condition
  //   int CalledTraversalId = NextCallNodes[0]->getTraversalId();

  //   auto *RootDecl =
  //       CallNode->getStatementInfo()->getEnclosingFunction()->isGlobal()
  //           ? CallNode->getStatementInfo()
  //                 ->getEnclosingFunction()
  //                 ->getFunctionDecl()
  //                 ->getParamDecl(0)
  //           : nullptr;

  //   auto * cilkSpawn = "cilk_spawn "; //added cilkspawn for parallelization
  //   //if(CallNode -> isSpawned())
  //   CallPartText += cilkSpawn;
  //   CallPartText += Printer.printStmt(
  //       NextCallNodes[0]->getStatementInfo()->Stmt,
  //       ASTCtx->getSourceManager(), RootDecl, "not used",
  //       CallNode->getTraversalId(),
  //       /*replace this*/ HasCXXCall, HasCXXCall);

  //   CallPartText += "\n}";
  //   return;
  // }
  /***************************************************************************************************************************************************/

  // dd adjusted flags code

  CallPartText += AdjustedFlagCode;

  std::vector<clang::CallExpr *> NexTCallExpressions;

  for (auto *Node : NextCallNodes)
    NexTCallExpressions.push_back(
        dyn_cast<clang::CallExpr>(Node->getStatementInfo()->Stmt));

  bool HasVirtual = false;
  bool HasCXXMethod = false;

  for (auto *Call : NexTCallExpressions) {
    auto *CalleeInfo = FunctionsFinder::getFunctionInfo(
        Call->getCalleeDecl()->getAsFunction()->getDefinition());
    if (CalleeInfo->isCXXMember())
      HasCXXMethod = true;
    if (CalleeInfo->isVirtual())
      HasVirtual = true;
  }

  std::string NextCallName;
  string NextCallParamsText;

  if (!HasVirtual)
    NextCallName = createName(NexTCallExpressions, false, nullptr);
  else
    NextCallName = getVirtualStub(NexTCallExpressions);

  // if (NextCallName == "__virtualStub14")
  //   assert(false);

  Transformer->performFusion(
      NexTCallExpressions, /*IsTopLevel*/ false,
      CallNode->getStatementInfo()->getEnclosingFunction()->getFunctionDecl(),
      Transformer->Heuristic);

  auto *RootDeclCallNode =
      CallNode->getStatementInfo()->getEnclosingFunction()->isGlobal()
          ? CallNode->getStatementInfo()
                ->getEnclosingFunction()
                ->getFunctionDecl()
                ->getParamDecl(0)
          : nullptr;

  // Create the call
  // Adding code for depth control

  // this part is for calls that are to be executed in parallel with a
  // cilk_spawn
  if (isParallel == 1) {

    CallPartText += "if (depth < maxDepth)";
    CallPartText += " {";
    CallPartText += "\n";

    /***************************************************************************************************************************************************/

    // add cilk_spawn for parallelization

    CallPartText += "cilk_spawn ";

    if (!HasVirtual) {
      CallPartText += NextCallName + "_parallel" + "(";

      if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
          clang::Stmt::CallExprClass) {
        auto FirstArgument =
            dyn_cast<clang::CallExpr>(CallNode->getStatementInfo()->Stmt)
                ->getArg(0);
        NextCallParamsText += Printer.printStmt(
            FirstArgument, ASTCtx->getSourceManager(), RootDeclCallNode, "",
            CallNode->getTraversalId(), HasCXXCall, HasCXXCall);

      } else if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
                 clang::Stmt::CXXMemberCallExprClass) {
        NextCallParamsText += Printer.printStmt(
            CallNode->getStatementInfo()
                ->Stmt->child_begin()
                ->child_begin()
                ->IgnoreImplicit(),
            ASTCtx->getSourceManager(), RootDeclCallNode, "",
            CallNode->getTraversalId(), HasCXXCall, HasCXXCall);
      }

    } else {

      if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
          clang::Stmt::CXXMemberCallExprClass) {
        CallPartText +=
            Printer.printStmt(CallNode->getStatementInfo()
                                  ->Stmt->child_begin()
                                  ->child_begin()
                                  ->IgnoreImplicit(),
                              ASTCtx->getSourceManager(), RootDeclCallNode, "",
                              CallNode->getTraversalId(), HasCXXCall,
                              HasCXXCall) +
            "->" + NextCallName + "_parallel" + "(";

      } else if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
                 clang::Stmt::CallExprClass) {
        auto FirstArgument =
            dyn_cast<clang::CallExpr>(CallNode->getStatementInfo()->Stmt)
                ->getArg(0);

        CallPartText += NextCallName + "_parallel" + "(";
        NextCallParamsText += Printer.printStmt(
            FirstArgument, ASTCtx->getSourceManager(), RootDeclCallNode, "",
            CallNode->getTraversalId(), HasCXXCall, HasCXXCall);
      } else {
        llvm_unreachable("unexpected");
      }
    }

    for (auto *CallNode : NextCallNodes) {
      auto *CallExpr =
          dyn_cast<clang::CallExpr>(CallNode->getStatementInfo()->Stmt);
      auto *RootDecl =
          CallNode->getStatementInfo()->getEnclosingFunction()->isGlobal()
              ? CallNode->getStatementInfo()
                    ->getEnclosingFunction()
                    ->getFunctionDecl()
                    ->getParamDecl(0)
              : nullptr;
      for (int ArgIdx =
               CallNode->getStatementInfo()->getEnclosingFunction()->isGlobal()
                   ? 1
                   : 0;
           ArgIdx < CallExpr->getNumArgs(); ArgIdx++) {
        NextCallParamsText +=
            (NextCallParamsText == "" ? "" : ", ") +
            Printer.printStmt(
                CallExpr->getArg(ArgIdx), ASTCtx->getSourceManager(),
                RootDecl /* not used*/, "not-used", CallNode->getTraversalId(),
                HasCXXCall, HasCXXCall);
      }
    }

    // Added variables depth and maxDepth to control the depth of recursion in
    // the parallel code

    NextCallParamsText +=
        (NextCallParamsText == ""
             ? "AdjustedTruncateFlags, depth + 1, maxDepth"
             : ", AdjustedTruncateFlags, depth + 1, maxDepth");

    /***************************************************************************************************************************************************/

    CallPartText += NextCallParamsText;
    CallPartText += ");";

    CallPartText += "}\n";

    // This is the second else call to the depth control part
    NextCallParamsText = "";

    CallPartText += "else";
    CallPartText += " {";
    CallPartText += "\n";

    /***************************************************************************************************************************************************/

    // write the serial code for the call part
    if (!HasVirtual) {
      CallPartText += NextCallName + "_serial" + "(";

      if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
          clang::Stmt::CallExprClass) {
        auto FirstArgument =
            dyn_cast<clang::CallExpr>(CallNode->getStatementInfo()->Stmt)
                ->getArg(0);
        NextCallParamsText += Printer.printStmt(
            FirstArgument, ASTCtx->getSourceManager(), RootDeclCallNode, "",
            CallNode->getTraversalId(), HasCXXCall, HasCXXCall);

      } else if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
                 clang::Stmt::CXXMemberCallExprClass) {
        NextCallParamsText += Printer.printStmt(
            CallNode->getStatementInfo()
                ->Stmt->child_begin()
                ->child_begin()
                ->IgnoreImplicit(),
            ASTCtx->getSourceManager(), RootDeclCallNode, "",
            CallNode->getTraversalId(), HasCXXCall, HasCXXCall);
      }

    } else {

      if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
          clang::Stmt::CXXMemberCallExprClass) {
        CallPartText +=
            Printer.printStmt(CallNode->getStatementInfo()
                                  ->Stmt->child_begin()
                                  ->child_begin()
                                  ->IgnoreImplicit(),
                              ASTCtx->getSourceManager(), RootDeclCallNode, "",
                              CallNode->getTraversalId(), HasCXXCall,
                              HasCXXCall) +
            "->" + NextCallName + "_serial" + "(";
      } else if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
                 clang::Stmt::CallExprClass) {
        auto FirstArgument =
            dyn_cast<clang::CallExpr>(CallNode->getStatementInfo()->Stmt)
                ->getArg(0);

        CallPartText += NextCallName + "_serial" + "(";
        NextCallParamsText += Printer.printStmt(
            FirstArgument, ASTCtx->getSourceManager(), RootDeclCallNode, "",
            CallNode->getTraversalId(), HasCXXCall, HasCXXCall);
      } else {
        llvm_unreachable("unexpected");
      }
    }

    for (auto *CallNode : NextCallNodes) {
      auto *CallExpr =
          dyn_cast<clang::CallExpr>(CallNode->getStatementInfo()->Stmt);
      auto *RootDecl =
          CallNode->getStatementInfo()->getEnclosingFunction()->isGlobal()
              ? CallNode->getStatementInfo()
                    ->getEnclosingFunction()
                    ->getFunctionDecl()
                    ->getParamDecl(0)
              : nullptr;
      for (int ArgIdx =
               CallNode->getStatementInfo()->getEnclosingFunction()->isGlobal()
                   ? 1
                   : 0;
           ArgIdx < CallExpr->getNumArgs(); ArgIdx++) {
        NextCallParamsText +=
            (NextCallParamsText == "" ? "" : ", ") +
            Printer.printStmt(
                CallExpr->getArg(ArgIdx), ASTCtx->getSourceManager(),
                RootDecl /* not used*/, "not-used", CallNode->getTraversalId(),
                HasCXXCall, HasCXXCall);
      }
    }

    // Added variables depth and maxDepth to control the depth of recursion for
    // parallel code

    NextCallParamsText +=
        (NextCallParamsText == "" ? "AdjustedTruncateFlags"
                                  : ", AdjustedTruncateFlags");

    /***************************************************************************************************************************************************/

    CallPartText += NextCallParamsText;
    CallPartText += ");";

    CallPartText += "}";

    CallPartText += "\n}";

  }

  // ADD code for just the serial version of the code with no parallel part
  else {

    std::string SerialCallName = NextCallName;

    // no cilk_spawn for the last call
    if (isParallel == 2) {
      CallPartText += "if (depth < maxDepth)";
      CallPartText += " {";
      CallPartText += "\n";
    }

    /***************************************************************************************************************************************************/

    // for the serial part of the code
    if (isParallel == 3) {
      NextCallName += "_serial";
    } else {
      NextCallName += "_parallel";
    }

    if (!HasVirtual) {
      CallPartText += NextCallName + "(";

      if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
          clang::Stmt::CallExprClass) {
        auto FirstArgument =
            dyn_cast<clang::CallExpr>(CallNode->getStatementInfo()->Stmt)
                ->getArg(0);
        NextCallParamsText += Printer.printStmt(
            FirstArgument, ASTCtx->getSourceManager(), RootDeclCallNode, "",
            CallNode->getTraversalId(), HasCXXCall, HasCXXCall);

      } else if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
                 clang::Stmt::CXXMemberCallExprClass) {
        NextCallParamsText += Printer.printStmt(
            CallNode->getStatementInfo()
                ->Stmt->child_begin()
                ->child_begin()
                ->IgnoreImplicit(),
            ASTCtx->getSourceManager(), RootDeclCallNode, "",
            CallNode->getTraversalId(), HasCXXCall, HasCXXCall);
      }

    } else {

      if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
          clang::Stmt::CXXMemberCallExprClass) {
        CallPartText +=
            Printer.printStmt(CallNode->getStatementInfo()
                                  ->Stmt->child_begin()
                                  ->child_begin()
                                  ->IgnoreImplicit(),
                              ASTCtx->getSourceManager(), RootDeclCallNode, "",
                              CallNode->getTraversalId(), HasCXXCall,
                              HasCXXCall) +
            "->" + NextCallName + "(";
      } else if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
                 clang::Stmt::CallExprClass) {
        auto FirstArgument =
            dyn_cast<clang::CallExpr>(CallNode->getStatementInfo()->Stmt)
                ->getArg(0);

        CallPartText += NextCallName + "(";
        NextCallParamsText += Printer.printStmt(
            FirstArgument, ASTCtx->getSourceManager(), RootDeclCallNode, "",
            CallNode->getTraversalId(), HasCXXCall, HasCXXCall);
      } else {
        llvm_unreachable("unexpected");
      }
    }

    for (auto *CallNode : NextCallNodes) {
      auto *CallExpr =
          dyn_cast<clang::CallExpr>(CallNode->getStatementInfo()->Stmt);
      auto *RootDecl =
          CallNode->getStatementInfo()->getEnclosingFunction()->isGlobal()
              ? CallNode->getStatementInfo()
                    ->getEnclosingFunction()
                    ->getFunctionDecl()
                    ->getParamDecl(0)
              : nullptr;
      for (int ArgIdx =
               CallNode->getStatementInfo()->getEnclosingFunction()->isGlobal()
                   ? 1
                   : 0;
           ArgIdx < CallExpr->getNumArgs(); ArgIdx++) {
        NextCallParamsText +=
            (NextCallParamsText == "" ? "" : ", ") +
            Printer.printStmt(
                CallExpr->getArg(ArgIdx), ASTCtx->getSourceManager(),
                RootDecl /* not used*/, "not-used", CallNode->getTraversalId(),
                HasCXXCall, HasCXXCall);
      }
    }

    // Added variables depth and maxDepth to control the depth of recursion in
    // the parallel code

    if (isParallel == 2) {

      NextCallParamsText +=
          (NextCallParamsText == ""
               ? "AdjustedTruncateFlags, depth + 1, maxDepth"
               : ", AdjustedTruncateFlags, depth + 1, maxDepth");
    }

    /***************************************************************************************************************************************************/

    if (isParallel == 2) {
      CallPartText += NextCallParamsText;
      CallPartText += ");";
      CallPartText += "}\n";

      // This is the second else call to the depth control part
      NextCallParamsText = "";

      CallPartText += "else";
      CallPartText += " {";
      CallPartText += "\n";

      /***************************************************************************************************************************************************/

      // for the fully serial code
      NextCallName = SerialCallName + "_serial";

      if (!HasVirtual) {
        CallPartText += NextCallName + "(";

        if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
            clang::Stmt::CallExprClass) {
          auto FirstArgument =
              dyn_cast<clang::CallExpr>(CallNode->getStatementInfo()->Stmt)
                  ->getArg(0);
          NextCallParamsText += Printer.printStmt(
              FirstArgument, ASTCtx->getSourceManager(), RootDeclCallNode, "",
              CallNode->getTraversalId(), HasCXXCall, HasCXXCall);

        } else if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
                   clang::Stmt::CXXMemberCallExprClass) {
          NextCallParamsText += Printer.printStmt(
              CallNode->getStatementInfo()
                  ->Stmt->child_begin()
                  ->child_begin()
                  ->IgnoreImplicit(),
              ASTCtx->getSourceManager(), RootDeclCallNode, "",
              CallNode->getTraversalId(), HasCXXCall, HasCXXCall);
        }

      } else {
        if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
            clang::Stmt::CXXMemberCallExprClass) {
          CallPartText +=
              Printer.printStmt(CallNode->getStatementInfo()
                                    ->Stmt->child_begin()
                                    ->child_begin()
                                    ->IgnoreImplicit(),
                                ASTCtx->getSourceManager(), RootDeclCallNode,
                                "", CallNode->getTraversalId(), HasCXXCall,
                                HasCXXCall) +
              "->" + NextCallName + "(";
        } else if (CallNode->getStatementInfo()->Stmt->getStmtClass() ==
                   clang::Stmt::CallExprClass) {
          auto FirstArgument =
              dyn_cast<clang::CallExpr>(CallNode->getStatementInfo()->Stmt)
                  ->getArg(0);

          CallPartText += NextCallName + "(";
          NextCallParamsText += Printer.printStmt(
              FirstArgument, ASTCtx->getSourceManager(), RootDeclCallNode, "",
              CallNode->getTraversalId(), HasCXXCall, HasCXXCall);
        } else {
          llvm_unreachable("unexpected");
        }
      }

      for (auto *CallNode : NextCallNodes) {
        auto *CallExpr =
            dyn_cast<clang::CallExpr>(CallNode->getStatementInfo()->Stmt);
        auto *RootDecl =
            CallNode->getStatementInfo()->getEnclosingFunction()->isGlobal()
                ? CallNode->getStatementInfo()
                      ->getEnclosingFunction()
                      ->getFunctionDecl()
                      ->getParamDecl(0)
                : nullptr;
        for (int ArgIdx = CallNode->getStatementInfo()
                                  ->getEnclosingFunction()
                                  ->isGlobal()
                              ? 1
                              : 0;
             ArgIdx < CallExpr->getNumArgs(); ArgIdx++) {
          NextCallParamsText +=
              (NextCallParamsText == "" ? "" : ", ") +
              Printer.printStmt(
                  CallExpr->getArg(ArgIdx), ASTCtx->getSourceManager(),
                  RootDecl /* not used*/, "not-used",
                  CallNode->getTraversalId(), HasCXXCall, HasCXXCall);
        }
      }
    }

    NextCallParamsText +=
        (NextCallParamsText == "" ? "AdjustedTruncateFlags"
                                  : ", AdjustedTruncateFlags");

    /***************************************************************************************************************************************************/

    CallPartText += NextCallParamsText;
    CallPartText += ");";
    CallPartText += "}";

    if (isParallel == 2) {
      CallPartText += "\n}";
    }
  }

  return;
}

const clang::CXXRecordDecl *
extractDeclTraversedType(clang::FunctionDecl *FuncDecl) {
  auto *FunctionInfo =
      FunctionsFinder::getFunctionInfo(FuncDecl->getDefinition());
  return dyn_cast<CXXRecordDecl>(FunctionInfo->getTraversedTreeTypeDecl());
}

// return th

const clang::CXXRecordDecl *getHighestCommonTraversedType(
    std::vector<clang::FunctionDecl *> &ParticipatingFunctions) {
  auto *HighestCommon = extractDeclTraversedType(ParticipatingFunctions[0]);
  for (int i = 1; i < ParticipatingFunctions.size(); i++) {
    const clang::CXXRecordDecl *Candidate =
        extractDeclTraversedType(ParticipatingFunctions[i]);
    if (Candidate == HighestCommon)
      continue;
    if (std::find(RecordsAnalyzer::DerivedRecords[HighestCommon].begin(),
                  RecordsAnalyzer::DerivedRecords[HighestCommon].end(),
                  Candidate) !=
        RecordsAnalyzer::DerivedRecords[HighestCommon].end()) {
      HighestCommon = Candidate;
    } else if (std::find(RecordsAnalyzer::DerivedRecords[Candidate].begin(),
                         RecordsAnalyzer::DerivedRecords[Candidate].end(),
                         HighestCommon) !=
               RecordsAnalyzer::DerivedRecords[Candidate].end()) {
      // do nothing
    } else {
      llvm_unreachable("not supposed to happen !");
    }
  }
  return HighestCommon;
}
void TraversalSynthesizer::generateWriteBackInfo(
    const std::vector<clang::CallExpr *> &ParticipatingCalls,
    const std::vector<vector<DG_Node *>> &TopologicalOrder, bool HasVirtual,
    bool HasCXXCall, const CXXRecordDecl *DerivedType) {

  StatementPrinter Printer;

  // generate the name of the new function
  string idName = createName(ParticipatingCalls, HasVirtual, DerivedType);

  // check if already generated and assert on It
  assert(!isGenerated(ParticipatingCalls, HasVirtual, DerivedType));

  vector<clang::FunctionDecl *> TraversalsDeclarationsList;
  TraversalsDeclarationsList.resize(ParticipatingCalls.size());
  transform(ParticipatingCalls.begin(), ParticipatingCalls.end(),
            TraversalsDeclarationsList.begin(), [&](clang::CallExpr *CallExpr) {
              auto *CalleeDecl =
                  dyn_cast<clang::FunctionDecl>(CallExpr->getCalleeDecl())
                      ->getDefinition();

              auto *CalleeInfo = FunctionsFinder::getFunctionInfo(CalleeDecl);

              if (CalleeInfo->isVirtual())
                return (clang::FunctionDecl *)CalleeInfo->getDeclAsCXXMethod()
                    ->getCorrespondingMethodInClass(DerivedType)
                    ->getDefinition();
              else
                return CalleeDecl;
            });

  // create keback info
  FusedTraversalWritebackInfo *WriteBackInfo =
      new FusedTraversalWritebackInfo();

  SynthesizedFunctions[idName] = WriteBackInfo;

  WriteBackInfo->ParticipatingCalls = ParticipatingCalls;
  WriteBackInfo->FunctionName = idName;

  // create forward declaration for parallel part
  WriteBackInfo->ForwardDeclaration = "void " + idName + "_parallel" + "(";

  // create forward declaration for the serial part
  WriteBackInfo->ForwardDeclaration_serial = "void " + idName + "_serial" + "(";

  // Adding the type of the traversed node as the first argument
  // Actually this should be hmm
  WriteBackInfo->ForwardDeclaration +=
      getHighestCommonTraversedType(TraversalsDeclarationsList)
          ->getNameAsString() +
      "*" + " _r";

  WriteBackInfo->ForwardDeclaration_serial +=
      getHighestCommonTraversedType(TraversalsDeclarationsList)
          ->getNameAsString() +
      "*" + " _r";

  // append the arguments of each method and rename locals  by adding _fx_ only
  // participating traversals
  int Idx = -1;
  for (auto *Decl : TraversalsDeclarationsList) {
    bool First = true;
    Idx++;
    for (auto *Param : Decl->parameters()) {
      if (FunctionsFinder::getFunctionInfo(Decl)->isGlobal() && First) {
        First = false;
        continue;
      }

      WriteBackInfo->ForwardDeclaration +=
          "," + string(Param->getType().getAsString()) + " _f" +
          to_string(Idx) + "_" + Param->getDeclName().getAsString();

      WriteBackInfo->ForwardDeclaration_serial +=
          "," + string(Param->getType().getAsString()) + " _f" +
          to_string(Idx) + "_" + Param->getDeclName().getAsString();
    }
  }

  WriteBackInfo->ForwardDeclaration +=
      ", unsigned int truncate_flags"; // deleted ")"

  WriteBackInfo->ForwardDeclaration_serial += ", unsigned int truncate_flags)";

  // Added this for setting the depth part, introduced variable depth and
  // maxDepth
  /***************************************************************************************************************************************************/
  WriteBackInfo->ForwardDeclaration += ", int depth, int maxDepth)";
  /***************************************************************************************************************************************************/

  string RootCasting = "";
  if (HasCXXCall) {
    for (int i = 0; i < TraversalsDeclarationsList.size(); i++) {
      auto *Decl = TraversalsDeclarationsList[i];
      CXXRecordDecl *CastedToType = nullptr;

      if (FunctionsFinder::getFunctionInfo(Decl)->isGlobal())
        CastedToType = Decl->getParamDecl(0)->getType()->getAsCXXRecordDecl();
      else
        CastedToType = dyn_cast<clang::CXXMethodDecl>(Decl)->getParent();

      RootCasting += CastedToType->getNameAsString() + " *" + string("_r") +
                     "_f" + to_string(i) + " = " + "(" +
                     CastedToType->getNameAsString() + "*)(_r);\n";
    }
  }

  string VisitsCounting =
      "\n#ifdef COUNT_VISITS \n _VISIT_COUNTER++;\n #endif \n";

  WriteBackInfo->Body += VisitsCounting;

  ////WriteBackInfo -> Body += "std::cout << 123 << std::endl;" ;

  WriteBackInfo->Body += RootCasting;

  unordered_map<int, vector<DG_Node *>> StamentsOderedByTId;

  // Topological sort for generating the parallel schedule

  int CurBlockId = 0;
  int flag = 0;
  int addSync = 0;
  int i = 0;
  int vec_size = 0;
  bool tellParr = false;

  for (auto vecNode : TopologicalOrder) {

    i++;
    int j = 1;
    vec_size = 0;

    for (auto *DG_Node : vecNode) {

      if (DG_Node->getStatementInfo()->isCallStmt())
        vec_size++;
    }

    for (auto *DG_Node : vecNode) {
      if (!DG_Node->getStatementInfo()->isCallStmt()) {
        flag = 0;
        StamentsOderedByTId[DG_Node->getTraversalId()].push_back(DG_Node);
        continue;
      }
      // ecnounter a statement
      if (flag == 0) {
        CurBlockId++;
        flag = 1;
        // the block part
        string blockSubPart = "";
        setBlockSubPart(/*Decls,*/ blockSubPart, TraversalsDeclarationsList,
                        CurBlockId, StamentsOderedByTId, HasCXXCall);
        WriteBackInfo->Body += blockSubPart;
      }

      // Label the parallel calls...
      /*****************************************************************************************************************/
      std::string s = std::to_string(j);
      WriteBackInfo->Body += "/*Parallel Call " + s + "*/";
      /*****************************************************************************************************************/

      string CallPartText = "";
      int isParallel;

      // check wheather the current call is not the last call in the function
      if (j < vec_size) {

        isParallel = 1;
        tellParr = true;
      }
      // is not true, its the last call in the function
      else {
        isParallel = 2;
      }
      // increment j, keeps track of how many calls have been written
      j++;

      // write back this call
      this->setCallPart(CallPartText, ParticipatingCalls,
                        TraversalsDeclarationsList, DG_Node, WriteBackInfo,
                        HasCXXCall, isParallel);

      // collect call expression (only for participating traversals)
      WriteBackInfo->Body += CallPartText;

      // flag to make sure the sync part is added to the code
      addSync = 1;

      StamentsOderedByTId.clear();
    }
    // add the cilk sync part only if there are more than 1 calls in the
    // function and add sync is enabled
    if (addSync == 1 && (vec_size > 1)) {
      WriteBackInfo->Body += "cilk_sync;\n";
      addSync = 0;
    }
  }
  CurBlockId++;

  // add the last cilk_sync.
  if (addSync == 1 && tellParr == true) {
    WriteBackInfo->Body += "cilk_sync;\n";
    addSync = 0;
    tellParr = false;
  }

  string blockSubPart = "";

  this->setBlockSubPart(/*Decls, */ blockSubPart, TraversalsDeclarationsList,
                        CurBlockId, StamentsOderedByTId, HasCXXCall);

  WriteBackInfo->Body += blockSubPart;

  std::string CallPartText = "return ;\n";
  // callect call expression (only for participating traversals)
  WriteBackInfo->Body += CallPartText;

  WriteBackInfo->Body += "};\n\n";

  /*********************************************************************************************************************/
  // Prepare forward declaration for serial code

  std::string forward_declaration_serial = "void " + idName + "_serial" + "(";

  // Adding the type of the traversed node as the first argument
  // Actually this should be hmm
  forward_declaration_serial +=
      getHighestCommonTraversedType(TraversalsDeclarationsList)
          ->getNameAsString() +
      "*" + " _r";

  // append the arguments of each method and rename locals  by adding _fx_ only
  // participating traversals
  Idx = -1;
  for (auto *Decl : TraversalsDeclarationsList) {
    bool First = true;
    Idx++;
    for (auto *Param : Decl->parameters()) {
      if (FunctionsFinder::getFunctionInfo(Decl)->isGlobal() && First) {
        First = false;
        continue;
      }

      forward_declaration_serial +=
          "," + string(Param->getType().getAsString()) + " _f" +
          to_string(Idx) + "_" + Param->getDeclName().getAsString();
    }
  }

  forward_declaration_serial += ", unsigned int truncate_flags)";

  /************************************************************************************************************************/
  // end forward declaration for serial code

  WriteBackInfo->Body += forward_declaration_serial;

  WriteBackInfo->Body += "{\n";

  // WriteBackInfo->Body = /* Decls + */ WriteBackInfo->Body;

  // string fullFun = "//****** this fused method is generated by PLCL\n " +
  //                  WriteBackInfo->ForwardDeclaration + "{\n" +
  //                  "//first level declarations\n" + "\n//Body\n" +
  //                  WriteBackInfo->Body + "\n}\n\n";

  /********************************************************************************************************************************
   *
   *
   *
   */
  // ADDED remove from here

  // StatementPrinter Printer;

  // generate the name of the new function
  // idName = createName(ParticipatingCalls, HasVirtual, DerivedType);

  // check if already generated and assert on It
  // commenting for now remove later
  // assert(!isGenerated(ParticipatingCalls, HasVirtual, DerivedType));

  // vector<clang::FunctionDecl *> TraversalsDeclarationsList;
  // TraversalsDeclarationsList.resize(ParticipatingCalls.size());
  // transform(ParticipatingCalls.begin(), ParticipatingCalls.end(),
  // TraversalsDeclarationsList.begin(), [&](clang::CallExpr *CallExpr) {
  //   auto *CalleeDecl =
  //       dyn_cast<clang::FunctionDecl>(CallExpr->getCalleeDecl())
  //           ->getDefinition();

  //   auto *CalleeInfo = FunctionsFinder::getFunctionInfo(CalleeDecl);

  //   if (CalleeInfo->isVirtual())
  //     return (clang::FunctionDecl *)CalleeInfo->getDeclAsCXXMethod()
  //         ->getCorrespondingMethodInClass(DerivedType)
  //         ->getDefinition();
  //   else
  //     return CalleeDecl;
  // });

  // create keback info
  // WriteBackInfo = new FusedTraversalWritebackInfo();

  // SynthesizedFunctions[idName] = WriteBackInfo;

  // WriteBackInfo->ParticipatingCalls = ParticipatingCalls;
  // WriteBackInfo->FunctionName = idName;

  // create forward declaration
  // WriteBackInfo->ForwardDeclaration += "void " + idName + "(";

  // Adding the type of the traversed node as the first argument
  // Actually this should be hmm
  // WriteBackInfo->ForwardDeclaration +=
  //    getHighestCommonTraversedType(TraversalsDeclarationsList)
  //        ->getNameAsString() +
  //    "*" + " _r";

  // append the arguments of each method and rename locals  by adding _fx_ only
  // participating traversals
  // Idx = -1;
  // for (auto *Decl : TraversalsDeclarationsList) {
  //   bool First = true;
  //   Idx++;
  //   for (auto *Param : Decl->parameters()) {
  //     if (FunctionsFinder::getFunctionInfo(Decl)->isGlobal() && First) {
  //       First = false;
  //       continue;
  //     }

  //     WriteBackInfo->ForwardDeclaration +=
  //         "," + string(Param->getType().getAsString()) + " _f" +
  //         to_string(Idx) + "_" + Param->getDeclName().getAsString();
  //   }
  // }

  // WriteBackInfo->ForwardDeclaration += ", unsigned int truncate_flags";
  // //deleted ")"

  // //Added this for setting the depth part, introduced variable depth and
  // maxDepth
  // /**********************************************************************/
  // WriteBackInfo->ForwardDeclaration += ", int depth, int maxDepth)";
  /***********************************************************************/

  RootCasting = "";
  if (HasCXXCall) {
    for (int i = 0; i < TraversalsDeclarationsList.size(); i++) {
      auto *Decl = TraversalsDeclarationsList[i];
      CXXRecordDecl *CastedToType = nullptr;

      if (FunctionsFinder::getFunctionInfo(Decl)->isGlobal())
        CastedToType = Decl->getParamDecl(0)->getType()->getAsCXXRecordDecl();
      else
        CastedToType = dyn_cast<clang::CXXMethodDecl>(Decl)->getParent();

      RootCasting += CastedToType->getNameAsString() + " *" + string("_r") +
                     "_f" + to_string(i) + " = " + "(" +
                     CastedToType->getNameAsString() + "*)(_r);\n";
    }
  }

  VisitsCounting = "\n#ifdef COUNT_VISITS \n _VISIT_COUNTER++;\n #endif \n";

  WriteBackInfo->Body += VisitsCounting;
  WriteBackInfo->Body += RootCasting;

  // clear the statements in the vector, we are about to generate code for all
  // the serial part
  StamentsOderedByTId.clear();

  CurBlockId = 0;
  flag = 0;
  addSync = 0;
  i = 0;
  for (auto vecNode : TopologicalOrder) {
    i++;
    for (auto *DG_Node : vecNode) {
      if (!DG_Node->getStatementInfo()->isCallStmt()) {
        flag = 0;
        StamentsOderedByTId[DG_Node->getTraversalId()].push_back(DG_Node);
        continue;
      }
      // ecnounter a statement
      if (flag == 0) {
        CurBlockId++;
        flag = 1;
        // the block part
        // WriteBackInfo->Body += "//block " + to_string(CurBlockId) + "\n";
        blockSubPart = "";
        setBlockSubPart(/*Decls,*/ blockSubPart, TraversalsDeclarationsList,
                        CurBlockId, StamentsOderedByTId, HasCXXCall);
        WriteBackInfo->Body += blockSubPart;
      }

      WriteBackInfo->Body += "/*Serial Call*/";

      CallPartText = "";
      // write the call part set isParallel to 3 which indicates its for the
      // serial code.
      this->setCallPart(CallPartText, ParticipatingCalls,
                        TraversalsDeclarationsList, DG_Node, WriteBackInfo,
                        HasCXXCall, 3);
      // collect call expression (only for participating traversals)
      WriteBackInfo->Body += CallPartText;
      addSync = 1;

      StamentsOderedByTId.clear();
    }
    if (addSync == 1) {
      addSync = 0;
    }
  }
  CurBlockId++; // till here

  if (addSync == 1) {
    addSync = 0;
  }

  blockSubPart = "";

  this->setBlockSubPart(/*Decls, */ blockSubPart, TraversalsDeclarationsList,
                        CurBlockId, StamentsOderedByTId, HasCXXCall);

  WriteBackInfo->Body += blockSubPart;

  CallPartText = "return ;\n";
  // callect call expression (only for participating traversals)
  WriteBackInfo->Body += CallPartText;
  WriteBackInfo->Body = /* Decls + */ WriteBackInfo->Body;

  ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
  ///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////Till
  ///here
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
/***********************************************************************************

Trying to make a seperate write back info code for non parallel code.
*/
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////added
///below

// void TraversalSynthesizer::generateWriteBackInfo_serial(
//     const std::vector<clang::CallExpr *> &ParticipatingCalls,
//     const std::vector<vector<DG_Node *>> &TopologicalOrder, bool HasVirtual,
//     bool HasCXXCall, const CXXRecordDecl *DerivedType) {

//   StatementPrinter Printer;

//   // generate the name of the new function
//   string idName = createName(ParticipatingCalls, HasVirtual, DerivedType);

//   // check if already generated and assert on It
//   //commenting for now remove later
//   //assert(!isGenerated(ParticipatingCalls, HasVirtual, DerivedType));

//   vector<clang::FunctionDecl *> TraversalsDeclarationsList;
//   TraversalsDeclarationsList.resize(ParticipatingCalls.size());
//   transform(ParticipatingCalls.begin(), ParticipatingCalls.end(),
//             TraversalsDeclarationsList.begin(), [&](clang::CallExpr
//             *CallExpr) {
//               auto *CalleeDecl =
//                   dyn_cast<clang::FunctionDecl>(CallExpr->getCalleeDecl())
//                       ->getDefinition();

//               auto *CalleeInfo =
//               FunctionsFinder::getFunctionInfo(CalleeDecl);

//               if (CalleeInfo->isVirtual())
//                 return (clang::FunctionDecl
//                 *)CalleeInfo->getDeclAsCXXMethod()
//                     ->getCorrespondingMethodInClass(DerivedType)
//                     ->getDefinition();
//               else
//                 return CalleeDecl;
//             });

//   // create keback info
//   FusedTraversalWritebackInfo *WriteBackInfo =
//       new FusedTraversalWritebackInfo();

//   SynthesizedFunctions[idName] = WriteBackInfo;

//   WriteBackInfo->ParticipatingCalls = ParticipatingCalls;
//   WriteBackInfo->FunctionName = idName ;

//   // create forward declaration
//   WriteBackInfo->ForwardDeclaration = "void " + idName + "_serial" +  "(";

//   // Adding the type of the traversed node as the first argument
//   // Actually this should be hmm
//   WriteBackInfo->ForwardDeclaration +=
//       getHighestCommonTraversedType(TraversalsDeclarationsList)
//           ->getNameAsString() +
//       "*" + " _r";

//   // append the arguments of each method and rename locals  by adding _fx_
//   only
//   // participating traversals
//   int Idx = -1;
//   for (auto *Decl : TraversalsDeclarationsList) {
//     bool First = true;
//     Idx++;
//     for (auto *Param : Decl->parameters()) {
//       if (FunctionsFinder::getFunctionInfo(Decl)->isGlobal() && First) {
//         First = false;
//         continue;
//       }

//       WriteBackInfo->ForwardDeclaration +=
//           "," + string(Param->getType().getAsString()) + " _f" +
//           to_string(Idx) + "_" + Param->getDeclName().getAsString();
//     }
//   }

//   WriteBackInfo->ForwardDeclaration += ", unsigned int truncate_flags";
//   //deleted ")"

//   //Added this for setting the depth part, introduced variable depth and
//   maxDepth
//   /**********************************************************************/
//   WriteBackInfo->ForwardDeclaration += ", int depth, int maxDepth)";
//   /***********************************************************************/

//   string RootCasting = "";
//   if (HasCXXCall) {
//     for (int i = 0; i < TraversalsDeclarationsList.size(); i++) {
//       auto *Decl = TraversalsDeclarationsList[i];
//       CXXRecordDecl *CastedToType = nullptr;

//       if (FunctionsFinder::getFunctionInfo(Decl)->isGlobal())
//         CastedToType =
//         Decl->getParamDecl(0)->getType()->getAsCXXRecordDecl();
//       else
//         CastedToType = dyn_cast<clang::CXXMethodDecl>(Decl)->getParent();

//       RootCasting += CastedToType->getNameAsString() + " *" + string("_r") +
//                      "_f" + to_string(i) + " = " + "(" +
//                      CastedToType->getNameAsString() + "*)(_r);\n";
//     }
//   }

//   string VisitsCounting =
//       "\n#ifdef COUNT_VISITS \n _VISIT_COUNTER++;\n #endif \n";

//   WriteBackInfo->Body += VisitsCounting;
//   WriteBackInfo->Body += RootCasting;

//   unordered_map<int, vector<DG_Node *>> StamentsOderedByTId;

//   int CurBlockId = 0;
//   int flag = 0;                                             //added flag
//   variable int addSync = 0; int i = 0; for (auto vecNode : TopologicalOrder)
//   {                   //added for vec of vec implementation
//     i++;
//     for (auto *DG_Node : vecNode) {
//       if (!DG_Node->getStatementInfo()->isCallStmt()) {
//         flag = 0;
//         StamentsOderedByTId[DG_Node->getTraversalId()].push_back(DG_Node);
//         continue;
//       }
//        //ecnounter a statement
//        if(flag == 0){                                              //added
//          CurBlockId++;
//          flag = 1;                             //added flag for looking at
//          calls parallely
//          //the block part
//          //WriteBackInfo->Body += "//block " + to_string(CurBlockId) + "\n";
//          string blockSubPart = "";
//          setBlockSubPart(/*Decls,*/ blockSubPart,
//          TraversalsDeclarationsList,CurBlockId, StamentsOderedByTId,
//          HasCXXCall); WriteBackInfo->Body +=  blockSubPart;
//      }

//       //Added this just for info of the parallel calls...
//       /******************************************************************************/
//       std::string s = std::to_string(i);
//       WriteBackInfo->Body += "/*Parallel Call " + s +  "*/";
//       /**********************************************************************************/

//       //WriteBackInfo->Body +=  "cilk_spawn [=]{"; //added a silk spawn and
//       created each block as a lambda function string CallPartText = "";
//       this->setCallPart(CallPartText, ParticipatingCalls,
//                         TraversalsDeclarationsList, DG_Node, WriteBackInfo,
//                         HasCXXCall);
//       // callect call expression (only for participating traversals)
//       WriteBackInfo->Body += CallPartText;
//       //WriteBackInfo->Body += "}();"; //lambda function ends here addSync =
//       1;

//        StamentsOderedByTId.clear();

//     }
//     if(addSync == 1){
//       WriteBackInfo->Body +=  "cilk_sync;\n";
//       addSync = 0;
//     }
//   }
//   CurBlockId++; // till here
//   //WriteBackInfo->Body += "//block " + to_string(CurBlockId) + "\n";
//   if(addSync == 1){
//       WriteBackInfo->Body +=  "cilk_sync;\n";
//       addSync = 0;
//   }

//   string blockSubPart = "";

//   this->setBlockSubPart(/*Decls, */ blockSubPart, TraversalsDeclarationsList,
//                         CurBlockId, StamentsOderedByTId, HasCXXCall);

//   WriteBackInfo->Body += blockSubPart;

//   std::string CallPartText = "return ;\n";
//   // callect call expression (only for participating traversals)
//   WriteBackInfo->Body += CallPartText;
//   WriteBackInfo->Body = /* Decls + */ WriteBackInfo->Body;

//   // string fullFun = "//****** this fused method is generated by PLCL\n " +
//   //                  WriteBackInfo->ForwardDeclaration + "{\n" +
//   //                  "//first level declarations\n" + "\n//Body\n" +
//   //                  WriteBackInfo->Body + "\n}\n\n";
// }

//////////////////////////////////////////////////////////////////////////////////////////////////////added
///extra till here

extern AccessPath extractVisitedChild(clang::CallExpr *Call);

void TraversalSynthesizer::WriteUpdates(
    const std::vector<clang::CallExpr *> CallsExpressions,
    clang::FunctionDecl *EnclosingFunctionDecl) {

  for (auto *CallExpr : CallsExpressions)
    Rewriter.InsertText(CallExpr->getBeginLoc(), "//");

  // add forward declarations
  for (auto &SynthesizedFunction : SynthesizedFunctions) {
    Rewriter.InsertText(
        EnclosingFunctionDecl->getTypeSourceInfo()->getTypeLoc().getBeginLoc(),
        (SynthesizedFunction.second->ForwardDeclaration) + string(";\n"));
  }

  for (auto &SynthesizedFunction : SynthesizedFunctions) {
    Rewriter.InsertText(
        EnclosingFunctionDecl->getTypeSourceInfo()->getTypeLoc().getBeginLoc(),
        (SynthesizedFunction.second->ForwardDeclaration_serial) +
            string(";\n"));
  }

  static std::set<string> InsertedFunctions;

  for (auto &SynthesizedFunction : SynthesizedFunctions) {
    if(InsertedFunctions.count(SynthesizedFunction.second->FunctionName))
    continue;
    else 
    InsertedFunctions.insert(SynthesizedFunction.second->FunctionName);
    Rewriter.InsertText(
        EnclosingFunctionDecl->getTypeSourceInfo()->getTypeLoc().getBeginLoc(),
        (SynthesizedFunction.second->ForwardDeclaration + "\n{\n" +
         SynthesizedFunction.second->Body + "\n};\n"));
  }

  for (auto &SynthesizedFunction : SynthesizedFunctions) {
    if (InsertedFunctions.count(SynthesizedFunction.second->FunctionName))
      continue;
    else
      InsertedFunctions.insert(SynthesizedFunction.second->FunctionName);
    Rewriter.InsertText(
        EnclosingFunctionDecl->getTypeSourceInfo()->getTypeLoc().getBeginLoc(),
        (SynthesizedFunction.second->ForwardDeclaration_serial + "\n{\n" +
         SynthesizedFunction.second->Body + "\n};\n"));
  }

  StatementPrinter Printer;
  // 2-build the new function call and add It.
  bool IsMemberCall = CallsExpressions[0]->getStmtClass() ==
                      clang::Stmt::CXXMemberCallExprClass;

  bool HasVirtual = false;
  bool HasCXXMethod = false;

  for (auto *Call : CallsExpressions) {
    auto *CalleeInfo = FunctionsFinder::getFunctionInfo(
        Call->getCalleeDecl()->getAsFunction()->getDefinition());
    if (CalleeInfo->isCXXMember())
      HasCXXMethod = true;
    if (CalleeInfo->isVirtual())
      HasVirtual = true;
  }
  std::string NextCallName;
  if (!HasVirtual)
    NextCallName = createName(CallsExpressions, false, nullptr);
  else
    NextCallName = getVirtualStub(CallsExpressions);

  string NewCall = "";

  NewCall += "\n\tint startDepth = 0;\n\t";

  NewCall += "\n\tint maximumDepth = 1024;\n\t";

  NewCall += "\n\tif (startDepth < maximumDepth)";
  NewCall += " {";
  NewCall += "\n";

  string Params = "";

  if (!HasVirtual) {
    NewCall += NextCallName + "_parallel" + "(";

    if (CallsExpressions[0]->getStmtClass() == clang::Stmt::CallExprClass) {
      auto FirstArgument =
          dyn_cast<clang::CallExpr>(CallsExpressions[0])->getArg(0);
      Params += Printer.printStmt(FirstArgument, ASTCtx->getSourceManager(),
                                  nullptr, "", -1);
    } else if (CallsExpressions[0]->getStmtClass() ==
               clang::Stmt::CXXMemberCallExprClass) {
      Params += Printer.printStmt(
          CallsExpressions[0]->child_begin()->child_begin()->IgnoreImplicit(),
          ASTCtx->getSourceManager(), nullptr, "", -1, false);
    }

  } else {
    if (CallsExpressions[0]->getStmtClass() ==
        clang::Stmt::CXXMemberCallExprClass) {
      NewCall += Printer.printStmt(CallsExpressions[0]
                                       ->child_begin()
                                       ->child_begin()
                                       ->IgnoreImplicit(),
                                   ASTCtx->getSourceManager(), nullptr, "", -1,
                                   false) +
                 "->" + NextCallName + "_parallel" + "(";
    } else if (CallsExpressions[0]->getStmtClass() ==
               clang::Stmt::CallExprClass) {

      auto FirstArgument =
          dyn_cast<clang::CallExpr>(CallsExpressions[0])->getArg(0);

      NewCall += NextCallName + "(";
      Params += Printer.printStmt(FirstArgument, ASTCtx->getSourceManager(),
                                  nullptr, "", -1);
    } else {
      llvm_unreachable("unexpected");
    }
  }

  // 3-append arguments of all methods in the same oƒrder and build default
  // params
  // hack
  for (auto *CallExpr : CallsExpressions) {

    for (int ArgIdx =
             CallExpr->getCalleeDecl()->getAsFunction()->isGlobal() ? 1 : 0;
         ArgIdx < CallExpr->getNumArgs(); ArgIdx++) {
      Params += ((Params.size() == 0) ? "" : ", ") +
                Printer.stmtTostr(CallExpr->getArg(ArgIdx),
                                  ASTCtx->getSourceManager());
    }
  }

  // add initial truncate flags
  unsigned int x = 0;
  for (int i = 0; i < CallsExpressions.size(); i++)
    x |= (1 << i);

  // added ", 0, 1024" here to control the depth part 0 means starting depth and
  // 1024 is max depth

  Params += ((Params.size() == 0) ? "" : ", ") + toBinaryString(x) +
            ", startDepth, maximumDepth" + ");";
  /**********************************************************************************************/

  NewCall += Params;
  NewCall += "\n\t}";
  NewCall += "\n\telse{\n\t";

  // add a call for the serial part also
  Params = "";
  if (!HasVirtual) {
    NewCall += NextCallName + "_serial" + "(";

    if (CallsExpressions[0]->getStmtClass() == clang::Stmt::CallExprClass) {
      auto FirstArgument =
          dyn_cast<clang::CallExpr>(CallsExpressions[0])->getArg(0);
      Params += Printer.printStmt(FirstArgument, ASTCtx->getSourceManager(),
                                  nullptr, "", -1);
    } else if (CallsExpressions[0]->getStmtClass() ==
               clang::Stmt::CXXMemberCallExprClass) {
      Params += Printer.printStmt(
          CallsExpressions[0]->child_begin()->child_begin()->IgnoreImplicit(),
          ASTCtx->getSourceManager(), nullptr, "", -1, false);
    }

  } else {
    if (CallsExpressions[0]->getStmtClass() ==
        clang::Stmt::CXXMemberCallExprClass) {
      NewCall += Printer.printStmt(CallsExpressions[0]
                                       ->child_begin()
                                       ->child_begin()
                                       ->IgnoreImplicit(),
                                   ASTCtx->getSourceManager(), nullptr, "", -1,
                                   false) +
                 "->" + NextCallName + "_serial" + "(";
    } else if (CallsExpressions[0]->getStmtClass() ==
               clang::Stmt::CallExprClass) {

      auto FirstArgument =
          dyn_cast<clang::CallExpr>(CallsExpressions[0])->getArg(0);

      NewCall += NextCallName + "(";
      Params += Printer.printStmt(FirstArgument, ASTCtx->getSourceManager(),
                                  nullptr, "", -1);
    } else {
      llvm_unreachable("unexpected");
    }
  }

  // 3-append arguments of all methods in the same oƒrder and build default
  // params
  // hack
  for (auto *CallExpr : CallsExpressions) {

    for (int ArgIdx =
             CallExpr->getCalleeDecl()->getAsFunction()->isGlobal() ? 1 : 0;
         ArgIdx < CallExpr->getNumArgs(); ArgIdx++) {
      Params += ((Params.size() == 0) ? "" : ", ") +
                Printer.stmtTostr(CallExpr->getArg(ArgIdx),
                                  ASTCtx->getSourceManager());
    }
  }

  // add initial truncate flags
  x = 0;
  for (int i = 0; i < CallsExpressions.size(); i++)
    x |= (1 << i);

  // added ", 0, 1024" here to control the depth part 0 means starting depth and
  // 1024 is max depth

  Params += ((Params.size() == 0) ? "" : ", ") + toBinaryString(x) + ");";
  /**********************************************************************************************/

  NewCall += Params;
  NewCall += "\n\t}";

  Rewriter.InsertTextAfter(
      Lexer::findLocationAfterToken(
          CallsExpressions[CallsExpressions.size() - 1]->getLocEnd(),
          tok::TokenKind::semi, ASTCtx->getSourceManager(),
          ASTCtx->getLangOpts(), true),
      "\n\t//added by fuse transformer \n\t" + NewCall + "\n");

  // add virtual stubs

  for (auto &Entry : Stubs) {
    auto &Calls = Entry.first;
    auto &StubName = Entry.second;

    AccessPath AP = extractVisitedChild(Calls[0]);

    auto *CalledChildType = AP.getDeclAtIndex(AP.SplittedAccessPath.size() - 1)
                                ->getType()
                                ->getPointeeCXXRecordDecl();

    vector<clang::FunctionDecl *> TraversalsDeclarationsList;
    TraversalsDeclarationsList.resize(Calls.size());
    transform(Calls.begin(), Calls.end(), TraversalsDeclarationsList.begin(),
              [&](clang::CallExpr *CallExpr) {
                auto *CalleeDecl =
                    dyn_cast<clang::FunctionDecl>(CallExpr->getCalleeDecl())
                        ->getDefinition();

                auto *CalleeInfo = FunctionsFinder::getFunctionInfo(CalleeDecl);

                if (CalleeInfo->isVirtual())
                  return (clang::FunctionDecl *)CalleeInfo->getDeclAsCXXMethod()
                      ->getDefinition();
                else
                  return CalleeDecl;
              });

    // append the arguments of each method and rename locals  by adding _fx_
    // only participating traversals

    string Params_serial = "";

    string Args_serial = "";

    string Params = "";
    string Args = "this";

    int Idx = -1;
    for (auto *Decl : TraversalsDeclarationsList) {
      bool First = true;
      Idx++;
      for (auto *Param : Decl->parameters()) {
        if (FunctionsFinder::getFunctionInfo(Decl)->isGlobal() && First) {
          First = false;
          continue;
        }

        Params += (Params == "" ? "" : ", ") +
                  string(Param->getType().getAsString()) + " _f" +
                  to_string(Idx) + "_" + Param->getDeclName().getAsString();
        Args += (Args == "" ? "" : ", ") + string(" _f") + to_string(Idx) +
                "_" + Param->getDeclName().getAsString();
      }
    }

    Params +=
        (Params == "" ? "" : ", ") + string("unsigned int truncate_flags");

    Params_serial = Params;

    // Added code here to implement the depth part
    // Introduced depth and maxDepth variables
    Params += string(", int depth, int maxDepth");
    /**********************************************/

    Args += ", truncate_flags";

    // copy args to serial args as this is all that is needed for the serial
    // code
    Args_serial = Args;

    // passing the depth and maxDepth variables to the fused functions
    Args += ", depth";
    Args += ", maxDepth";
    /*******************************************************************/

    auto LambdaFun = [&](const CXXRecordDecl *DerivedType) {
      if (InsertedStubs[DerivedType].count(Entry.second))
        return;
      else
        InsertedStubs[DerivedType].insert(Entry.second);

      assert(Rewriter::isRewritable(DerivedType->getLocEnd()));
      Rewriter.InsertText(
          DerivedType->getDefinition()->getLocEnd(),
          // parallel stub
          (DerivedType == CalledChildType ? "virtual" : "") + string(" void ") +
              StubName + "_parallel" + "(" + Params + ")" +
              (DerivedType == CalledChildType ? "" : "override") + ";\n" +

              // serial stub
              (DerivedType == CalledChildType ? "virtual" : "") +
              string(" void ") + StubName + "_serial" + "(" + Params_serial +
              ")" + (DerivedType == CalledChildType ? "" : "override") + ";\n"

      );

      Rewriter.InsertTextAfter(
          EnclosingFunctionDecl->getAsFunction()
              ->getDefinition()
              ->getTypeSourceInfo()
              ->getTypeLoc()
              .getBeginLoc(),

          // Create the parallel virtual stub
          "void " + DerivedType->getNameAsString() + "::" + StubName +
              "_parallel" + "(" + Params + "){" +

              // create the code for the parallel code
              createName(Calls, true, DerivedType) + "_parallel" + "(" + Args +
              ");" + "}\n\n\n" +

              // create the code for the serial call
              "void " + DerivedType->getNameAsString() + "::" + StubName +
              "_serial" + "(" + Params_serial + "){" +

              // create the code for the serial call
              createName(Calls, true, DerivedType) + "_serial" + "(" +
              Args_serial + ");"

              + "}\n\n\n");

      return;
    };
    LambdaFun(CalledChildType);
    for (auto *DerivedType : RecordsAnalyzer::DerivedRecords[CalledChildType]) {
      LambdaFun(DerivedType);
    }
  }
}

void StatementPrinter::print_handleStmt(const clang::Stmt *Stmt,
                                        SourceManager &SM) {
  Stmt = Stmt->IgnoreImplicit();
  switch (Stmt->getStmtClass()) {

  case Stmt::CompoundStmtClass: {
    for (auto *ChildStmt : Stmt->children())
      print_handleStmt(ChildStmt, SM);
    break;
  }
  case Stmt::CallExprClass: {
    auto *CallExpr = dyn_cast<clang::CallExpr>(Stmt);
    Output += CallExpr->getDirectCallee()->getNameAsString() + "(";

    if (CallExpr->getNumArgs()) {
      auto *LastArgument = CallExpr->getArg(CallExpr->getNumArgs() - 1);
      for (auto *Argument : CallExpr->arguments()) {
        print_handleStmt(Argument, SM);
        if (Argument != LastArgument)
          Output += ",";
      }
    }
    Output += ")";

    if (NestedExpressionDepth == 0)
      Output += ";";
    break;
  }
  case Stmt::BinaryOperatorClass: {
    // print the lhs ,
    NestedExpressionDepth++;
    auto *BinaryOperator = dyn_cast<clang::BinaryOperator>(Stmt);

    if (BinaryOperator->isAssignmentOp())
      Output += "\t";
    print_handleStmt(BinaryOperator->getLHS(), SM);

    // print op
    Output += BinaryOperator->getOpcodeStr();

    // print lhs
    print_handleStmt(BinaryOperator->getRHS(), SM);
    if (BinaryOperator->isAssignmentOp())
      Output += ";\n";

    NestedExpressionDepth--;
    break;
  }
  case Stmt::DeclRefExprClass: {
    auto *DeclRefExpr = dyn_cast<clang::DeclRefExpr>(Stmt);
    if (DeclRefExpr->getDecl() == this->RootNodeDecl)
      Output +=
          RootCasedPerTraversals ? "_r_f" + to_string(TraversalIndex) : "_r";
    else if (!DeclRefExpr->getDecl()->isDefinedOutsideFunctionOrMethod())
      // only change prefix local decl with _f(TID)_ if TID>= 0
      Output += TraversalIndex >= 0
                    ? "_f" + to_string(this->TraversalIndex) + "_" +
                          DeclRefExpr->getDecl()->getNameAsString()
                    : DeclRefExpr->getDecl()->getNameAsString();
    else
      Output += stmtTostr(Stmt, SM);
    break;
  }
  case Stmt::ImplicitCastExprClass: {
    Output += "|>";
    print_handleStmt(dyn_cast<clang::ImplicitCastExpr>(Stmt)->getSubExpr(), SM);
    break;
  }
  case Stmt::MemberExprClass: {
    auto *MemberExpression = dyn_cast<clang::MemberExpr>(Stmt);
    print_handleStmt(*MemberExpression->child_begin(), SM);

    if (MemberExpression->isArrow())
      Output += "->" + MemberExpression->getMemberDecl()->getNameAsString();
    else
      Output += "." + MemberExpression->getMemberDecl()->getNameAsString();

    break;
  }
  case Stmt::IfStmtClass: {
    auto &IfStmt = *dyn_cast<clang::IfStmt>(Stmt);
    // check the condition part first
    if (IfStmt.getCond() != nullptr) {
      NestedExpressionDepth++;
      Output += "\t if (";
      auto *IfStmtCondition = IfStmt.getCond()->IgnoreImplicit();
      print_handleStmt(IfStmtCondition, SM);
      Output += ")";
      NestedExpressionDepth--;
    }

    if (IfStmt.getThen() != nullptr) {
      auto *IfStmtThenPart = IfStmt.getThen()->IgnoreImplicit();
      Output += "{\n";
      print_handleStmt(IfStmtThenPart, SM);
      Output += "\t}";
    } else {
      Output += "{}\n";
    }

    if (IfStmt.getElse() != nullptr) {
      auto *IfStmtElsePart = IfStmt.getElse()->IgnoreImplicit();
      Output += "else {\n\t";
      print_handleStmt(IfStmtElsePart, SM);
      Output += "\n\t}";
    } else {
      Output += "\n";
    }
    break;
  }
  case Stmt::ReturnStmtClass: {
    unsigned int x = 0;
    for (int i = 0; i < (TraversalsCount + 1); i++)
      x |= (1 << i);
    Output += "\t truncate_flags&=" +
              toBinaryString(x & (~(unsigned int)(1 << (TraversalIndex)))) +
              "; goto " + NextLabel + " ;\n";

    break;
  }
  case Stmt::Stmt::NullStmtClass:
    Output += "\t\t;\n";
    break;
  case Stmt::DeclStmtClass: {
    auto *DeclStmt = dyn_cast<clang::DeclStmt>(Stmt);
    for (auto *Decl : DeclStmt->decls()) {
      auto *VarDecl = dyn_cast<clang::VarDecl>(Decl);
      assert(VarDecl != nullptr);
      Output += "\t" + VarDecl->getType().getAsString() + " " + "_f" +
                to_string(this->TraversalIndex) + "_" +
                VarDecl->getNameAsString() + " ";
      if (VarDecl->getInit() != nullptr) {
        Output += "=";
        print_handleStmt(VarDecl->getInit(), SM);
        Output += ";\n";
      } else
        Output += ";\n";
    }
    break;
  }
  case Stmt::ParenExprClass: {
    auto *ParenExpr = dyn_cast<clang::ParenExpr>(Stmt);
    Output += "(";
    print_handleStmt(ParenExpr->getSubExpr(), SM);
    Output += ")";
  } break;
  case Stmt::CXXMemberCallExprClass: {
    auto *CallExpr = dyn_cast<clang::CXXMemberCallExpr>(Stmt);
    print_handleStmt(*(CallExpr->child_begin())->child_begin(), SM);
    Output += "->" +
              CallExpr->getCalleeDecl()->getAsFunction()->getNameAsString() +
              "(";
    if (CallExpr->getNumArgs()) {
      auto *LastArgument = CallExpr->getArg(CallExpr->getNumArgs() - 1);
      for (auto *Argument : CallExpr->arguments()) {
        print_handleStmt(Argument, SM);
        if (Argument != LastArgument)
          Output += ",";
      }
    }
    Output += ")";
    if (NestedExpressionDepth == 0)
      Output += ";";
    break;
  }
  case Stmt::CXXStaticCastExprClass: {
    auto *CastStmt = dyn_cast<CXXStaticCastExpr>(Stmt);
    Output +=
        "static_cast<" + CastStmt->getTypeAsWritten().getAsString() + ">(";
    print_handleStmt(CastStmt->getSubExpr(), SM);
    Output += ")";
    break;
  }
  case Stmt::CXXDeleteExprClass: {
    auto *DeleteArgument = dyn_cast<CXXDeleteExpr>(Stmt)->getArgument();
    Output += "delete ";
    print_handleStmt(DeleteArgument, SM);
    Output += ";";
    break;
  }
  case Stmt::CXXThisExprClass: {

    if (!ReplaceThis)
      Output += "this";
    else
      Output +=
          RootCasedPerTraversals ? "_r_f" + to_string(TraversalIndex) : "_r";
    break;
  }
  case Stmt::UnaryOperatorClass: {
    auto *UnaryOpExp = dyn_cast<clang::UnaryOperator>(Stmt);
    Output += UnaryOpExp->getOpcodeStr(UnaryOpExp->getOpcode()).str();
    print_handleStmt(UnaryOpExp->getSubExpr(), SM);
    break;
  }
  case Stmt::IntegerLiteralClass: {
    auto *IntegerLit = dyn_cast<clang::IntegerLiteral>(Stmt);
    Output += IntegerLit->getValue().toString(10, true);
    break;
  }
  case Stmt::CXXConstructExprClass: {
    auto *ConstructExpr = dyn_cast<clang::CXXConstructExpr>(Stmt);
    if (ConstructExpr->getNumArgs())
      print_handleStmt(ConstructExpr->getArg(0), SM);
    break;
  }
  default:
    LLVM_DEBUG(Stmt->dump());
    Output += stmtTostr(Stmt, SM);
  }
  return;
}