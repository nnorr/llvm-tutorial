# Only one top-level expression can be compiled -- a second would redefine
# main. The driver rejects it and exits non-zero.
#
# RUN: not %toy -c %s -o /dev/null 2>&1 | %filecheck %s

def sq(x) x * x;
sq(7);
sq(8);

# CHECK: only one top-level expression is supported when compiling
