#ifndef KALEIDOSCOPE_LEXER_H
#define KALEIDOSCOPE_LEXER_H

#include "SourceLocation.h"

#include <istream>
#include <string>

namespace kaleidoscope {

/// The lexer returns tokens [0-255] if it is an unknown character, otherwise
/// one of these for known things.
///
/// A plain enum, not an enum class: gettok() returns int, negative for these
/// tokens and positive for a literal character, and the parser switches over
/// both in one statement.
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

/// Lexer - Turns a character stream into tokens, tracking line/column so the
/// parser can stamp AST nodes with source locations for debug info.
///
/// Takes an istream so tests can drive it from a std::istringstream.
class Lexer {
  std::istream &In;
  int LastChar = ' ';

  /// LexLoc is where the lexer currently is; CurLoc is the start of the token
  /// most recently returned by gettok().
  SourceLocation LexLoc{1, 0};
  SourceLocation CurLoc;

  std::string IdentifierStr; // Filled in if tok_identifier
  double NumVal = 0.0;       // Filled in if tok_number

  /// Reads one character, maintaining LexLoc.
  int advance();

public:
  explicit Lexer(std::istream &In) : In(In) {}

  /// Returns the next token from the stream.
  int gettok();

  const std::string &getIdentifierStr() const { return IdentifierStr; }
  double getNumVal() const { return NumVal; }
  SourceLocation getCurLoc() const { return CurLoc; }
};

} // namespace kaleidoscope

#endif
