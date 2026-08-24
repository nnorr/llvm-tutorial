#ifndef KALEIDOSCOPE_AST_H
#define KALEIDOSCOPE_AST_H

#include "SourceLocation.h"

#include "llvm/Support/Casting.h"

#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace kaleidoscope {

/// ExprAST - Base class for all expression nodes.
class ExprAST {
public:
  enum ExprASTKind {
    Expr_Number,
    Expr_Variable,
    Expr_Unary,
    Expr_Binary,
    Expr_Assign,
    Expr_Call,
    Expr_If,
    Expr_For,
    Expr_Var,
  };

private:
  const ExprASTKind Kind;
  SourceLocation Loc;

public:
  ExprAST(ExprASTKind Kind, SourceLocation Loc) : Kind(Kind), Loc(Loc) {}
  virtual ~ExprAST() = default;

  ExprASTKind getKind() const { return Kind; }

  int getLine() const { return Loc.Line; }
  int getCol() const { return Loc.Col; }
};

/// NumberExprAST - Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(SourceLocation Loc, double Val)
      : ExprAST(Expr_Number, Loc), Val(Val) {}
  double getVal() const { return Val; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Number; }
};

/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(SourceLocation Loc, std::string Name)
      : ExprAST(Expr_Variable, Loc), Name(std::move(Name)) {}
  const std::string &getName() const { return Name; }

  static bool classof(const ExprAST *E) {
    return E->getKind() == Expr_Variable;
  }
};

/// UnaryExprAST - Expression class for a unary operator.
class UnaryExprAST : public ExprAST {
  char Opcode;
  std::unique_ptr<ExprAST> Operand;

public:
  UnaryExprAST(SourceLocation Loc, char Opcode,
               std::unique_ptr<ExprAST> Operand)
      : ExprAST(Expr_Unary, Loc), Opcode(Opcode), Operand(std::move(Operand)) {}
  char getOpcode() const { return Opcode; }
  ExprAST &getOperand() const { return *Operand; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Unary; }
};

/// BinaryExprAST - Expression class for a binary operator.
///
class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(SourceLocation Loc, char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : ExprAST(Expr_Binary, Loc), Op(Op), LHS(std::move(LHS)),
        RHS(std::move(RHS)) {}
  char getOp() const { return Op; }
  ExprAST &getLHS() const { return *LHS; }
  ExprAST &getRHS() const { return *RHS; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Binary; }
};

/// AssignExprAST - Expression class for 'name = expr'.
///
/// Separate from BinaryExprAST: "the LHS of '=' is an identifier" is a
/// syntactic rule, so the parser enforces it and stores the name directly.
class AssignExprAST : public ExprAST {
  std::string Name;
  std::unique_ptr<ExprAST> Value;

public:
  AssignExprAST(SourceLocation Loc, std::string Name,
                std::unique_ptr<ExprAST> Value)
      : ExprAST(Expr_Assign, Loc), Name(std::move(Name)),
        Value(std::move(Value)) {}
  const std::string &getName() const { return Name; }
  ExprAST &getValue() const { return *Value; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Assign; }
};

/// CallExprAST - Expression class for function calls.
class CallExprAST : public ExprAST {
  std::string Callee;
  std::vector<std::unique_ptr<ExprAST>> Args;

public:
  CallExprAST(SourceLocation Loc, std::string Callee,
              std::vector<std::unique_ptr<ExprAST>> Args)
      : ExprAST(Expr_Call, Loc), Callee(std::move(Callee)),
        Args(std::move(Args)) {}
  const std::string &getCallee() const { return Callee; }
  const std::vector<std::unique_ptr<ExprAST>> &getArgs() const { return Args; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Call; }
};

/// IfExprAST - Expression class for if/then/else.
class IfExprAST : public ExprAST {
  std::unique_ptr<ExprAST> Cond, Then, Else;

public:
  IfExprAST(SourceLocation Loc, std::unique_ptr<ExprAST> Cond,
            std::unique_ptr<ExprAST> Then, std::unique_ptr<ExprAST> Else)
      : ExprAST(Expr_If, Loc), Cond(std::move(Cond)), Then(std::move(Then)),
        Else(std::move(Else)) {}
  ExprAST &getCond() const { return *Cond; }
  ExprAST &getThen() const { return *Then; }
  ExprAST &getElse() const { return *Else; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_If; }
};

/// ForExprAST - Expression class for for/in.
class ForExprAST : public ExprAST {
  std::string VarName;
  std::unique_ptr<ExprAST> Start, End, Step, Body;

public:
  ForExprAST(SourceLocation Loc, std::string VarName,
             std::unique_ptr<ExprAST> Start, std::unique_ptr<ExprAST> End,
             std::unique_ptr<ExprAST> Step, std::unique_ptr<ExprAST> Body)
      : ExprAST(Expr_For, Loc), VarName(std::move(VarName)),
        Start(std::move(Start)), End(std::move(End)), Step(std::move(Step)),
        Body(std::move(Body)) {}
  const std::string &getVarName() const { return VarName; }
  ExprAST &getStart() const { return *Start; }
  ExprAST &getEnd() const { return *End; }
  ExprAST *getStep() const { return Step.get(); }
  ExprAST &getBody() const { return *Body; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_For; }
};

/// VarExprAST - Expression class for var/in
class VarExprAST : public ExprAST {
  std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames;
  std::unique_ptr<ExprAST> Body;

public:
  VarExprAST(
      SourceLocation Loc,
      std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> VarNames,
      std::unique_ptr<ExprAST> Body)
      : ExprAST(Expr_Var, Loc), VarNames(std::move(VarNames)),
        Body(std::move(Body)) {}
  /// Each entry is (name, initializer); the initializer may be null, meaning
  /// the variable is initialized to 0.0.
  const std::vector<std::pair<std::string, std::unique_ptr<ExprAST>>> &
  getVarNames() const {
    return VarNames;
  }
  ExprAST &getBody() const { return *Body; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Var; }
};

/// PrototypeAST - This class represents the "prototype" for a function,
/// which captures its name, and its argument names (thus implicitly the number
/// of arguments the function takes), as well as if it is an operator.
///
/// Not an ExprAST, so it does not participate in the visitor.
class PrototypeAST {
  std::string Name;
  std::vector<std::string> Args;
  bool IsOperator;
  unsigned Precedence; // Precedence if a binary op.
  int Line;

public:
  PrototypeAST(SourceLocation Loc, std::string Name,
               std::vector<std::string> Args, bool IsOperator = false,
               unsigned Prec = 0)
      : Name(std::move(Name)), Args(std::move(Args)), IsOperator(IsOperator),
        Precedence(Prec), Line(Loc.Line) {}

  const std::string &getName() const { return Name; }
  const std::vector<std::string> &getArgs() const { return Args; }

  bool isUnaryOp() const { return IsOperator && Args.size() == 1; }
  bool isBinaryOp() const { return IsOperator && Args.size() == 2; }

  char getOperatorName() const {
    assert(isUnaryOp() || isBinaryOp());
    return Name[Name.size() - 1];
  }

  unsigned getBinaryPrecedence() const { return Precedence; }
  int getLine() const { return Line; }
};

/// FunctionAST - This class represents a function definition itself.
class FunctionAST {
  std::unique_ptr<PrototypeAST> Proto;
  std::unique_ptr<ExprAST> Body;

public:
  FunctionAST(std::unique_ptr<PrototypeAST> Proto,
              std::unique_ptr<ExprAST> Body)
      : Proto(std::move(Proto)), Body(std::move(Body)) {}

  PrototypeAST &getProto() const { return *Proto; }
  ExprAST &getBody() const { return *Body; }

  /// Hands the prototype to CodeGen, which re-declares it into later modules.
  /// The node no longer owns it afterwards; read getProto() first.
  std::unique_ptr<PrototypeAST> takeProto() { return std::move(Proto); }
};

} // namespace kaleidoscope

#endif
