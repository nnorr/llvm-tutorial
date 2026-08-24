# -g attaches a DISubprogram to every function, a DILocation to instructions,
# and describes each parameter with a dbg_declare pointing at its stack slot.
#
# RUN: %toy -c %s -g --emit-llvm -o /dev/null 2>&1 | %filecheck %s

def sq(x) x * x;

# CHECK:       define double @sq(double %x) !dbg
# CHECK:         alloca double
# CHECK:         #dbg_declare(ptr %x1
# CHECK:         fmul double {{.*}}, !dbg
#
# CHECK:       !llvm.dbg.cu = !{
# CHECK:       distinct !DICompileUnit(language: DW_LANG_C
# CHECK-SAME:    producer: "Kaleidoscope Compiler"
# CHECK:       distinct !DISubprogram(name: "sq"
# CHECK:       !DILocalVariable(name: "x", arg: 1
