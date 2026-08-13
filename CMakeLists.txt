cmake_minimum_required(VERSION 3.20)
project(kaleidoscope CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Conda's activation script exports CXXFLAGS/CPPFLAGS containing -O2 -DNDEBUG,
# which CMake picks up as the *initial* value of CMAKE_CXX_FLAGS. For a project
# you are stepping through in a debugger that is the wrong default, so pin a
# build type unless the user asked for one.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# Setting the build type is not enough on its own: conda's -O2 lands in
# CMAKE_CXX_FLAGS, which is emitted *before* the per-config flags. Putting -O0
# in the Debug flags makes it win, since the last -O on the command line does.
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

find_package(LLVM REQUIRED CONFIG)
message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION} from ${LLVM_DIR}")

llvm_map_components_to_libnames(LLVM_LIBS
  core
  orcjit
  native
  passes
  analysis
  instcombine
  scalaropts
  transformutils
)

add_executable(toy
  src/main.cpp
  src/Lexer.cpp
  src/Parser.cpp
  src/CodeGen.cpp
  src/DebugInfo.cpp
  src/ObjectEmitter.cpp
)

target_include_directories(toy PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include
  ${LLVM_INCLUDE_DIRS}
)

target_compile_definitions(toy PRIVATE ${LLVM_DEFINITIONS})
target_link_libraries(toy PRIVATE ${LLVM_LIBS})

# The JIT resolves 'extern' declarations (putchard, printd) against the running
# process, so this executable's own symbols must be in the dynamic symbol
# table. Without this, `extern putchard(x);` links but fails to resolve.
set_target_properties(toy PROPERTIES ENABLE_EXPORTS ON)
