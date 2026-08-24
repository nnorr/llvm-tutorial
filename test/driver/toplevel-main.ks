# A top-level expression becomes main(), so the object has a real entry point.
#
# RUN: %toy -c %s --emit-llvm -o /dev/null 2>&1 | %filecheck %s

def sq(x) x * x;
sq(7);

# CHECK-LABEL: define double @sq(double %x)
# CHECK-LABEL: define double @main()
# CHECK:         call double @sq(double 7.000000e+00)
