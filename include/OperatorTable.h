#ifndef KALEIDOSCOPE_OPERATORTABLE_H
#define KALEIDOSCOPE_OPERATORTABLE_H

#include <cctype>
#include <map>

namespace kaleidoscope {

/// OperatorTable - Precedence for binary operators.
///
/// Shared by the Parser, which reads it, and CodeGen, which writes it when a
/// "def binary | 5 (LHS RHS) ..." is generated. Owned by neither, so the two
/// do not depend on each other.
class OperatorTable {
  std::map<char, int> Precedence;

public:
  /// Installs the built-in operators. 1 is lowest precedence.
  OperatorTable() {
    Precedence['='] = 2;
    Precedence['<'] = 10;
    Precedence['+'] = 20;
    Precedence['-'] = 20;
    Precedence['*'] = 40; // highest.
  }

  /// Returns the precedence of Tok, or -1 if it is not a declared binop.
  int getPrecedence(int Tok) const {
    if (!isascii(Tok))
      return -1;
    auto It = Precedence.find(static_cast<char>(Tok));
    if (It == Precedence.end() || It->second <= 0)
      return -1;
    return It->second;
  }

  void setPrecedence(char Op, int Prec) { Precedence[Op] = Prec; }
  void erase(char Op) { Precedence.erase(Op); }
};

} // namespace kaleidoscope

#endif
