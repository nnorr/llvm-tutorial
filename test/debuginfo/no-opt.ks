# -g disables the IR passes, because mem2reg would delete the allocas that
# dbg_declare points at. So the stack slots and the bool round trip that
# codegen/mem2reg.ks and codegen/binop-fold.ks assert are gone both survive
# here. See docs/10-debuginfo-and-optimization.md.
#
# RUN: %toy -c %s -g --emit-llvm -o /dev/null 2>&1 | %filecheck %s

def binary : 1 (x y) y;
def loop(n) var a = 0 in (for i = 1, i < n in a = a + i) : a;

# CHECK-LABEL: define double @loop(double %n) !dbg
# CHECK:         alloca double
# CHECK-NOT:     phi double
# CHECK:         uitofp i1 {{.*}} to double
# CHECK:         fcmp one double
