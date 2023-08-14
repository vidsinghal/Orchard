//===--- FuseTransformation.h ---------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//===----------------------------------------------------------------------===//

#ifndef TREE_FUSER_FUSE_TRANSFORMATION
#define TREE_FUSER_FUSE_TRANSFORMATION

#include "AccessPath.h"
#include "DependenceAnalyzer.h"
#include "FunctionAnalyzer.h"
#include "FunctionsFinder.h"
#include "LLVMDependencies.h"
#include <TraversalSynthesizer.h>
#include <set>
#include <stdio.h>
#include <unordered_map>
#include <vector>

using namespace std;

typedef std::unordered_map<clang::FunctionDecl *,
                           std::vector<std::vector<clang::CallExpr *>>>
    CandidatesList;

class FusionCandidatesFinder
    : public RecursiveASTVisitor<FusionCandidatesFinder> {
private:
  ASTContext *Ctx;

  /// Refers to the currently traversed function
  clang::FunctionDecl *CurrentFuncDecl;

  /// Store for candidates of fusion
  CandidatesList FusionCandidates;

  /// Analyzed information for the functions within the same Ctx
  FunctionsFinder *FunctionsInformation;

  /// Return true if two calls traverse the same tree from the same node
  bool areCompatibleCalls(clang::CallExpr *Call1, clang::CallExpr *Call2);

public:
  /// Search the source code for valid fusion candidates
  void findCandidates() { this->TraverseDecl(Ctx->getTranslationUnitDecl()); }

  /// Return list of fusion candidates
  CandidatesList &getFusionCandidates() { return FusionCandidates; }

  FusionCandidatesFinder(ASTContext *Ctx, FunctionsFinder *FunctionsInfo) {
    this->Ctx = Ctx;
    this->FunctionsInformation = FunctionsInfo;
  }

  bool VisitCompoundStmt(const clang::CompoundStmt *CompoundStmt);

  bool VisitFunctionDecl(clang::FunctionDecl *FunctionDec);
};
class TraversalSynthesizer;
class FusionTransformer {
private:
  static clang::Rewriter Rewriter;
  FunctionsFinder *FunctionsInformation;
  ASTContext *Ctx;
  static DependenceAnalyzer DepAnalyzer;
  static TraversalSynthesizer *Synthesizer;

public:
  std::string Heuristic;

  /// Perform fusion transformation on a given list of candidates
  void performFusion(const vector<clang::CallExpr *> &Candidate,
                     bool IsTopLevel,
                     clang::FunctionDecl *EnclosingFunctionDecl
                     /*just needed for top level*/,
                     std::string Heuristic);

  /// Commiting source code updates to the source files
  void overwriteChangedFiles() { Rewriter.overwriteChangedFiles(); }

  void performGreedyFusion(DependenceGraph *DepGraph);

  // vector<DG_Node *> findToplogicalOrder(DependenceGraph *DepGraph);

  /*void findToplogicalOrderRec(vector<DG_Node *> &topOrder,
                              unordered_map<DG_Node *, bool> &visited,
                              DG_Node *node);
  */

  // The algorithm for topologically sorting the dependence graph for
  // parallelism
  vector<vector<DG_Node *>> parallelSchedule(DependenceGraph *DepGraph);

  bool unfusableCallsExist(DG_Node *function1, DG_Node *function2,
                           DependenceGraph *DepGraph);

  FusionTransformer(ASTContext *Ctx, FunctionsFinder *FunctionsInfo,
                    std::string Heuristic);
};

#endif