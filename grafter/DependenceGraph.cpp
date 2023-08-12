//===--- DependeceGraph.cpp -----------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "DependenceGraph.h"
#include <stack>

std::vector<DG_Node *> MergeInfo::getCallsOrdered() {
  vector<DG_Node *> Res;
  for (auto *Node : MergedNodes) {
    Res.push_back(Node);
  }
  std::sort(Res.begin(), Res.end(), [](const DG_Node *a, const DG_Node *b) {
    if (a->getTraversalId() != b->getTraversalId())
      return a->getTraversalId() < b->getTraversalId();
    else
      return a->getStatementInfo()->getStatementId() <
             b->getStatementInfo()->getStatementId();
  });
  return Res;
};

// added functionality for mark nodes visited
void DG_Node::markVisited(std::unordered_map<DG_Node *, bool> &VisitedNodes) {

  if (!this->isMerged()) {
    VisitedNodes[this] = true;
    return;
  }

  // merged node case
  auto *mergInfo = getMergeInfo();
  for (auto *n : mergInfo->MergedNodes) {
    VisitedNodes[n] = true;
  }
}
// added functionality for returning all the successors of the merged nodes
set<DG_Node *> DG_Node::getAllSuccessors() {
  std::set<DG_Node *> successors;
  if (isMerged()) {
    auto *mergedInfo = getMergeInfo();
    for (auto *n : getMergeInfo()->MergedNodes) {
      for (auto &succ : n->getSuccessors()) {
        auto *succNode = succ.first;
        successors.insert(succNode);
      }
    }
  } else
    for (auto succ : getSuccessors()) {
      successors.insert(succ.first);
    }
  return successors;
}

bool DG_Node::allPredesVisited(
    std::unordered_map<DG_Node *, bool> &VisitedNodes) {
  if (!isMerged()) {
    for (auto &Dependency : getPredecessors()) {
      if (!VisitedNodes[Dependency.first])
        return false;
    }
    return true;
  }

  // Handle Merged Nodes
  for (auto *MergedNode : Info->MergedNodes) {
    for (auto &Dependency : MergedNode->getPredecessors()) {

      // Excluded internal dependences between merged Nodes
      if (Info->isInMergedNodes(Dependency.first))
        continue;

      if (!VisitedNodes[Dependency.first])
        return false;
    }
  }
  return true;
}

bool DG_Node::isRootNode() {
  std::unordered_map<DG_Node *, bool> tmp;
  return allPredesVisited(tmp);
}

/// TODO why pir?
DG_Node *DependenceGraph::createNode(pair<StatementInfo *, int> Value) {

  DG_Node *Node = new DG_Node(Value.first, Value.second);
  Nodes.push_back(Node);
  return Node;
}

void DependenceGraph::merge(DG_Node *Node1, DG_Node *Node2) {
  if (Node1->isMerged() && Node2->isMerged()) {

    MergeInfo *Tmp = Node2->Info;

    for (auto *Node : Node2->Info->MergedNodes) {
      Node1->Info->MergedNodes.insert(Node);
      Node->Info = Node1->Info;
    }
    delete Tmp;

  }

  else if (Node1->isMerged() && !Node2->isMerged()) {
    Node2->Info = Node1->Info;
    Node1->Info->MergedNodes.insert(Node2);
    Node2->IsMerged = true;

  } else if (!Node1->isMerged() && Node2->isMerged()) {
    Node1->Info = Node2->Info;
    Node2->Info->MergedNodes.insert(Node1);
    Node1->IsMerged = true;

  } else if (!Node1->isMerged() && !Node2->isMerged()) {
    MergeInfo *Tmp = new MergeInfo();
    Node1->IsMerged = true;
    Node2->IsMerged = true;

    Tmp->MergedNodes.insert(Node1);
    Tmp->MergedNodes.insert(Node2);
    Node1->Info = Tmp;
    Node2->Info = Tmp;
  }
}

void DependenceGraph::unmerge(DG_Node *Node) {
  MergeInfo *NodeMergeInfo = Node->Info;

  // unmerge current Node
  Node->IsMerged = false;
  Node->Info = nullptr;
  NodeMergeInfo->MergedNodes.erase(Node);

  // special case if unmerging resulted in single Node
  if (NodeMergeInfo->MergedNodes.size() == 1) {
    (*NodeMergeInfo->MergedNodes.begin())->IsMerged = false;
    (*NodeMergeInfo->MergedNodes.begin())->Info = nullptr;
    delete NodeMergeInfo;
  }
}

void DependenceGraph::dump() {
  Logger::getStaticLogger().logDebug("dumping the dependence graph");
  Logger::getStaticLogger().logDebug("dumping Nodes:");

  for (auto *Node : Nodes) {
    Logger::getStaticLogger().logDebug(
        "TraversalId:" + to_string(Node->TraversalId) +
        ",stmtId:" + to_string(Node->getStatementInfo()->getStatementId()) +
        ",address:" + to_string((long long)(Node->getStatementInfo())) +
        ",body:" + string(Node->getStatementInfo()->Stmt->getStmtClassName()));

    for (auto &Successor : Node->getSuccessors()) {
      auto *SuccNode = Successor.first;
      auto &SuccDep = Successor.second;
      Logger::getStaticLogger().logDebug(
          "dep To :[" + to_string(SuccNode->TraversalId) + "|" +
          to_string(SuccNode->getStatementInfo()->getStatementId()) + "|" +
          to_string((long long)(SuccNode->getStatementInfo())) + "], Type:[" +
          to_string(SuccDep.CONTROL_DEP) + "|" + to_string(SuccDep.GLOBAL_DEP) +
          "|" + to_string(SuccDep.LOCAL_DEP) + "|" +
          to_string(SuccDep.ONTREE_DEP) + "|" +
          to_string(SuccDep.ONTREE_DEP_FUSABLE) + "]");
    }
  }
}

// void DependenceGraph::dumpToPrint() {
//   Logger::getStaticLogger().logDebug("dumping the dependence graph");
//   Logger::getStaticLogger().logDebug("dumping Nodes");
//
//   for (auto &Node : Nodes) {
//
//     for (auto &Successor : Node->getSuccessors()) {
//       auto *SuccNode = Successor.first;
//       auto &SuccDep = Successor.second;
//
//       if (Node->TraversalId != SuccNode->TraversalId) {
//         Logger::getStaticLogger().log(
//             "\"t:" + to_string(Node->TraversalId) +
//             ",s:" + to_string(Node->StatementInfo->getStatementId()) +
//             "\"" + " -> " + "\"t:" + to_string(SuccNode->TraversalId) + ",s:"
//             + to_string(SuccNode->StatementInfo->getStatementId()) + "\";");
//       }
//     }
//   }
//   for (auto &Node : Nodes) {
//
//     if (Node->StatementInfo->isCallStmt()) {
//       Logger::getStaticLogger().log(
//           "\"t:" + to_string(Node->TraversalId) +
//           ",s:" + to_string(Node->StatementInfo->getStatementId()) + "\"" +
//           ";[color=\"0.305 0.625 1.000\"]\n");
//     }
//   }
// }

void DependenceGraph::dumpMergeInfo() {
  Logger::getStaticLogger().logDebug("Dumping Merge Info\n");
  unordered_map<MergeInfo *, bool> Visited;
  for (auto *Node : Nodes) {
    if (!Node->getStatementInfo()->isCallStmt())
      continue;

    if (!Node->isMerged()) {
      Logger::getStaticLogger().logDebug(
          "unmerged call Node to child:" +
          Node->getStatementInfo()->getCalledChild()->getNameAsString() + ":[" +
          to_string(Node->TraversalId) + "|" +
          to_string(Node->getStatementInfo()->getStatementId()) + "|" +
          to_string((long long)(Node->getStatementInfo())));
      continue;
    }

    if (Node->isMerged() && !Visited[Node->Info]) {
      auto *NodeMergeInfo = Node->Info;
      Visited[NodeMergeInfo] = true;
      Logger::getStaticLogger().logDebug("merge info :\nmerged child :" +
                                         (*NodeMergeInfo->MergedNodes.begin())
                                             ->getStatementInfo()->getCalledChild()
                                             ->getNameAsString() +
                                         " participating Nodes:");

      for (auto *MergedNode : NodeMergeInfo->MergedNodes) {
        Logger::getStaticLogger().logDebug(
            "Node :[" + to_string(MergedNode->TraversalId) + "|" +
            to_string(MergedNode->getStatementInfo()->getStatementId()) + "|" +
            to_string((long long)(MergedNode->getStatementInfo())));
      }
    }
  }
}

void DependenceGraph::addDependency(DEPENDENCE_TYPE DependenceType,
                                    DG_Node *Src, DG_Node *Dest) {
  assert(Src != Dest);
  // TODO: add getSuccessorsType function
  if (DependenceType == GLOBAL_DEP) {
    Src->getSuccessors()[Dest].GLOBAL_DEP = true;
    Dest->getPredecessors()[Src].GLOBAL_DEP = true;

  } else if (DependenceType == LOCAL_DEP) {
    Src->getSuccessors()[Dest].LOCAL_DEP = true;
    Dest->getPredecessors()[Src].LOCAL_DEP = true;

  } else if (DependenceType == ONTREE_DEP) {
    Src->getSuccessors()[Dest].ONTREE_DEP = true;
    Dest->getPredecessors()[Src].ONTREE_DEP = true;

  } else if (DependenceType == ONTREE_DEP_FUSABLE) {

    Src->getSuccessors()[Dest].ONTREE_DEP_FUSABLE = true;
    Dest->getPredecessors()[Src].ONTREE_DEP_FUSABLE = true;
  }

  else if (DependenceType == CONTROL_DEP) {
    Src->getSuccessors()[Dest].CONTROL_DEP = true;
    Dest->getPredecessors()[Src].CONTROL_DEP = true;
  }
}

// Check that merged nodes call the same child
bool DependenceGraph::hasWrongFuse(MergeInfo *MergeInfo) {
  std::set<int> TraversalIds;
  clang::FieldDecl *CalledChild =
      (*MergeInfo->MergedNodes.begin())->getStatementInfo()->getCalledChild();

  // check that they all have the same called child
  for (auto *MergedNode : MergeInfo->MergedNodes) {
    if (MergedNode->getStatementInfo()->getCalledChild() != CalledChild)
      return true;

    TraversalIds.insert(MergedNode->TraversalId);
  }
  return false;
}

bool DependenceGraph::hasWrongFuse() {
  unordered_map<MergeInfo *, bool> Visited;
  for (auto *Node : Nodes) {

    if (Node->isMerged() && !Visited[Node->Info]) {
      Visited[Node->Info] = true;
      if (hasWrongFuse(Node->Info))
        return true;
    }
  }
  return false;
}

bool DependenceGraph::hasIllegalMerge() { return hasCycle() || hasWrongFuse(); }

bool DependenceGraph::hasCycleRec(DG_Node *Node,
                                  unordered_map<DG_Node *, int> &Visited,
                                  stack<DG_Node *> &CyclePath) {
  // All children are visited (black)
  if (Visited[Node] == 2)
    return false;

  // Visit still on stack (gray)
  if (Visited[Node] == 1) {
    CyclePath.push(Node);
    return true;
  }

  CyclePath.push(Node);

  if (!Node->isMerged()) {
    Visited[Node] = 1;

    //  visit successors
    for (auto &Successor : Node->getSuccessors()) {
      if (hasCycleRec(Successor.first, Visited, CyclePath))
        return true;
    }

    Visited[Node] = 2;
    CyclePath.pop();
    return false;
  }

  // Handle merged nodes
  MergeInfo *NodeMergeInfo = Node->Info;

  for (auto *MergedNode : NodeMergeInfo->MergedNodes) {
    assert(!Visited[MergedNode]);
    Visited[MergedNode] = 1;
  }

  // Visit all successors
  for (auto *MergedNode : NodeMergeInfo->MergedNodes) {
    for (auto &Successor : MergedNode->getSuccessors()) {
      if (NodeMergeInfo->isInMergedNodes(Successor.first))
        continue;
      else {
        if (hasCycleRec(Successor.first, Visited, CyclePath))
          return true;
      }
    }
  }

  // Finalize colors
  for (auto *MergedNode : NodeMergeInfo->MergedNodes) {
    assert(Visited[MergedNode] == 1);
    Visited[MergedNode] = 2;
  }
  CyclePath.pop();
  return false;
}

void DependenceGraph::printCyclePath(stack<DG_Node *> CyclePath) {

  Logger::getStaticLogger().logDebug("printing cycle info ");
  while (!CyclePath.empty()) {
    DG_Node *TopNode = CyclePath.top();
    CyclePath.pop();

    if (TopNode->isMerged()) {
      Logger::getStaticLogger().logDebug("-merge Node participating Nodes  :");

      for (auto *MergedNode : TopNode->Info->MergedNodes) {
        Logger::getStaticLogger().logDebug(
            "\tNode :[" + to_string(MergedNode->getTraversalId()) + "|" +
            to_string(MergedNode->getStatementInfo()->getStatementId()) + "|" +
            to_string((long long)(MergedNode->getStatementInfo())));
      }
    } else {
      Logger::getStaticLogger().logDebug(
          "-single Node :[" + to_string((TopNode)->getTraversalId()) + "|" +
          to_string((TopNode)->getStatementInfo()->getStatementId()) + "|" +
          to_string((long long)((TopNode)->getStatementInfo())));
      Logger::getStaticLogger().logDebug(
          TopNode->getStatementInfo()->Stmt->getStmtClassName());
    }
  }
}

bool DependenceGraph::hasCycle() {

  std::unordered_map<DG_Node *, int> Visited;
  stack<DG_Node *> CyclePath;
  for (auto Node : Nodes) {
    // Not  Grey
    assert(Visited[Node] != 1);
    if (!Visited[Node]) {
      if (hasCycleRec(Node, Visited, CyclePath)) {
        // printCyclePath(CyclePath);
        return true;
      }
    }
  }
  return false;
}

void DependenceGraph::mergeAllCalls() {
  std::unordered_map<clang::FieldDecl *, vector<DG_Node *>> ChildCallers;
  for (auto *Node : Nodes) {
    if (Node->getStatementInfo()->isCallStmt()) {
      ChildCallers[Node->getStatementInfo()->getCalledChild()].push_back(Node);
    }
  }

  for (auto &ChildAndCallers : ChildCallers) {
    for (int i = 1; i < ChildAndCallers.second.size(); i++) {
      merge(ChildAndCallers.second[i - 1], ChildAndCallers.second[i]);
    }
  }
}