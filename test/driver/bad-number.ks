# The lexer rejects a malformed literal instead of silently reading 1.23 and
# dropping the rest, which is what the upstream tutorial does.
#
# A rejected token still lexes to 0.0 so the parse continues, which means the
# diagnostic alone is not enough -- the driver has to consult Lexer::hadError()
# or it writes a .o where bad() returns 0.0. This test pins both halves: the
# message, and the non-zero exit.
#
# RUN: rm -f %t.o
# RUN: not %toy -c %s -o %t.o 2>&1 | %filecheck %s
# RUN: not test -e %t.o

def bad() 1.23.45;

# CHECK: invalid number literal '1.23.45'
