//===--- FSMUtility.cpp --------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//
#include <FSMUtility.h>
#include <Logger.h>
#include <cstdlib>
#include <string>
#include <vector>

#define DEBUG_TYPE "fsm-utility"

int FSMUtility::Counter = 2;

std::unordered_map<clang::ValueDecl *, int> FSMUtility::SymbolToLabel =
    []() -> std::unordered_map<clang::ValueDecl *, int> {
  std::unordered_map<clang::ValueDecl *, int> ret;
  ret[nullptr] = 1;
  return ret;
}();

std::unordered_map<int, clang::ValueDecl *> FSMUtility::LabelToSymbol =
    []() -> std::unordered_map<int, clang::ValueDecl *> {
  std::unordered_map<int, clang::ValueDecl *> ret;
  ret[1] = nullptr;
  return ret;
}();
std::unordered_map<int, int> FSMUtility::SymbolToLabel_Abst =
    std::unordered_map<int, int>();

/// Maps strict access labels to their symbols
std::unordered_map<int, int> FSMUtility::LabelToSymbol_Abst =
    std::unordered_map<int, int>();

FSM *FSMUtility::AnyClosureAutomata = nullptr;

void FSMUtility::addSymbol(clang::ValueDecl *ValueDecl) {
  if (!SymbolToLabel.count(ValueDecl)) {
    LLVM_DEBUG(ValueDecl->dump());
    LLVM_DEBUG(outs() << "mapped to " << Counter);
    SymbolToLabel[ValueDecl] = Counter;
    LabelToSymbol[Counter] = ValueDecl;
    Counter++;
  }
}

void FSMUtility::addSymbol(int AbstractAccessId) {
  if (!SymbolToLabel_Abst.count(AbstractAccessId)) {
    SymbolToLabel_Abst[AbstractAccessId] = Counter;
    LabelToSymbol_Abst[Counter] = AbstractAccessId;
    Counter++;
  }
}

void FSMUtility::addTransition(FSM &Automata, int Src, int Dest,
                               clang::ValueDecl *Access) {
  assert(SymbolToLabel.count(Access));
  Automata.AddArc(
      Src, fst::StdArc(SymbolToLabel[Access], SymbolToLabel[Access], 0, Dest));
}

void FSMUtility::addTransitionOnAbstractAccess(FSM &Automata, int Src, int Dest,
                                               int Access) {
  assert(SymbolToLabel_Abst.count(Access));
  Automata.AddArc(Src, fst::StdArc(SymbolToLabel_Abst[Access],
                                   SymbolToLabel_Abst[Access], 0, Dest));
}

void FSMUtility::addTraversedNodeTransition(FSM &Automata, int Src, int Dest) {
  Automata.AddArc(Src, fst::StdArc(1, 1, 0, Dest));
}

void FSMUtility::addEpsTransition(FSM &Automata, int Src, int Dest) {
  Automata.AddArc(Src, fst::StdArc(0, 0, 0, Dest));
}

void FSMUtility::addAnyTransition(FSM &Automata, int Src, int Dest) {
  for (int I = 1; I < Counter; I++) {
    Automata.AddArc(Src, fst::StdArc(I, I, 0, Dest));
  }
}

bool FSMUtility::hasNonEmptyIntersection(const FSM &Automata1,
                                         const FSM &Automata2) {
  FSM Intersection;
  fst::Intersect(Automata1, Automata2, &Intersection);
  return !isEmpty(Intersection);
}

bool FSMUtility::isEmpty(const FSM &Automata) {
  FSM Intersection;
  fst::Intersect(Automata, Automata, &Intersection);
  return Intersection.NumStates() == 0;
}

const FSM &FSMUtility::getAnyClosureAutomata() {
  if (!AnyClosureAutomata) {
    AnyClosureAutomata = new FSM();
    int Src = AnyClosureAutomata->AddState();
    AnyClosureAutomata->SetStart(Src);

    int Dest = AnyClosureAutomata->AddState();
    AnyClosureAutomata->SetFinal(Dest, 0);

    FSMUtility::addAnyTransition(*AnyClosureAutomata, Src, Dest);

    fst::Closure(AnyClosureAutomata, fst::CLOSURE_STAR);
  }

  return *AnyClosureAutomata;
}

FSM *FSMUtility::CopyRootRemoved(const FSM &In) {
  // create a copy of the input
  FSM *Out = new FSM();
  fst::Union(Out, In);
  std::vector<std::pair<int, int>> ReplaceMap;
  // convert transition on root to epsilon transitions
  ReplaceMap.push_back(fst::make_pair(1, 0));
  fst::Relabel(Out, ReplaceMap, ReplaceMap);
  return Out;
}

void FSMUtility::print(const FSM &Automata, std::string FileName,
                       bool Simplify) {

  if (!Automata.NumStates() ||
      !FSMUtility::hasNonEmptyIntersection(Automata, Automata)) {
    Logger::getStaticLogger().logWarn(
        "TREEFUSER_WARNING: Cannot print empty automata");
    return;
  }

  system((string("rm ") + FileName + ".*").c_str());

  if (!Simplify) {
    Automata.Write(FileName + ".fst");
  } else {
    FSM Tmp, Tmp2, Tmp3, Tmp4;
    fst::Union(&Tmp, Automata);
    //  fst::Disambiguate(Automata, &Tmp);
    fst::RmEpsilon(&Tmp);
    fst::Minimize(&Tmp, &Tmp2, 0, true);
    fst::Disambiguate(Tmp, &Tmp3);
    fst::Minimize(&Tmp3, &Tmp2, 0, true);
    // fst::Disambiguate(Tmp3, &Tmp4);
    //  fst::Minimize(&Tmp4, &Tmp2, 0, true);

    Tmp3.Write(FileName + ".fst");
  }

  system((string("fstdraw ") + FileName + ".fst " + FileName + ".dot").c_str());

  system(
      (string("sed -i 's/") + "0:0" + +"/" + "eps" + "/g' " + FileName + ".dot")
          .c_str());

  // Replace labels with symbols
  for (auto &Entry : LabelToSymbol) {
    if (Entry.second) // avoid when nullptr (thisExpr)
      system((string("sed -i 's/") + std::to_string(Entry.first) + ":" +
              std::to_string(Entry.first) + "/" +
              Entry.second->getNameAsString() + "/g' " + FileName + ".dot")
                 .c_str());
  }
  for (auto &Entry : LabelToSymbol_Abst) {
    if (Entry.second) // avoid when nullptr (thisExpr)
      system((string("sed -i 's/") + std::to_string(Entry.first) + ":" +
              std::to_string(Entry.first) + "/" + string("loc<") +
              to_string(Entry.second) + ">" + "/g' " + FileName + ".dot")
                 .c_str());
  }
  system((string("sed -i 's/") + "1:1" + +"/" + "^root" + "/g' " + FileName +
          ".dot")
             .c_str());

  system((string("dot -Tps  ") + FileName + ".dot  > " + FileName + ".ps")
             .c_str());
  // system((string("open ") + FileName + ".ps").c_str());
}
