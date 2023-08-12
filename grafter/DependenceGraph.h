//===--- DependeceGraph.h -------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#ifndef TREE_FUSER_DEPENDENCE_GRAPH
#define TREE_FUSER_DEPENDENCE_GRAPH

#include "Logger.h"
#include "StatementInfo.h"

#include <stack>
#include <stdio.h>
#include <unordered_map>
#include <vector>

class DG_Node;
class DependenceGraph;

enum DEPENDENCE_TYPE {
  GLOBAL_DEP,
  LOCAL_DEP,
  ONTREE_DEP,
  ONTREE_DEP_FUSABLE,
  CONTROL_DEP
};

struct MergeInfo {
public:
  std::set<DG_Node *> MergedNodes;
  bool isInMergedNodes(DG_Node *Node) { return MergedNodes.count(Node); }

  /// Return a list of the merged nodes ordered by their original execution
  /// order
  vector<DG_Node *> getCallsOrdered();
};

struct DependenceInfo {
public:
  bool GLOBAL_DEP = false;
  bool LOCAL_DEP = false;
  bool ONTREE_DEP = false;
  bool ONTREE_DEP_FUSABLE = false;
  bool CONTROL_DEP = false;
  bool isNotFusable() {
    return GLOBAL_DEP || LOCAL_DEP || ONTREE_DEP || CONTROL_DEP;
  }
};

class DG_Node {
  friend DependenceGraph;

private:
  std::unordered_map<DG_Node *, DependenceInfo> Successors;
  std::unordered_map<DG_Node *, DependenceInfo> Predecessors;

  /// A unique Id associated with each traversal within the dependence graph in
  /// their original order
  int TraversalId;

  /// Pointer to the statment information associated with the stored node
  StatementInfo *StmtInfo;

  /// Indicates if this node is a merged call node
  bool IsMerged = false;

  /// Store merge information if the node is merged
  MergeInfo *Info = nullptr;

public:
  // keep track of all the tree children that are visited by a call statement.
  std::set<FieldDecl *> treeChildsVisited;

  bool isRootNode();

  // indicates if node is candidate for parrallel execution
  bool IsSpawned = false;

  int ID;

  struct MergeInfo *getMergeInfo() const {
    return Info;
  }

  class StatementInfo *getStatementInfo() const {
    return StmtInfo;
  }

  std::unordered_map<DG_Node *, DependenceInfo> &getSuccessors() {
    return Successors;
  }

  std::unordered_map<DG_Node *, DependenceInfo> &getPredecessors() {
    return Predecessors;
  }

  bool isMerged() const { return IsMerged; }

  bool isSpawned() const { return IsSpawned; }

  int getTraversalId() const { return TraversalId; }

  DG_Node(class StatementInfo *StmtInfo_, int TraversalId_) {
    StmtInfo = StmtInfo_;
    TraversalId = TraversalId_;
  }

  bool allPredesVisited(std::unordered_map<DG_Node *, bool> &VisitedNodes);
  void markVisited(std::unordered_map<DG_Node *, bool> &VisitedNodes);
  set<DG_Node *> getAllSuccessors();
};

class DependenceGraph {
private:
  bool hasCycleRec(DG_Node *Node, std::unordered_map<DG_Node *, int> &Visited,
                   std::stack<DG_Node *> &CyclePath);
  /// Store all graph nodes
  std::vector<DG_Node *> Nodes;

public:
  std::vector<DG_Node *> &getNodes() { return Nodes; }

  void dump();

  void dumpToPrint();

  void dumpMergeInfo();

  /// Merge two nodes in the graph
  void merge(DG_Node *Node1, DG_Node *Node2);

  /// Unmerge a node from the set of nodes that its merged with
  void unmerge(DG_Node *Node);

  /// add a dependence between two nodes
  void addDependency(DEPENDENCE_TYPE DepType, DG_Node *Src, DG_Node *Dest);

  /// Check if the merged cal node are safe to be merged
  bool hasWrongFuse(MergeInfo *MergeInfo);

  DG_Node *createNode(pair<StatementInfo *, int> Value);

  bool hasCycle();

  bool hasWrongFuse();

  bool hasIllegalMerge();

  void mergeAllCalls();

  void printCyclePath(stack<DG_Node *> path);
};
#endif
