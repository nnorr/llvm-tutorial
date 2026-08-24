# A user-defined operator becomes an ordinary function with a mangled name, and
# uses of it lower to a direct call.
#
# RUN: %toy -c %s --emit-llvm -o /dev/null 2>&1 | %filecheck %s

def unary!(v) if v then 0 else 1;
def binary > 10 (LHS RHS) RHS < LHS;
def test(x) !(x > 2);

# CHECK-LABEL: define double @"unary!"(double %v)
# CHECK-LABEL: define double @"binary>"(double %LHS, double %RHS)
# CHECK-LABEL: define double @test(double %x)
# CHECK:         call double @"binary>"(double %x, double 2.000000e+00)
# CHECK:         call double @"unary!"(
