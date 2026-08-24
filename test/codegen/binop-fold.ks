# Kaleidoscope has no bool, so a comparison is widened to double and compared
# against 0.0 to branch. InstCombine folds that round trip back to the original
# fcmp; without the passes it stays in the IR (see debuginfo/no-opt.ks).
#
# RUN: %toy -c %s --emit-llvm -o /dev/null 2>&1 | %filecheck %s

def loop(n) for i = 1, i < n in i;

# CHECK-LABEL: define double @loop(double %n)
# CHECK:         fcmp ult double
# CHECK-NOT:     uitofp
# CHECK-NOT:     fcmp one
# CHECK:         br i1
