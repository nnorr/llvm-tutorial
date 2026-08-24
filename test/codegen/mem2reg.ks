# Mutable variables are emitted as stack slots and promoted by mem2reg, so no
# alloca survives and the loop carries its state in PHI nodes instead.
#
# RUN: %toy -c %s --emit-llvm -o /dev/null 2>&1 | %filecheck %s

def binary : 1 (x y) y;
def fib(n)
  var a = 0, b = 1, c in
  (for i = 1, i < n in
     (c = a + b : a = b : b = c)) : b;

# CHECK-LABEL: define double @fib(double %n)
# CHECK-NOT:     alloca
# CHECK:         phi double
# CHECK:         phi double
# CHECK:         phi double
# CHECK:         fadd double
