# SPDX-License-Identifier: Apache-2.0

add_onnx_mlir_executable(TestInstrumentation
  TestInstrumentation.cpp

  NO_INSTALL

  INCLUDE_DIRS PUBLIC
  ${ONNX_MLIR_SRC_ROOT}/include

  LINK_LIBS PRIVATE
  cruntime
  )

add_test(NAME TestInstrumentation COMMAND TestInstrumentation)

add_subdirectory(BType)
add_subdirectory(DisposableElementsAttr)
add_subdirectory(Einsum)
add_subdirectory(FloatingPoint16)
add_subdirectory(Runtime)
add_subdirectory(Strides)
