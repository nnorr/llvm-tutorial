# lit configuration for the Kaleidoscope IR tests.
#
# Each test is a .ks file carrying its own RUN: line and CHECK: expectations,
# the way llvm/test works. Run the whole suite with `ctest -R lit`, or one file
# with `lit -v test/codegen/fib.ks`.

import os
import lit.formats

config.name = "Kaleidoscope"
config.test_format = lit.formats.ShTest(execute_external=True)
config.suffixes = [".ks"]
config.test_source_root = os.path.dirname(__file__)
config.test_exec_root = os.path.join(config.kaleidoscope_obj_root, "test")

# %toy is the driver under test; %filecheck resolves FileCheck wherever the
# toolchain put it (conda-forge hides it in libexec/llvm, not bin).
config.substitutions.append(("%toy", config.toy_binary))
config.substitutions.append(("%filecheck", config.filecheck_binary))

config.environment["PATH"] = os.path.pathsep.join(
    [os.path.dirname(config.filecheck_binary), config.environment.get("PATH", "")]
)
