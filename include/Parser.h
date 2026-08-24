#ifndef KALEIDOSCOPE_PARSER_H
#define KALEIDOSCOPE_PARSER_H

#include "AST.h"
#include "Lexer.h"
#include "OperatorTable.h"

#include <memory>

namespace kaleidoscope {

class Parser {
  Lexer &Lex;
  OperatorTable &Ops;

  int CurTok = 0;

  int getNextToken() { return CurTok = Lex.gettok(); }

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
  Parser(Lexer &Lex, OperatorTable &Ops);

  int getCurTok() const { return CurTok; }

  void advance() { getNextToken(); }

  std::unique_ptr<FunctionAST> parseDefinition();
  std::unique_ptr<PrototypeAST> parseExtern();
  std::unique_ptr<FunctionAST>
  parseTopLevelExpr(const std::string &Name = "__anon_expr");
};

} // namespace kaleidoscope

#endif
