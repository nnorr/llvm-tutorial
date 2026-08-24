#ifndef KALEIDOSCOPE_ASTDUMPER_H
#define KALEIDOSCOPE_ASTDUMPER_H

#include "AST.h"
#include "ASTVisitor.h"

#include <ostream>

namespace kaleidoscope {

/// ASTDumper - Prints the AST as an indented tree.
///
/// The second consumer of the nodes, and the one that needs no LLVM headers.
/// Instantiates ASTVisitor with the default RetTy of void.
class ASTDumper : public ASTVisitor<ASTDumper> {
  friend class ASTVisitor<ASTDumper>; // calls the hooks below

  std::ostream &Out;
  int Indent = 0;

  /// Writes the current indentation, then a node label with its source
  /// location.
  std::ostream &line(const char *Label);

  /// Dumps a child under a caption, one level deeper.
  void child(const char *Caption, ExprAST &E);

  /// Per-node hooks, reached through the inherited visit(ExprAST &).
  void visitNumber(NumberExprAST &E);
  void visitVariable(VariableExprAST &E);
  void visitUnary(UnaryExprAST &E);
  void visitBinary(BinaryExprAST &E);
  void visitAssign(AssignExprAST &E);
  void visitCall(CallExprAST &E);
  void visitIf(IfExprAST &E);
  void visitFor(ForExprAST &E);
  void visitVar(VarExprAST &E);

public:
  explicit ASTDumper(std::ostream &Out) : Out(Out) {}

  void dump(ExprAST &E);
  void dump(PrototypeAST &P);
  void dump(FunctionAST &F);
};

} // namespace kaleidoscope

#endif
