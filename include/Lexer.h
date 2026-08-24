#ifndef KALEIDOSCOPE_LEXER_H
#define KALEIDOSCOPE_LEXER_H

#include "SourceLocation.h"

#include <istream>
#include <string>

namespace kaleidoscope {

enum Token {
  tok_eof = -1,

  // commands
  tok_def = -2,
  tok_extern = -3,

  // primary
  tok_identifier = -4,
  tok_number = -5,

  // control
  tok_if = -6,
  tok_then = -7,
  tok_else = -8,
  tok_for = -9,
  tok_in = -10,

  // operators
  tok_binary = -11,
  tok_unary = -12,

  // var definition
  tok_var = -13
};

/// Human-readable name for a token, for diagnostics.
std::string getTokName(int Tok);

class Lexer {
  std::istream &In;
  int LastChar = ' ';

  SourceLocation LexLoc{1, 0};
  SourceLocation CurLoc;

  std::string IdentifierStr; // Filled in if tok_identifier
  double NumVal = 0.0;       // Filled in if tok_number
  bool HadError = false;

  int advance();

public:
  explicit Lexer(std::istream &In) : In(In) {}

  int gettok();

  const std::string &getIdentifierStr() const { return IdentifierStr; }
  double getNumVal() const { return NumVal; }
  SourceLocation getCurLoc() const { return CurLoc; }

  /// True if any token was rejected. A bad token still yields a usable value
  /// so lexing can continue, so the driver has to ask before trusting the
  /// result; the REPL ignores it and keeps going.
  bool hadError() const { return HadError; }
};

} // namespace kaleidoscope

#endif
