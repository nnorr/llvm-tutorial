#include "Parser.h"

#include <cctype>
#include <cstdio>

namespace kaleidoscope {

namespace {

/// LogError* - Little helper functions for error handling. File-local: nothing
/// outside the parser needs them. CodeGen has its own logError returning
/// Value*.
std::unique_ptr<ExprAST> logError(const char *Str) {
  fprintf(stderr, "Error: %s\n", Str);
  return nullptr;
}

std::unique_ptr<PrototypeAST> logErrorP(const char *Str) {
  logError(Str);
  return nullptr;
}

} // namespace

Parser::Parser(Lexer &Lex, OperatorTable &Ops) : Lex(Lex), Ops(Ops) {
  getNextToken();
}

/// numberexpr ::= number
std::unique_ptr<ExprAST> Parser::parseNumberExpr() {
  SourceLocation Loc = Lex.getCurLoc();
  auto Result = std::make_unique<NumberExprAST>(Loc, Lex.getNumVal());
  getNextToken(); // consume the number
  return Result;
}

/// parenexpr ::= '(' expression ')'
std::unique_ptr<ExprAST> Parser::parseParenExpr() {
  getNextToken(); // eat (.
  auto V = parseExpression();
  if (!V)
    return nullptr;

  if (CurTok != ')')
    return logError("expected ')'");
  getNextToken(); // eat ).
  return V;
}

/// identifierexpr
///   ::= identifier
///   ::= identifier '(' expression* ')'
std::unique_ptr<ExprAST> Parser::parseIdentifierExpr() {
  std::string IdName = Lex.getIdentifierStr();
  SourceLocation LitLoc = Lex.getCurLoc();

  getNextToken(); // eat identifier.

  if (CurTok != '(') // Simple variable ref.
    return std::make_unique<VariableExprAST>(LitLoc, IdName);

  // Call.
  getNextToken(); // eat (
  std::vector<std::unique_ptr<ExprAST>> Args;
  if (CurTok != ')') {
    while (true) {
      if (auto Arg = parseExpression())
        Args.push_back(std::move(Arg));
      else
        return nullptr;

      if (CurTok == ')')
        break;

      if (CurTok != ',')
        return logError("Expected ')' or ',' in argument list");
      getNextToken();
    }
  }

  // Eat the ')'.
  getNextToken();

  return std::make_unique<CallExprAST>(LitLoc, IdName, std::move(Args));
}

/// ifexpr ::= 'if' expression 'then' expression 'else' expression
std::unique_ptr<ExprAST> Parser::parseIfExpr() {
  SourceLocation IfLoc = Lex.getCurLoc();

  getNextToken(); // eat the if.

  auto Cond = parseExpression();
  if (!Cond)
    return nullptr;

  if (CurTok != tok_then)
    return logError("expected then");
  getNextToken(); // eat the then

  auto Then = parseExpression();
  if (!Then)
    return nullptr;

  if (CurTok != tok_else)
    return logError("expected else");
  getNextToken();

  auto Else = parseExpression();
  if (!Else)
    return nullptr;

  return std::make_unique<IfExprAST>(IfLoc, std::move(Cond), std::move(Then),
                                     std::move(Else));
}

/// forexpr ::= 'for' identifier '=' expr ',' expr (',' expr)? 'in' expression
std::unique_ptr<ExprAST> Parser::parseForExpr() {
  // The tutorial let ForExprAST default its location to whatever CurLoc had
  // reached by the time the node was built -- i.e. the end of the body. Taking
  // it at the 'for' keyword gives more useful debug line info.
  SourceLocation ForLoc = Lex.getCurLoc();

  getNextToken(); // eat the for.

  if (CurTok != tok_identifier)
    return logError("expected identifier after for");

  std::string IdName = Lex.getIdentifierStr();
  getNextToken(); // eat identifier.

  if (CurTok != '=')
    return logError("expected '=' after for");
  getNextToken(); // eat '='.

  auto Start = parseExpression();
  if (!Start)
    return nullptr;
  if (CurTok != ',')
    return logError("expected ',' after for start value");
  getNextToken();

  auto End = parseExpression();
  if (!End)
    return nullptr;

  // The step value is optional.
  std::unique_ptr<ExprAST> Step;
  if (CurTok == ',') {
    getNextToken();
    Step = parseExpression();
    if (!Step)
      return nullptr;
  }

  if (CurTok != tok_in)
    return logError("expected 'in' after for");
  getNextToken(); // eat 'in'.

  auto Body = parseExpression();
  if (!Body)
    return nullptr;

  return std::make_unique<ForExprAST>(ForLoc, IdName, std::move(Start),
                                      std::move(End), std::move(Step),
                                      std::move(Body));
}

/// varexpr ::= 'var' identifier ('=' expression)?
///                    (',' identifier ('=' expression)?)* 'in' expression
std::unique_ptr<ExprAST> Parser::parseVarExpr() {
  SourceLocation VarLoc = Lex.getCurLoc();

  getNextToken(); // eat the var.

  std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;

  // At least one variable name is required.
  if (CurTok != tok_identifier)
    return logError("expected identifier after var");

  while (true) {
    std::string Name = Lex.getIdentifierStr();
    getNextToken(); // eat identifier.

    // Read the optional initializer.
    std::unique_ptr<ExprAST> Init;
    if (CurTok == '=') {
      getNextToken(); // eat the '='.

      Init = parseExpression();
      if (!Init)
        return nullptr;
    }

    VarNames.emplace_back(Name, std::move(Init));

    // End of var list, exit loop.
    if (CurTok != ',')
      break;
    getNextToken(); // eat the ','.

    if (CurTok != tok_identifier)
      return logError("expected identifier list after var");
  }

  // At this point, we have to have 'in'.
  if (CurTok != tok_in)
    return logError("expected 'in' keyword after 'var'");
  getNextToken(); // eat 'in'.

  auto Body = parseExpression();
  if (!Body)
    return nullptr;

  return std::make_unique<VarExprAST>(VarLoc, std::move(VarNames),
                                      std::move(Body));
}

/// primary
///   ::= identifierexpr | numberexpr | parenexpr | ifexpr | forexpr | varexpr
std::unique_ptr<ExprAST> Parser::parsePrimary() {
  switch (CurTok) {
  default:
    return logError("unknown token when expecting an expression");
  case tok_identifier:
    return parseIdentifierExpr();
  case tok_number:
    return parseNumberExpr();
  case '(':
    return parseParenExpr();
  case tok_if:
    return parseIfExpr();
  case tok_for:
    return parseForExpr();
  case tok_var:
    return parseVarExpr();
  }
}

/// unary
///   ::= primary
///   ::= '!' unary
std::unique_ptr<ExprAST> Parser::parseUnary() {
  // If the current token is not an operator, it must be a primary expr.
  if (!isascii(CurTok) || CurTok == '(' || CurTok == ',')
    return parsePrimary();

  // If this is a unary operator, read it.
  SourceLocation OpLoc = Lex.getCurLoc();
  int Opc = CurTok;
  getNextToken();
  if (auto Operand = parseUnary())
    return std::make_unique<UnaryExprAST>(OpLoc, static_cast<char>(Opc),
                                          std::move(Operand));
  return nullptr;
}

/// binoprhs ::= ('+' unary)*
std::unique_ptr<ExprAST> Parser::parseBinOpRHS(int ExprPrec,
                                               std::unique_ptr<ExprAST> LHS) {
  // If this is a binop, find its precedence.
  while (true) {
    int TokPrec = getTokPrecedence();

    // If this is a binop that binds at least as tightly as the current binop,
    // consume it, otherwise we are done.
    if (TokPrec < ExprPrec)
      return LHS;

    // Okay, we know this is a binop.
    int BinOp = CurTok;
    SourceLocation BinLoc = Lex.getCurLoc();
    getNextToken(); // eat binop

    // Parse the unary expression after the binary operator.
    auto RHS = parseUnary();
    if (!RHS)
      return nullptr;

    // If BinOp binds less tightly with RHS than the operator after RHS, let
    // the pending operator take RHS as its LHS.
    int NextPrec = getTokPrecedence();
    if (TokPrec < NextPrec) {
      RHS = parseBinOpRHS(TokPrec + 1, std::move(RHS));
      if (!RHS)
        return nullptr;
    }

    // Merge LHS/RHS.
    if (BinOp == '=') {
      // "the destination of '=' must be an identifier" is a syntactic rule, so
      // enforce it here rather than in codegen. Using LLVM-style RTTI
      // (Kind + classof) rather than C++ dynamic_cast, since LLVM is normally
      // built -fno-rtti and the AST already carries the discriminator.
      //
      // Doing this in the parser means AssignExprAST can store the name
      // directly, so codegen has no cast and no failure path at all -- an
      // assignment to a non-variable is unrepresentable, not merely rejected.
      auto *LHSVar = llvm::dyn_cast<VariableExprAST>(LHS.get());
      if (!LHSVar)
        return logError("destination of '=' must be a variable");

      LHS = std::make_unique<AssignExprAST>(BinLoc, LHSVar->getName(),
                                            std::move(RHS));
    } else {
      LHS = std::make_unique<BinaryExprAST>(BinLoc, static_cast<char>(BinOp),
                                            std::move(LHS), std::move(RHS));
    }
  }
}

/// expression ::= unary binoprhs
std::unique_ptr<ExprAST> Parser::parseExpression() {
  auto LHS = parseUnary();
  if (!LHS)
    return nullptr;

  return parseBinOpRHS(0, std::move(LHS));
}

/// prototype
///   ::= id '(' id* ')'
///   ::= binary LETTER number? (id, id)
///   ::= unary LETTER (id)
std::unique_ptr<PrototypeAST> Parser::parsePrototype() {
  std::string FnName;

  SourceLocation FnLoc = Lex.getCurLoc();

  unsigned Kind = 0; // 0 = identifier, 1 = unary, 2 = binary.
  unsigned BinaryPrecedence = 30;

  switch (CurTok) {
  default:
    return logErrorP("Expected function name in prototype");
  case tok_identifier:
    FnName = Lex.getIdentifierStr();
    Kind = 0;
    getNextToken();
    break;
  case tok_unary:
    getNextToken();
    if (!isascii(CurTok))
      return logErrorP("Expected unary operator");
    FnName = "unary";
    FnName += static_cast<char>(CurTok);
    Kind = 1;
    getNextToken();
    break;
  case tok_binary:
    getNextToken();
    if (!isascii(CurTok))
      return logErrorP("Expected binary operator");
    FnName = "binary";
    FnName += static_cast<char>(CurTok);
    Kind = 2;
    getNextToken();

    // Read the precedence if present.
    if (CurTok == tok_number) {
      double Val = Lex.getNumVal();
      if (Val < 1 || Val > 100)
        return logErrorP("Invalid precedence: must be 1..100");
      BinaryPrecedence = static_cast<unsigned>(Val);
      getNextToken();
    }
    break;
  }

  if (CurTok != '(')
    return logErrorP("Expected '(' in prototype");

  std::vector<std::string> ArgNames;
  while (getNextToken() == tok_identifier)
    ArgNames.push_back(Lex.getIdentifierStr());
  if (CurTok != ')')
    return logErrorP("Expected ')' in prototype");

  // success.
  getNextToken(); // eat ')'.

  // Verify right number of names for operator.
  if (Kind && ArgNames.size() != Kind)
    return logErrorP("Invalid number of operands for operator");

  return std::make_unique<PrototypeAST>(FnLoc, FnName, std::move(ArgNames),
                                        Kind != 0, BinaryPrecedence);
}

/// definition ::= 'def' prototype expression
std::unique_ptr<FunctionAST> Parser::parseDefinition() {
  getNextToken(); // eat def.
  auto Proto = parsePrototype();
  if (!Proto)
    return nullptr;

  if (auto E = parseExpression())
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  return nullptr;
}

/// toplevelexpr ::= expression
std::unique_ptr<FunctionAST> Parser::parseTopLevelExpr(const std::string &Name) {
  SourceLocation FnLoc = Lex.getCurLoc();
  if (auto E = parseExpression()) {
    // Wrap the expression in a zero-argument function.
    auto Proto = std::make_unique<PrototypeAST>(FnLoc, Name,
                                                std::vector<std::string>());
    return std::make_unique<FunctionAST>(std::move(Proto), std::move(E));
  }
  return nullptr;
}

/// external ::= 'extern' prototype
std::unique_ptr<PrototypeAST> Parser::parseExtern() {
  getNextToken(); // eat extern.
  return parsePrototype();
}

} // namespace kaleidoscope
