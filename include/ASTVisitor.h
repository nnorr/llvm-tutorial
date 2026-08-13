#ifndef KALEIDOSCOPE_ASTVISITOR_H
#define KALEIDOSCOPE_ASTVISITOR_H

namespace kaleidoscope {

class NumberExprAST;
class VariableExprAST;
class UnaryExprAST;
class BinaryExprAST;
class CallExprAST;
class IfExprAST;
class ForExprAST;
class VarExprAST;

/// ASTVisitor - Double-dispatch interface over the expression nodes.
///
/// The tutorial hangs a virtual codegen() directly off each AST node, which is
/// why its IRBuilder and symbol tables had to be globals: the nodes had no way
/// to receive backend state. Here the nodes stay pure data and each consumer
/// (CodeGen, ASTDumper, ...) is a visitor carrying its own state.
///
/// Note there is no visit() for PrototypeAST or FunctionAST -- neither derives
/// from ExprAST, and consumers handle them through dedicated entry points.
///
/// Visitors that produce a value (CodeGen produces llvm::Value*) stash it in a
/// member and expose a typed wrapper, since visit() cannot return a type the
/// AST is allowed to know about.
class ASTVisitor {
public:
  virtual ~ASTVisitor() = default;

  virtual void visit(NumberExprAST &E) = 0;
  virtual void visit(VariableExprAST &E) = 0;
  virtual void visit(UnaryExprAST &E) = 0;
  virtual void visit(BinaryExprAST &E) = 0;
  virtual void visit(CallExprAST &E) = 0;
  virtual void visit(IfExprAST &E) = 0;
  virtual void visit(ForExprAST &E) = 0;
  virtual void visit(VarExprAST &E) = 0;
};

} // namespace kaleidoscope

#endif
