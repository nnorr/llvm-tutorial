#ifndef KALEIDOSCOPE_AST_H
#define KALEIDOSCOPE_AST_H

#include "ASTVisitor.h"
#include "SourceLocation.h"

#include "llvm/Support/Casting.h"

#include <cassert>
#include <memory>
#include <string>
#include <utility>
#include <vector>

/// The AST carries no LLVM *IR* dependency -- no Value*, no IRBuilder, no
/// codegen(). Its one LLVM include is llvm/Support/Casting.h, a header-only
/// utility that provides isa<>/dyn_cast<>/cast<> on top of the hand-rolled
/// Kind discriminator below. This is the idiomatic LLVM alternative to C++
/// RTTI (LLVM is normally built -fno-rtti), and it is the same shape MLIR's
/// own Toy tutorial uses for its AST.
///
/// Unlike the tutorial, these classes are NOT in an anonymous namespace. There
/// they were confined to one translation unit; in a header an anonymous
/// namespace would give every .cpp its own distinct set of types, so
/// unique_ptr<ExprAST> in Parser.h would not be the same type as in main.cpp.
namespace kaleidoscope {

/// ExprAST - Base class for all expression nodes.
class ExprAST {
public:
  /// Discriminator for LLVM-style RTTI. Every concrete subclass gets a tag and
  /// a matching classof(), which is all isa<>/dyn_cast<> need.
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
  /// The tutorial defaults the location to the lexer's CurLoc global, which
  /// makes the AST depend upward on the Lexer. Here the parser must pass it.
  ExprAST(ExprASTKind Kind, SourceLocation Loc) : Kind(Kind), Loc(Loc) {}
  virtual ~ExprAST() = default;

  ExprASTKind getKind() const { return Kind; }

  virtual void accept(ASTVisitor &V) = 0;

  int getLine() const { return Loc.Line; }
  int getCol() const { return Loc.Col; }
};

/// NumberExprAST - Expression class for numeric literals like "1.0".
class NumberExprAST : public ExprAST {
  double Val;

public:
  NumberExprAST(SourceLocation Loc, double Val)
      : ExprAST(Expr_Number, Loc), Val(Val) {}
  void accept(ASTVisitor &V) override { V.visit(*this); }

  double getVal() const { return Val; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Number; }
};

/// VariableExprAST - Expression class for referencing a variable, like "a".
class VariableExprAST : public ExprAST {
  std::string Name;

public:
  VariableExprAST(SourceLocation Loc, std::string Name)
      : ExprAST(Expr_Variable, Loc), Name(std::move(Name)) {}
  void accept(ASTVisitor &V) override { V.visit(*this); }

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
  void accept(ASTVisitor &V) override { V.visit(*this); }

  char getOpcode() const { return Opcode; }
  ExprAST &getOperand() const { return *Operand; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Unary; }
};

/// BinaryExprAST - Expression class for a binary operator.
///
/// Assignment is NOT represented here -- see AssignExprAST.
class BinaryExprAST : public ExprAST {
  char Op;
  std::unique_ptr<ExprAST> LHS, RHS;

public:
  BinaryExprAST(SourceLocation Loc, char Op, std::unique_ptr<ExprAST> LHS,
                std::unique_ptr<ExprAST> RHS)
      : ExprAST(Expr_Binary, Loc), Op(Op), LHS(std::move(LHS)),
        RHS(std::move(RHS)) {}
  void accept(ASTVisitor &V) override { V.visit(*this); }

  char getOp() const { return Op; }
  ExprAST &getLHS() const { return *LHS; }
  ExprAST &getRHS() const { return *RHS; }

  static bool classof(const ExprAST *E) { return E->getKind() == Expr_Binary; }
};

/// AssignExprAST - Expression class for 'name = expr'.
///
/// The tutorial routes assignment through BinaryExprAST and then, in codegen,
/// casts the LHS to VariableExprAST to recover the name. But "the LHS of '='
/// must be an identifier" is a *syntactic* rule, so the parser can enforce it
/// and store the name directly. Codegen then needs no cast and no failure
/// path, and the invalid state (an assignment whose LHS is not a variable) is
/// unrepresentable rather than merely rejected later.
class AssignExprAST : public ExprAST {
  std::string Name;
  std::unique_ptr<ExprAST> Value;

public:
  AssignExprAST(SourceLocation Loc, std::string Name,
                std::unique_ptr<ExprAST> Value)
      : ExprAST(Expr_Assign, Loc), Name(std::move(Name)),
        Value(std::move(Value)) {}
  void accept(ASTVisitor &V) override { V.visit(*this); }

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
  void accept(ASTVisitor &V) override { V.visit(*this); }

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
  void accept(ASTVisitor &V) override { V.visit(*this); }

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
  void accept(ASTVisitor &V) override { V.visit(*this); }

  const std::string &getVarName() const { return VarName; }
  ExprAST &getStart() const { return *Start; }
  ExprAST &getEnd() const { return *End; }
  /// The step is optional; null means 1.0.
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
  void accept(ASTVisitor &V) override { V.visit(*this); }

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

  /// CodeGen takes ownership of the prototype so it can be re-declared into a
  /// later module. After this the node no longer owns it -- use the reference
  /// returned by getProto() before calling this.
  std::unique_ptr<PrototypeAST> takeProto() { return std::move(Proto); }
};

} // namespace kaleidoscope

#endif
