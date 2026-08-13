#ifndef KALEIDOSCOPE_OPERATORTABLE_H
#define KALEIDOSCOPE_OPERATORTABLE_H

#include <cctype>
#include <map>

namespace kaleidoscope {

/// OperatorTable - Precedence for binary operators.
///
/// This is shared state, not owned by either the Parser or CodeGen. The Parser
/// reads it to drive ParseBinOpRHS; CodeGen *writes* it, because a user-defined
/// operator ("def binary | 5 (LHS RHS) ...") only becomes available once its
/// definition has been code-generated. In the single-file tutorial both sides
/// simply touched the `BinopPrecedence` global; pulling it out here breaks what
/// would otherwise be a Parser <-> CodeGen dependency cycle.
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
