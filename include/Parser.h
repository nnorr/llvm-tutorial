#ifndef KALEIDOSCOPE_PARSER_H
#define KALEIDOSCOPE_PARSER_H

#include "AST.h"
#include "Lexer.h"
#include "OperatorTable.h"

#include <memory>

namespace kaleidoscope {

/// Parser - Recursive-descent parser with precedence climbing for binary
/// operators. Builds the AST; it is the only thing that constructs AST nodes.
///
/// CurTok is one-token lookahead and lives here rather than in the Lexer: the
/// Lexer stays a pure token source, buffering is a parsing concern.
class Parser {
  Lexer &Lex;
  OperatorTable &Ops;

  /// The current token the parser is looking at.
  int CurTok = 0;

  int getNextToken() { return CurTok = Lex.gettok(); }

  /// Precedence of the pending binary operator token, or -1.
  int getTokPrecedence() const { return Ops.getPrecedence(CurTok); }

  std::unique_ptr<ExprAST> parseExpression();
  std::unique_ptr<ExprAST> parseNumberExpr();
  std::unique_ptr<ExprAST> parseParenExpr();
  std::unique_ptr<ExprAST> parseIdentifierExpr();
  std::unique_ptr<ExprAST> parseIfExpr();
  std::unique_ptr<ExprAST> parseForExpr();
  std::unique_ptr<ExprAST> parseVarExpr();
  std::unique_ptr<ExprAST> parsePrimary();
  std::unique_ptr<ExprAST> parseUnary();
  std::unique_ptr<ExprAST> parseBinOpRHS(int ExprPrec,
                                         std::unique_ptr<ExprAST> LHS);
  std::unique_ptr<PrototypeAST> parsePrototype();

public:
  /// Primes the first token, so CurTok is valid immediately after construction.
  Parser(Lexer &Lex, OperatorTable &Ops);

  int getCurTok() const { return CurTok; }

  /// Skip a token -- used by the driver for error recovery and to eat
  /// top-level semicolons.
  void advance() { getNextToken(); }

  /// definition ::= 'def' prototype expression
  std::unique_ptr<FunctionAST> parseDefinition();
  /// external ::= 'extern' prototype
  std::unique_ptr<PrototypeAST> parseExtern();
  /// toplevelexpr ::= expression
  ///
  /// The synthesized wrapper is named \p Name. The JIT driver uses the default
  /// "__anon_expr" and looks it up to evaluate; the object-file driver passes
  /// "main" so the emitted .o has a real entry point. That difference is why
  /// Ch8/Ch9 could only accept one top-level expression -- a second would
  /// redefine main.
  std::unique_ptr<FunctionAST>
  parseTopLevelExpr(const std::string &Name = "__anon_expr");
};

} // namespace kaleidoscope

#endif
