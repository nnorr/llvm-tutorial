#include "ASTDumper.h"

#include <string>

namespace kaleidoscope {

std::ostream &ASTDumper::line(const char *Label) {
  Out << std::string(Indent * 2, ' ') << Label;
  return Out;
}

void ASTDumper::child(const char *Caption, ExprAST &E) {
  ++Indent;
  Out << std::string(Indent * 2, ' ') << Caption << '\n';
  ++Indent;
  E.accept(*this);
  Indent -= 2;
}

void ASTDumper::dump(ExprAST &E) { E.accept(*this); }

void ASTDumper::dump(PrototypeAST &P) {
  Out << std::string(Indent * 2, ' ') << "Prototype " << P.getName() << " (";
  bool First = true;
  for (const auto &Arg : P.getArgs()) {
    if (!First)
      Out << ' ';
    Out << Arg;
    First = false;
  }
  Out << ")";
  if (P.isBinaryOp())
    Out << " [binary '" << P.getOperatorName() << "' prec "
        << P.getBinaryPrecedence() << ']';
  else if (P.isUnaryOp())
    Out << " [unary '" << P.getOperatorName() << "']";
  Out << " @" << P.getLine() << '\n';
}

void ASTDumper::dump(FunctionAST &F) {
  Out << std::string(Indent * 2, ' ') << "Function\n";
  ++Indent;
  dump(F.getProto());
  Out << std::string(Indent * 2, ' ') << "Body:\n";
  ++Indent;
  F.getBody().accept(*this);
  Indent -= 2;
}

void ASTDumper::visit(NumberExprAST &E) {
  line("Number ") << E.getVal() << " @" << E.getLine() << ':' << E.getCol()
                     << '\n';
}

void ASTDumper::visit(VariableExprAST &E) {
  line("Variable ") << E.getName() << " @" << E.getLine() << ':'
                       << E.getCol() << '\n';
}

void ASTDumper::visit(UnaryExprAST &E) {
  line("Unary ") << '\'' << E.getOpcode() << "' @" << E.getLine() << ':'
                    << E.getCol() << '\n';
  child("Operand:", E.getOperand());
}

void ASTDumper::visit(BinaryExprAST &E) {
  line("Binary ") << '\'' << E.getOp() << "' @" << E.getLine() << ':'
                     << E.getCol() << '\n';
  child("LHS:", E.getLHS());
  child("RHS:", E.getRHS());
}

void ASTDumper::visit(AssignExprAST &E) {
  line("Assign ") << E.getName() << " @" << E.getLine() << ':' << E.getCol()
                     << '\n';
  child("Value:", E.getValue());
}

void ASTDumper::visit(CallExprAST &E) {
  line("Call ") << E.getCallee() << " @" << E.getLine() << ':' << E.getCol()
                   << '\n';
  for (const auto &Arg : E.getArgs())
    child("Arg:", *Arg);
}

void ASTDumper::visit(IfExprAST &E) {
  line("If") << " @" << E.getLine() << ':' << E.getCol() << '\n';
  child("Cond:", E.getCond());
  child("Then:", E.getThen());
  child("Else:", E.getElse());
}

void ASTDumper::visit(ForExprAST &E) {
  line("For ") << E.getVarName() << " @" << E.getLine() << ':' << E.getCol()
                  << '\n';
  child("Start:", E.getStart());
  child("End:", E.getEnd());
  if (ExprAST *Step = E.getStep())
    child("Step:", *Step);
  child("Body:", E.getBody());
}

void ASTDumper::visit(VarExprAST &E) {
  line("Var") << " @" << E.getLine() << ':' << E.getCol() << '\n';
  for (const auto &NamedVar : E.getVarNames()) {
    if (NamedVar.second)
      child((NamedVar.first + " =").c_str(), *NamedVar.second);
    else {
      ++Indent;
      Out << std::string(Indent * 2, ' ') << NamedVar.first << " = 0.0\n";
      --Indent;
    }
  }
  child("Body:", E.getBody());
}

} // namespace kaleidoscope
