#include "Lexer.h"

#include <cctype>
#include <cstdlib>

namespace kaleidoscope {

std::string getTokName(int Tok) {
  switch (Tok) {
  case tok_eof:
    return "eof";
  case tok_def:
    return "def";
  case tok_extern:
    return "extern";
  case tok_identifier:
    return "identifier";
  case tok_number:
    return "number";
  case tok_if:
    return "if";
  case tok_then:
    return "then";
  case tok_else:
    return "else";
  case tok_for:
    return "for";
  case tok_in:
    return "in";
  case tok_binary:
    return "binary";
  case tok_unary:
    return "unary";
  case tok_var:
    return "var";
  }
  return std::string(1, static_cast<char>(Tok));
}

int Lexer::advance() {
  int C = In.get();
  if (C == std::istream::traits_type::eof())
    C = EOF;

  if (C == '\n' || C == '\r') {
    LexLoc.Line++;
    LexLoc.Col = 0;
  } else
    LexLoc.Col++;
  return C;
}

int Lexer::gettok() {
  // Skip any whitespace.
  while (isspace(LastChar))
    LastChar = advance();

  CurLoc = LexLoc;

  if (isalpha(LastChar)) { // identifier: [a-zA-Z][a-zA-Z0-9]*
    IdentifierStr = static_cast<char>(LastChar);
    while (isalnum((LastChar = advance())))
      IdentifierStr += static_cast<char>(LastChar);

    if (IdentifierStr == "def")
      return tok_def;
    if (IdentifierStr == "extern")
      return tok_extern;
    if (IdentifierStr == "if")
      return tok_if;
    if (IdentifierStr == "then")
      return tok_then;
    if (IdentifierStr == "else")
      return tok_else;
    if (IdentifierStr == "for")
      return tok_for;
    if (IdentifierStr == "in")
      return tok_in;
    if (IdentifierStr == "binary")
      return tok_binary;
    if (IdentifierStr == "unary")
      return tok_unary;
    if (IdentifierStr == "var")
      return tok_var;
    return tok_identifier;
  }

  if (isdigit(LastChar) || LastChar == '.') { // Number: [0-9.]+
    std::string NumStr;
    do {
      NumStr += static_cast<char>(LastChar);
      LastChar = advance();
    } while (isdigit(LastChar) || LastChar == '.');

    // The tutorial passes NumStr to strtod and ignores where it stopped, so
    // "1.23.45.67" silently becomes 1.23 -- everything from the second '.'
    // onward is swallowed by the token and lost without a word. Checking the
    // end pointer makes that loud instead of silent: strtod parses the longest
    // valid prefix, so if it did not land on the terminating NUL, the rest of
    // the run is junk.
    //
    // The whole [0-9.]+ run stays a single token rather than being re-split at
    // the second '.', so a typo produces one diagnostic here instead of a
    // confusing cascade of parse errors downstream.
    const char *Start = NumStr.c_str();
    char *End = nullptr;
    NumVal = strtod(Start, &End);

    if (End != Start + NumStr.size() || End == Start) {
      fprintf(stderr, "Error: invalid number literal '%s' at %d:%d\n",
              NumStr.c_str(), CurLoc.Line, CurLoc.Col);
      NumVal = 0.0;
    }
    return tok_number;
  }

  if (LastChar == '#') {
    // Comment until end of line.
    do
      LastChar = advance();
    while (LastChar != EOF && LastChar != '\n' && LastChar != '\r');

    if (LastChar != EOF)
      return gettok();
  }

  // Check for end of file.  Don't eat the EOF.
  if (LastChar == EOF)
    return tok_eof;

  // Otherwise, just return the character as its ascii value.
  int ThisChar = LastChar;
  LastChar = advance();
  return ThisChar;
}

} // namespace kaleidoscope
