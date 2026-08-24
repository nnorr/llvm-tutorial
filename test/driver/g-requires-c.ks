# -g describes a source file on disk, which the interactive JIT does not have,
# so it is rejected without -c.
#
# RUN: not %toy -g < %s 2>&1 | %filecheck %s

def sq(x) x * x;

# CHECK: -g requires -c
