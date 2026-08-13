#ifndef KALEIDOSCOPE_SOURCELOCATION_H
#define KALEIDOSCOPE_SOURCELOCATION_H

namespace kaleidoscope {

/// SourceLocation - A line/column position in the input stream. Produced by
/// the Lexer, carried by AST nodes, and consumed by DebugInfo when emitting
/// DILocation metadata.
struct SourceLocation {
  int Line = 1;
  int Col = 0;
};

} // namespace kaleidoscope

#endif
