#ifndef KALEIDOSCOPE_OPERATORTABLE_H
#define KALEIDOSCOPE_OPERATORTABLE_H

#include <cctype>
#include <map>

namespace kaleidoscope {

class OperatorTable {
  std::map<char, int> Precedence;

public:
  OperatorTable() {
    Precedence['='] = 2;
    Precedence['<'] = 10;
    Precedence['+'] = 20;
    Precedence['-'] = 20;
    Precedence['*'] = 40; // highest.
  }

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
