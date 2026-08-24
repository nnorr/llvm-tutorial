//===----------------------------------------------------------------------===//
// Unit tests for the Lexer.
//
// No test framework: a handful of macros is enough, and it keeps the build
// dependency-free. Run with `ctest --test-dir build` or `./build/lexer_tests`.
//
// These are possible at all because Lexer reads from an std::istream rather
// than calling getchar(). The tutorial's lexer can only be exercised by piping
// text into the whole program and eyeballing the output.
//===----------------------------------------------------------------------===//

#include "Lexer.h"

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

using namespace kaleidoscope;

namespace {

int Checks = 0;
int Failures = 0;
const char *CurrentTest = "";

void fail(int Line, const std::string &What) {
  ++Failures;
  std::cerr << "  FAIL [" << CurrentTest << "] line " << Line << ": " << What
            << "\n";
}

#define CHECK(cond)                                                            \
  do {                                                                         \
    ++Checks;                                                                  \
    if (!(cond))                                                               \
      fail(__LINE__, #cond);                                                   \
  } while (0)

#define CHECK_EQ(actual, expected)                                             \
  do {                                                                         \
    ++Checks;                                                                  \
    auto A_ = (actual);                                                        \
    auto E_ = (expected);                                                      \
    if (!(A_ == E_)) {                                                         \
      std::ostringstream OS_;                                                  \
      OS_ << #actual << " == " << A_ << ", expected " << E_;                   \
      fail(__LINE__, OS_.str());                                               \
    }                                                                          \
  } while (0)

/// Lexes Input to EOF and returns the token sequence (excluding tok_eof).
std::vector<int> tokenize(const std::string &Input) {
  std::istringstream IS(Input);
  Lexer Lex(IS);
  std::vector<int> Toks;
  for (int T = Lex.gettok(); T != tok_eof; T = Lex.gettok())
    Toks.push_back(T);
  return Toks;
}

//===----------------------------------------------------------------------===//

/// 1. Keywords are recognized, and anything else alphanumeric is an identifier.
void testKeywordsAndIdentifiers() {
  auto Toks = tokenize("def extern if then else for in binary unary var foo x1");

  const std::vector<int> Expected = {
      tok_def,    tok_extern, tok_if,  tok_then,       tok_else,
      tok_for,    tok_in,     tok_binary, tok_unary,   tok_var,
      tok_identifier, tok_identifier};
  CHECK_EQ(Toks.size(), Expected.size());
  for (size_t I = 0; I < Expected.size() && I < Toks.size(); ++I)
    CHECK_EQ(Toks[I], Expected[I]);

  // A keyword must match in full: "define" is an identifier, not tok_def.
  std::istringstream IS("define defx");
  Lexer Lex(IS);
  CHECK_EQ(Lex.gettok(), tok_identifier);
  CHECK_EQ(Lex.getIdentifierStr(), std::string("define"));
  CHECK_EQ(Lex.gettok(), tok_identifier);
  CHECK_EQ(Lex.getIdentifierStr(), std::string("defx"));

  // Identifiers are [a-zA-Z][a-zA-Z0-9]* -- '_' is NOT included, so "a_b"
  // lexes as identifier, '_', identifier.
  auto Under = tokenize("a_b");
  CHECK_EQ(Under.size(), size_t(3));
  if (Under.size() == 3) {
    CHECK_EQ(Under[0], tok_identifier);
    CHECK_EQ(Under[1], int('_'));
    CHECK_EQ(Under[2], tok_identifier);
  }
}

/// 2. Numbers, including the malformed-literal case the tutorial gets wrong.
void testNumbers() {
  std::istringstream IS("3 3.25 .5 42.");
  Lexer Lex(IS);

  CHECK_EQ(Lex.gettok(), tok_number);
  CHECK_EQ(Lex.getNumVal(), 3.0);
  CHECK_EQ(Lex.gettok(), tok_number);
  CHECK_EQ(Lex.getNumVal(), 3.25);
  CHECK_EQ(Lex.gettok(), tok_number);
  CHECK_EQ(Lex.getNumVal(), 0.5);
  CHECK_EQ(Lex.gettok(), tok_number);
  CHECK_EQ(Lex.getNumVal(), 42.0);
  CHECK_EQ(Lex.gettok(), tok_eof);

  // The tutorial silently reads "1.23.45.67" as 1.23, dropping everything from
  // the second '.'. We reject it (and print a diagnostic to stderr).
  std::istringstream Bad("1.23.45.67");
  Lexer BadLex(Bad);
  CHECK_EQ(BadLex.gettok(), tok_number);
  CHECK_EQ(BadLex.getNumVal(), 0.0);
  CHECK_EQ(BadLex.gettok(), tok_eof);

  // A lone '.' is not a number either.
  std::istringstream Dot(".");
  Lexer DotLex(Dot);
  CHECK_EQ(DotLex.gettok(), tok_number);
  CHECK_EQ(DotLex.getNumVal(), 0.0);

  // The rejected token still lexes to 0.0 so the parse can continue, so the
  // failure is only visible through hadError(). Without it the compile driver
  // would emit an object for a program it had already complained about.
  CHECK(BadLex.hadError());
  CHECK(DotLex.hadError());
  CHECK(!Lex.hadError());

  // A number is still terminated correctly by a following token.
  auto Toks = tokenize("1.5+2");
  CHECK_EQ(Toks.size(), size_t(3));
  if (Toks.size() == 3) {
    CHECK_EQ(Toks[0], tok_number);
    CHECK_EQ(Toks[1], int('+'));
    CHECK_EQ(Toks[2], tok_number);
  }
}

/// 3. Unknown characters come back as their own ASCII value.
///
/// This is the design that makes the parser's switch work: negative values are
/// tokens, positive values are literal characters, both in one int.
void testOperatorsAreRawAscii() {
  auto Toks = tokenize("+-*/<>=(),;|&:!");
  const std::string Expected = "+-*/<>=(),;|&:!";
  CHECK_EQ(Toks.size(), Expected.size());
  for (size_t I = 0; I < Expected.size() && I < Toks.size(); ++I)
    CHECK_EQ(Toks[I], int(Expected[I]));

  // Every real token is negative, every character token is positive.
  for (int T : tokenize("def foo ( x ) x + 1"))
    CHECK(T < 0 || (T > 0 && T < 256));
}

/// 4. Whitespace, comments and end of input.
void testCommentsAndEof() {
  // A comment runs to end of line and produces no token.
  CHECK_EQ(tokenize("# nothing here").size(), size_t(0));

  auto Toks = tokenize("def # trailing comment\nfoo");
  CHECK_EQ(Toks.size(), size_t(2));
  if (Toks.size() == 2) {
    CHECK_EQ(Toks[0], tok_def);
    CHECK_EQ(Toks[1], tok_identifier);
  }

  // Empty and whitespace-only input yield tok_eof immediately.
  CHECK_EQ(tokenize("").size(), size_t(0));
  CHECK_EQ(tokenize("   \t\n  ").size(), size_t(0));

  // EOF is not consumed: asking repeatedly keeps returning tok_eof rather than
  // running off the end.
  std::istringstream IS("x");
  Lexer Lex(IS);
  CHECK_EQ(Lex.gettok(), tok_identifier);
  CHECK_EQ(Lex.gettok(), tok_eof);
  CHECK_EQ(Lex.gettok(), tok_eof);
  CHECK_EQ(Lex.gettok(), tok_eof);
}

/// 5. Source locations, which drive the DWARF line table.
void testSourceLocations() {
  std::istringstream IS("def foo\n  bar\nbaz");
  Lexer Lex(IS);

  CHECK_EQ(Lex.gettok(), tok_def);
  CHECK_EQ(Lex.getCurLoc().Line, 1);

  CHECK_EQ(Lex.gettok(), tok_identifier); // foo
  CHECK_EQ(Lex.getCurLoc().Line, 1);

  CHECK_EQ(Lex.gettok(), tok_identifier); // bar, on line 2
  CHECK_EQ(Lex.getCurLoc().Line, 2);

  CHECK_EQ(Lex.gettok(), tok_identifier); // baz, on line 3
  CHECK_EQ(Lex.getCurLoc().Line, 3);

  // Columns advance within a line and reset across one.
  std::istringstream Cols("a b\nc");
  Lexer ColLex(Cols);
  ColLex.gettok();
  int ACol = ColLex.getCurLoc().Col;
  ColLex.gettok();
  int BCol = ColLex.getCurLoc().Col;
  CHECK(BCol > ACol);
  ColLex.gettok();
  CHECK_EQ(ColLex.getCurLoc().Line, 2);
  CHECK(ColLex.getCurLoc().Col < BCol);
}

struct TestCase {
  const char *Name;
  void (*Run)();
};

const TestCase Tests[] = {
    {"keywords and identifiers", testKeywordsAndIdentifiers},
    {"numbers", testNumbers},
    {"operators are raw ascii", testOperatorsAreRawAscii},
    {"comments and eof", testCommentsAndEof},
    {"source locations", testSourceLocations},
};

} // namespace

int main() {
  std::cerr << "note: the 'numbers' test deliberately lexes bad literals, so "
               "diagnostics below are expected\n\n";

  for (const auto &T : Tests) {
    CurrentTest = T.Name;
    int Before = Failures;
    T.Run();
    std::cout << (Failures == Before ? "  ok   " : "  FAIL ") << T.Name << "\n";
  }

  std::cout << "\n" << Checks << " checks, " << Failures << " failed\n";
  return Failures == 0 ? 0 : 1;
}
