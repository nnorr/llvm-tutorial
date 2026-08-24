#ifndef KALEIDOSCOPE_ASTVISITOR_H
#define KALEIDOSCOPE_ASTVISITOR_H

#include "AST.h"

#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"

namespace kaleidoscope {

/// ASTVisitor - Dispatches an ExprAST to a per-node hook on Derived.
///
/// CRTP plus a switch over the Kind discriminator, the shape Clang uses in
/// clang/AST/StmtVisitor.h. Dispatch is not virtual, so RetTy is free to vary
/// per visitor: CodeGen returns llvm::Value*, ASTDumper returns void.
///
///
/// Derived declares its hooks private and befriends this class.
template <typename Derived, typename RetTy = void> class ASTVisitor {
  Derived &derived() { return *static_cast<Derived *>(this); }

public:
  RetTy visit(ExprAST &E) {
    switch (E.getKind()) {
    case ExprAST::Expr_Number:
      return derived().visitNumber(llvm::cast<NumberExprAST>(E));
    case ExprAST::Expr_Variable:
      return derived().visitVariable(llvm::cast<VariableExprAST>(E));
    case ExprAST::Expr_Unary:
      return derived().visitUnary(llvm::cast<UnaryExprAST>(E));
    case ExprAST::Expr_Binary:
      return derived().visitBinary(llvm::cast<BinaryExprAST>(E));
    case ExprAST::Expr_Assign:
      return derived().visitAssign(llvm::cast<AssignExprAST>(E));
    case ExprAST::Expr_Call:
      return derived().visitCall(llvm::cast<CallExprAST>(E));
    case ExprAST::Expr_If:
      return derived().visitIf(llvm::cast<IfExprAST>(E));
    case ExprAST::Expr_For:
      return derived().visitFor(llvm::cast<ForExprAST>(E));
    case ExprAST::Expr_Var:
      return derived().visitVar(llvm::cast<VarExprAST>(E));
    }
    // No default: a new ExprASTKind makes -Wswitch fire here.
    llvm_unreachable("unknown ExprASTKind");
  }
};

} // namespace kaleidoscope

#endif
