//===--- FuseTransformation.cpp -------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//===----------------------------------------------------------------------===//

#include "FuseTransformation.h"
#include "DependenceAnalyzer.h"
#include "DependenceGraph.h"
#include <algorithm>

extern llvm::cl::OptionCategory TreeFuserCategory;
namespace opts {
llvm::cl::opt<unsigned>
    MaxMergedInstances("max-merged-f",
                       cl::desc("a maximum number of calls for the same "
                                "function that can be fused together"),
                       cl::init(5), cl::ZeroOrMore, cl::cat(TreeFuserCategory));
llvm::cl::opt<unsigned>
    MaxMergedNodes("max-merged-n",
                   cl::desc("a maximum number of  that can be fused together"),
                   cl::init(5), cl::ZeroOrMore, cl::cat(TreeFuserCategory));
} // namespace opts

bool FusionCandidatesFinder::VisitFunctionDecl(clang::FunctionDecl *FuncDecl) {
  CurrentFuncDecl = FuncDecl;
  return true;
}

clang::Rewriter FusionTransformer::Rewriter = clang::Rewriter();
DependenceAnalyzer FusionTransformer::DepAnalyzer = DependenceAnalyzer();
TraversalSynthesizer *FusionTransformer::Synthesizer = nullptr;

bool FusionCandidatesFinder::VisitCompoundStmt(
    const CompoundStmt *CompoundStmt) {

  std::vector<clang::CallExpr *> Candidate;

  for (auto *InnerStmt : CompoundStmt->body()) {

    if (InnerStmt->getStmtClass() != Stmt::CallExprClass &&
        InnerStmt->getStmtClass() != Stmt::CXXMemberCallExprClass) {

      if (Candidate.size() > 1)
        FusionCandidates[CurrentFuncDecl].push_back(Candidate);

      Candidate.clear();
      continue;
    }

    auto *CurrentCallStmt = dyn_cast<clang::CallExpr>(InnerStmt);
    if (Candidate.size() == 0) {
      if (areCompatibleCalls(CurrentCallStmt, CurrentCallStmt)) {
        Candidate.push_back(CurrentCallStmt);
      }
      continue;
    }

    if (areCompatibleCalls(Candidate[0], CurrentCallStmt)) {
      Candidate.push_back(CurrentCallStmt);
    } else {
      if (Candidate.size() > 1) //==1
        FusionCandidates[CurrentFuncDecl].push_back(Candidate);

      Candidate.clear();
    }
  }

  if (Candidate.size() > 1)
    FusionCandidates[CurrentFuncDecl].push_back(Candidate);

  Candidate.clear();
  return true;
}

AccessPath extractVisitedChild(clang::CallExpr *Call) {
  if (Call->getStmtClass() == clang::Stmt::CXXMemberCallExprClass) {
    auto *ExprCallRemoved =
        Call->child_begin()->child_begin()->IgnoreImplicit();

    return AccessPath(dyn_cast<clang::Expr>(ExprCallRemoved), nullptr);

  } else {
    return AccessPath(Call->getArg(0), nullptr);
  }
}

bool FusionCandidatesFinder::areCompatibleCalls(clang::CallExpr *Call1,
                                                clang::CallExpr *Call2) {

  if (Call1->getCalleeDecl() == nullptr || Call2->getCallee() == nullptr)
    return false;

  auto *Decl1 = dyn_cast<clang::FunctionDecl>(Call1->getCalleeDecl());
  auto *Decl2 = dyn_cast<clang::FunctionDecl>(Call2->getCalleeDecl());

  if (!FunctionsInformation->isValidFuse(Decl1->getDefinition()) ||
      !FunctionsInformation->isValidFuse(Decl2->getDefinition()))
    return false;

  // visiting the same child
  AccessPath TraversalRoot1 = extractVisitedChild(Call1);
  AccessPath TraversalRoot2 = extractVisitedChild(Call2);

  if (TraversalRoot1.SplittedAccessPath.size() !=
      TraversalRoot2.SplittedAccessPath.size())
    return false;

  for (int i = 0; i < TraversalRoot1.SplittedAccessPath.size(); i++) {
    if (TraversalRoot1.SplittedAccessPath[i].second !=
        TraversalRoot2.SplittedAccessPath[i].second)
      return false;
  }
  return true;
}

FusionTransformer::FusionTransformer(ASTContext *Ctx,
                                     FunctionsFinder *FunctionsInfo,
                                     std::string Heuristic) {
  Rewriter.setSourceMgr(Ctx->getSourceManager(), Ctx->getLangOpts());
  this->Ctx = Ctx;
  this->FunctionsInformation = FunctionsInfo;
  this->Heuristic = Heuristic;
  if (!this->Synthesizer)
    this->Synthesizer = new TraversalSynthesizer(Ctx, Rewriter, this);
}

void FusionTransformer::performFusion(
    const vector<clang::CallExpr *> &Candidate, bool IsTopLevel,
    clang::FunctionDecl *EnclosingFunctionDecl /*just needed fo top level*/,
    std::string Heuristic = "greedy") {

  bool HasVirtual = false;
  bool HasCXXMethod = false;

  for (auto *Call : Candidate) {
    auto *CalleeInfo = FunctionsFinder::getFunctionInfo(
        Call->getCalleeDecl()->getAsFunction()->getDefinition());
    if (CalleeInfo->isCXXMember())
      HasCXXMethod = true;
    if (CalleeInfo->isVirtual())
      HasVirtual = true;
  }
  AccessPath AP = extractVisitedChild(Candidate[0]);

  bool SelfCall =
      AP.getDeclAtIndex(AP.SplittedAccessPath.size() - 1) == nullptr;
  const CXXRecordDecl *TraversedType;
  if (SelfCall) {

    TraversedType = dyn_cast<CXXRecordDecl>(
        FunctionsFinder::getFunctionInfo(EnclosingFunctionDecl)
            ->getTraversedTreeTypeDecl());
  } else {
    TraversedType = AP.getDeclAtIndex(AP.SplittedAccessPath.size() - 1)
                        ->getType()
                        ->getPointeeCXXRecordDecl();
  }
  auto fuseFunctions = [&](const CXXRecordDecl *DerivedType) {
    if (!Synthesizer->isGenerated(Candidate, HasVirtual, DerivedType)) {
      Logger::getStaticLogger().logInfo(
          "Generating Code for function " +
          Synthesizer->createName(Candidate, HasVirtual, DerivedType));

      Logger::getStaticLogger().logInfo("Creating DG for a candidate");

      DependenceGraph *DepGraph =
          DepAnalyzer.createDependenceGraph(Candidate, HasVirtual, DerivedType);

      // added this part to perform coarse grained fusion.
      // for(auto *Node : DepGraph->getNodes()) {s
      //   std::vector<StatementInfo *> Statements;
      //   if(Node->getStatementInfo()->isCallStmt()) {
      //     Statements =
      //     Node->getStatementInfo()->getEnclosingFunction()->getStatements();
      //     for (auto *subStatements : Statements) {
      //       if (subStatements->isCallStmt()){
      //         if (subStatements->getCalledChild() != nullptr)
      //            Node->treeChildsVisited.insert(subStatements->getCalledChild());
      //       }
      //     }
      //   }
      // }

      // DepGraph->dump();
      // parallelize everythin here first
      // Continue parallelizing then fusing until the dependence graph converges

      //         std::vector<vector<DG_Node *>> parallel;
      //         std::vector<vector<DG_Node *>> temp;
      //         std::unordered_set<DG_Node*> set1;
      //         std::unordered_set<DG_Node*> set2;
      //         int i = 0;
      //         int j = 0;
      //         int k = 0;
      //         int l = 0;
      //         int check = 0;
      //         while(true)
      //         {
      //           cout << "ierations\n";
      //           cout << " ";
      /*parallel =*/ // parallelSchedule(DepGraph);
      //           if (parallel.size() == temp.size()){
      //
      //           for(j = 0; j < parallel.size(); j++){
      //
      //             if (parallel[j].size() != temp[j].size())
      //                break;
      //             for(k = 0; k < parallel[j].size(); k++)
      //                set1.insert(parallel[j][k]);
      //             for (k = 0; k < temp[j].size(); k++)
      //                 set2.insert(temp[j][k]);
      //             if (set2 != set1)
      //                 break;
      //             else{
      //                 check += 1;
      //                 set1.clear();
      //                 set2.clear();
      //             }
      //
      //           }}
      //           if(check == parallel.size())
      //              break;
      //           else{
      //             check = 0;
      //             set1.clear();
      //             set2.clear();
      //           }
      //           temp.swap(parallel);

      if (Heuristic != "solely-parallel") {
        performGreedyFusion(DepGraph);
      }
      // }
      LLVM_DEBUG(DepGraph->dumpMergeInfo());

      // Check that fusion was correctly made
      assert(!DepGraph->hasCycle() && "dep graph has cycle");
      assert(!DepGraph->hasWrongFuse() && "dep graph has wrong merging");

      // std::vector<DG_Node *> ToplogicalOrder = findToplogicalOrder(DepGraph);
      // //uncomment with recursion toposort
      std::vector<vector<DG_Node *>> ToplogicalOrder =
          parallelSchedule(DepGraph); // for the queue implementation

      Synthesizer->generateWriteBackInfo(Candidate, ToplogicalOrder, HasVirtual,
                                         HasCXXMethod, DerivedType);
      // added please remove if necessary !!!!!!!
      /////////////////////////////////////////////////////////////////////////////////////
      /*Synthesizer->generateWriteBackInfo_serial(Candidate, ToplogicalOrder,
         HasVirtual, HasCXXMethod, DerivedType);*/
      /////////////////////////////////////////////////////////////////////////////////////////

      // Logger::getStaticLogger().logDebug("Code Generation Done ");
    }
  };
  if (HasVirtual) {
    fuseFunctions(TraversedType);
    for (auto *DerivedType : RecordsAnalyzer::DerivedRecords[TraversedType]) {
      fuseFunctions(DerivedType);
    }
  } else
    fuseFunctions(nullptr);

  if (IsTopLevel) {

    Synthesizer->WriteUpdates(Candidate, EnclosingFunctionDecl);
  }
}

void FusionTransformer::performGreedyFusion(DependenceGraph *DepGraph) {
  unordered_map<clang::FieldDecl *, vector<DG_Node *>> ChildToCallers;

  std::vector<StatementInfo *> Statements_i;
  int numStatements_i = 0;

  std::vector<StatementInfo *> Statements_j;
  int numStatements_j = 0;

  for (auto *Node : DepGraph->getNodes()) {
    if (Node->getStatementInfo()->isCallStmt()) {
      ChildToCallers[Node->getStatementInfo()->getCalledChild()].push_back(
          Node);
    }
  }

  LLVM_DEBUG(for (auto &Entry
                  : ChildToCallers) {
    outs() << Entry.first->getNameAsString() << ":" << Entry.second.size()
           << "\n";
  });
  vector<unordered_map<clang::FieldDecl *, vector<DG_Node *>>::iterator>
      IteratorsList;

  for (auto It = ChildToCallers.begin(); It != ChildToCallers.end(); It++)
    IteratorsList.push_back(It);

  srand(time(nullptr));
  // random_shuffle(itList.begin(), itList.end());
  // random_shuffle(itList.begin(), itList.end());
  // random_shuffle(itList.begin(), itList.end());

  for (auto It : IteratorsList) {
    vector<DG_Node *> &CallNodes = It->second;

    // reverse(CallNodes.begin(), CallNodes.end());
    // random_shuffle(nodeLst.begin(), nodeLst.end());
    // random_shuffle(nodeLst.begin(), nodeLst.end());
    // random_shuffle(nodeLst.begin(), nodeLst.end());

    // while(true){
    for (int i = 0; i < CallNodes.size(); i++) {
      if (CallNodes[i]->isMerged())
        continue;

      for (int j = i + 1; j < CallNodes.size(); j++) {
        if (CallNodes[j]->isMerged())
          continue;

        // if (CallNodes[j]->ID == CallNodes[i]->ID){
        //   continue;
        // }

        // if (CallNodes[i]->getTraversalId() == CallNodes[j]->getTraversalId())
        //     continue;
        // check if there exist calls in CallNodes[i] and CallNodes[j] that act
        // on the same child and can't fused
        // if(unfusableCallsExist(CallNodes[i], CallNodes[j], DepGraph))
        //     continue;

        // if (CallNodes[i]->treeChildsVisited.size() != 2 &&
        // CallNodes[j]->treeChildsVisited.size() != 2)
        //    continue;

        // if (CallNodes[i]->treeChildsVisited !=
        // CallNodes[j]->treeChildsVisited)
        //    continue;

        // if(CallNodes[i]->isSpawned() && CallNodes[j]->isSpawned()){

        //   if(CallNodes[j]-> getTraversalId() == CallNodes[i]->
        //   getTraversalId()){

        // Statements_i = CallNodes[i] -> getStatementInfo() ->
        // getEnclosingFunction()->getStatements(); numStatements_i =
        // Statements_i.size(); bool tell_numStatements_i = false; for (auto *
        // myStatement: Statements_i){
        //    if (myStatement->isCallStmt())
        //        tell_numStatements_i = true;
        // }

        // Statements_j = CallNodes[j] -> getStatementInfo() ->
        // getEnclosingFunction()->getStatements(); numStatements_j =
        // Statements_j.size(); bool tell_numStatements_j = false; for (auto *
        // myStatement: Statements_j){
        //   if (myStatement->isCallStmt())
        //       tell_numStatements_j = true;
        // }

        // if (( CallNodes[i]->isSpawned() && numStatements_i > 16)  &&
        // (CallNodes[j]->isSpawned() && numStatements_j > 16) ){

        // if (tell_numStatements_i && tell_numStatements_j)
        //   continue;
        //}

        // int token = rand() % 2;

        //  if (CallNodes[i]->isSpawned() && CallNodes[j]->isSpawned() && token
        //  == 0)
        //      continue;

        //  if (numStatements_i >= 16 && numStatements_j >= 16)
        //       continue;

        // int token = rand() % 2;
        //             cout<<token;
        //             cout<<"\n";
        // if (token == 0)
        //  continue;

        // cout<<numStatements;
        // cout<<"\n";
        //      continue;
        //   }

        // }

        // potentially add the if condition to check if calls are in parallel or
        // not if(!CallNodes[i]->IsSpawned && !CallNodes[j]->IsSpawned)
        DepGraph->merge(CallNodes[i], CallNodes[j]);

        auto ReachMaxMerged = [&](MergeInfo *Info) {
          unordered_map<FunctionDecl *, int> Counter;
          for (auto *Node : Info->MergedNodes) {
            Counter[Node->getStatementInfo()
                        ->getCalledFunction()
                        ->getDefinition()]++;
            auto Count = Counter[Node->getStatementInfo()
                                     ->getCalledFunction()
                                     ->getDefinition()];

            if (Count > opts::MaxMergedInstances) {
              return true;
            }
          }
          return false;
        };

        if (CallNodes[i]->getMergeInfo()->MergedNodes.size() >
                opts::MaxMergedNodes ||
            ReachMaxMerged(CallNodes[i]->getMergeInfo()) ||
            DepGraph->hasCycle() ||
            DepGraph->hasWrongFuse(CallNodes[i]->getMergeInfo())) {
          LLVM_DEBUG(outs()
                     << "rollback on merge, " << DepGraph->hasCycle() << ","
                     << DepGraph->hasWrongFuse(CallNodes[i]->getMergeInfo())
                     << "\n");

          DepGraph->unmerge(CallNodes[j]);
        }
      }
    }
    // }
  }
}

/*
void FusionTransformer::findToplogicalOrderRec(
    vector<DG_Node *> &TopOrder, unordered_map<DG_Node *, bool> &Visited,
    DG_Node *Node) {
  if (!Node->allPredesVisited(Visited))
    return;

  TopOrder.push_back(Node);*/
/*
if (!Node->isMerged()) {
  Visited[Node] = true;
  for (auto &SuccDep : Node->getSuccessors()){
    if (!Visited[SuccDep.first])
      findToplogicalOrderRec(TopOrder, Visited, SuccDep.first);
  }
  return;
}

// Handle merged node
for (auto *MergedNode : Node->getMergeInfo()->MergedNodes) {
  assert(!Visited[MergedNode]);
  Visited[MergedNode] = true;
}
 //call return Successors
for (auto *MergedNode : Node->getMergeInfo()->MergedNodes) {
  for (auto &SuccDep : MergedNode->getSuccessors()) {

    if (Node->getMergeInfo()->isInMergedNodes(SuccDep.first))
      continue;

    // WRONG ASSERTION
    if (!Visited[SuccDep.first])
      findToplogicalOrderRec(TopOrder, Visited, SuccDep.first);
  }
 }*/
/*
Node->markVisited(Visited);
for (auto successors : Node->getAllSuccessors()){
   if(!Visited[successors]){
      findToplogicalOrderRec(TopOrder, Visited, successors);
  }
}
}

std::vector<DG_Node *>
FusionTransformer::findToplogicalOrder(DependenceGraph *DepGraph) {
std::unordered_map<DG_Node *, bool> Visited;
std::vector<DG_Node *> Order;

bool AllVisited = false;
//DepGraph->dump();
while (!AllVisited) {
 AllVisited = true;
 for (auto *Node : DepGraph->getNodes()) {
   if (!Visited[Node]) {
     AllVisited = false;
     findToplogicalOrderRec(Order, Visited, Node);
   }
 }
}
return Order;
}
*/

std::vector<vector<DG_Node *>>
FusionTransformer::parallelSchedule(DependenceGraph *DepGraph) {
  std::vector<vector<DG_Node *>> Order; // vector of vector for || schedule
  std::list<DG_Node *> readyList;
  std::unordered_map<DG_Node *, bool> Visited;

  // add only the roots!
  // std::multimap<DG_Node *, FieldDecl *> Visiting_Children;
  for (auto *Node : DepGraph->getNodes()) {

    // if(Node->allPredesVisited(Visited)) //check to see if root node or not

    // if(Node->isRootNode()!= Node->allPredesVisited(Visited) ){
    //   outs()<<"checking mismatch:\n";

    //   outs()<<Node->getTraversalId()<<"\n";
    //   outs()<<Node->getStatementInfo()->getStatementId()<<"\n";

    //   Node->getStatementInfo()->Stmt->dump();

    //   outs()<<"graph is \n";
    //   DepGraph->dumpMergeInfo();

    //   DepGraph->dump();

    // }
    //  assert(Node->isRootNode() == Node->allPredesVisited(Visited) &&
    //  "expecting root to have all pred visited!" );
    if (Node->isRootNode())
      readyList.push_back(Node);

    // std::vector<StatementInfo *> Statements;
    // if(Node->getStatementInfo()->isCallStmt()){

    //    Statements =
    //    Node->getStatementInfo()->getEnclosingFunction()->getStatements();
    //    std::vector<FieldDecl *> childCalled;
    //    for (auto *subStatements : Statements){

    //        if (subStatements->isCallStmt()){
    //            //outs() << "found a call statement\n";
    //            if (subStatements->getCalledChild() != nullptr)
    //                Node->treeChildsVisited.insert(subStatements->getCalledChild());
    //            //Visiting_Children.insert ( pair<DG_Node *, FieldDecl
    //            *>(Node, subStatements->getCalledChild()) );
    //        }

    //   }
    // }
  }

  // for (multimap<DG_Node *, FieldDecl *>::iterator it =
  // Visiting_Children.begin(); it != Visiting_Children.end(); ++it)
  //          cout << "  [" << (*it).first << ", " << (*it).second << "]" <<
  //          endl;

  // for(auto *Node : DepGraph->getNodes()){

  // cout << Node->treeChildsVisited.size() << endl;

  //}

  int counterID = 0;
  while (!readyList.empty()) {

    for (auto it = readyList.begin(); it != readyList.end();) {
      auto *Node = *it;
      auto *stmInfo = Node->getStatementInfo();
      if (stmInfo->isCallStmt()) {
        it++;
        continue;
      }

      if (Visited[Node]) {
        auto prev = it;
        it++;
        readyList.erase(prev);
        continue;
      }

      std::vector<DG_Node *> stmOrder; // make a vector for the parallel order
                                       // in each iteration of statements
      stmOrder.push_back(Node);
      Order.push_back(stmOrder);
      Node->markVisited(Visited);
      auto prev = it;
      it++;
      readyList.erase(prev);

      for (auto successor : Node->getAllSuccessors()) {
        if (!Visited[successor] && successor->allPredesVisited(Visited))
          readyList.push_back(successor);
      }
    } // after this loop readyList will only contain all the possible parallel
      // calls order

    std::vector<DG_Node *> parallelOrder; // parallel Order vector for calls

    for (auto *Node : readyList) {
      if (!Visited[Node]) {
        Node->markVisited(Visited);
        parallelOrder.push_back(Node);
      }
    }

    /*
    if(parallelOrder.size() == 1){
      for(auto *Node : parallelOrder){
          Node->IsSpawned = false;
      }
    }
    else{

     for(auto *Node : parallelOrder){
        Node->IsSpawned = true;
     }

    }
    */

    for (auto *Node : parallelOrder) {

      Node->ID = counterID;
    }

    readyList.clear();

    for (auto *Node : parallelOrder) {
      for (auto successor : Node->getAllSuccessors()) {
        if (!Visited[successor] && successor->allPredesVisited(Visited))
          readyList.push_back(successor);
      }
    }

    Order.push_back(parallelOrder);
    counterID++;
  }

  return Order;
}

bool FusionTransformer::unfusableCallsExist(DG_Node *function1,
                                            DG_Node *function2,
                                            DependenceGraph *DepGraph) {

  std::vector<StatementInfo *> body1 =
      function1->getStatementInfo()->getEnclosingFunction()->getStatements();

  std::vector<StatementInfo *> body2 =
      function2->getStatementInfo()->getEnclosingFunction()->getStatements();

  unordered_map<clang::FieldDecl *, vector<StatementInfo *>> ChildToCallers1;

  unordered_map<clang::FieldDecl *, vector<StatementInfo *>> ChildToCallers2;

  for (auto *statements : body1) {
    if (statements->isCallStmt()) {
      if (statements->getCalledChild() != nullptr)
        ChildToCallers1[statements->getCalledChild()].push_back(statements);
    }
  }

  for (auto *statements : body2) {
    if (statements->isCallStmt()) {
      if (statements->getCalledChild() != nullptr)
        ChildToCallers2[statements->getCalledChild()].push_back(statements);
    }
  }

  vector<unordered_map<clang::FieldDecl *, vector<StatementInfo *>>::iterator>
      IteratorsList;

  for (auto It = ChildToCallers1.begin(); It != ChildToCallers1.end(); It++)
    IteratorsList.push_back(It);

  for (auto It : IteratorsList) {
    vector<StatementInfo *> &CallNodes1 = It->second;
    clang::FieldDecl *Child = It->first;

    if (ChildToCallers2.find(Child) == ChildToCallers2.end()) {

      vector<StatementInfo *> CallNodes2 = ChildToCallers2[Child];

      for (int i = 0; i < CallNodes1.size(); i++) {

        DG_Node *Node1;
        for (auto *Node : DepGraph->getNodes()) {
          if (Node->getStatementInfo() == CallNodes1[i] &&
              Node->getTraversalId() == function1->getTraversalId()) {
            Node1 = Node;
            break;
          }
        }
        for (int j = 0; j < CallNodes2.size(); j++) {

          DG_Node *Node2;
          for (auto *Node : DepGraph->getNodes()) {
            if (Node->getStatementInfo() == CallNodes2[j] &&
                Node->getTraversalId() == function2->getTraversalId()) {
              Node2 = Node;
              break;
            }
          }

          DepGraph->merge(Node1, Node2);

          //  auto ReachMaxMerged = [&](MergeInfo *Info) {
          //    unordered_map<FunctionDecl *, int> Counter;
          //    for (auto *Node : Info->MergedNodes) {
          //      Counter[Node->getStatementInfo()
          //               ->getCalledFunction()
          //               ->getDefinition()]++;
          //       auto Count = Counter[Node->getStatementInfo()
          //                            ->getCalledFunction()
          //                            ->getDefinition()];
          //       if (Count > opts::MaxMergedInstances) {
          //         return true;
          //     }
          //   }
          //   return false;
          //  };

          if (/*Node1->getMergeInfo()->MergedNodes.size() > opts::MaxMergedNodes
                 ||*/
              /*ReachMaxMerged(Node1->getMergeInfo()) ||*/ DepGraph
                  ->hasCycle() ||
              DepGraph->hasWrongFuse(Node1->getMergeInfo())) {

            DepGraph->unmerge(Node2);

            return true;
          }

          DepGraph->unmerge(Node2);
        }
      }
    }
    // else{

    //    return true;

    // }
  }

  return false;
}