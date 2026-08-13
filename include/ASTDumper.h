#ifndef KALEIDOSCOPE_ASTDUMPER_H
#define KALEIDOSCOPE_ASTDUMPER_H

#include "AST.h"
#include "ASTVisitor.h"

#include <ostream>

namespace kaleidoscope {

/// ASTDumper - Prints the AST as an indented tree.
///
/// This exists to justify the visitor pattern as much as to be useful: it is a
/// second, completely independent consumer of the same nodes, and it needs no
/// LLVM headers at all. In the tutorial, dump() was a virtual method on every
/// node sitting right beside codegen(), so the AST carried both concerns; here
/// neither one is in the AST.
///
/// Contrast with CodeGen: that visitor produces a value and so needs the
/// Result-member trick. This one produces only output, so plain void visit()
/// methods suffice -- the two show both shapes a visitor can take.
class ASTDumper : public ASTVisitor {
  std::ostream &Out;
  int Indent = 0;

  /// Writes the current indentation, then a node label with its source
  /// location.
  std::ostream &line(const char *Label);

  /// Dumps a child under a caption, one level deeper.
  void child(const char *Caption, ExprAST &E);

public:
  explicit ASTDumper(std::ostream &Out) : Out(Out) {}

  void dump(ExprAST &E);
  void dump(PrototypeAST &P);
  void dump(FunctionAST &F);

  void visit(NumberExprAST &E) override;
  void visit(VariableExprAST &E) override;
  void visit(UnaryExprAST &E) override;
  void visit(BinaryExprAST &E) override;
  void visit(AssignExprAST &E) override;
  void visit(CallExprAST &E) override;
  void visit(IfExprAST &E) override;
  void visit(ForExprAST &E) override;
  void visit(VarExprAST &E) override;
};

} // namespace kaleidoscope

#endif
