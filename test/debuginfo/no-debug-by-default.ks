# Without -g there is no debug metadata at all: no !dbg on instructions, no
# compile unit, no subprogram.
#
# RUN: %toy -c %s --emit-llvm -o /dev/null 2>&1 | %filecheck %s

def sq(x) x * x;

# CHECK-LABEL: define double @sq(double %x)
# CHECK-NOT:   !dbg
# CHECK-NOT:   llvm.dbg.cu
# CHECK-NOT:   DISubprogram
