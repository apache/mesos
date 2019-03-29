# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# PROTOC_GENERATE is a convenience function that will:
#   (1) Compile .proto files found in the Mesos public-facing `include/`
#       directory, or with the `INTERNAL` option the Mesos `src/` directory,
#       or with the `LIB` option the external library's include directory.
#   (2) Place the generated files in the build folder, under the `include/`
#       directory, or with the `INTERNAL` option the `src/` directory (cannot
#       be used in conjunction with the `LIB` option). The `JAVA` option will
#       generate the Java Protobuf files to `src/java/generated`. The `GRPC`
#       option will generate the `.grpc.pb.h` and `.grpc.pb.cc` files.
#   (3) With the `LIB` option, append to list variable `LIB_PROTO_PATHS` the
#       fully qualified path to the external library's include directory.
#   (4) Append to list variables `PUBLIC_PROTOBUF_SRC`, `INTERNAL_PROTOBUF_SRC`,
#       and `JAVA_PROTOBUF_SRC` (depending on options passed in) the fully
#       qualified path to the generated files. This export is a *side effect*
#       and modifies the variables in the parent scope.
#
# Example 1: Suppose we wish to compile `include/mesos/mesos.proto`, we might
# pass in the following values for the parameters:
#
#   PROTOC_GENERATE(TARGET mesos/mesos)
#
# Where `mesos/mesos.proto` would be the relative path to the .proto file,
# we'd use this "root name" to generate files like `mesos/mesos.pb.cc`. In this
# case, this function would:
#
#   (1) Compile `include/mesos/mesos.proto`, which would generate the files
#       `build/include/mesos/mesos.pb.h` and `build/include/mesos/mesos.pb.cc`.
#   (2) Append the path `${MESOS_ROOT}/build/include/mesos/mesos.pb.cc` to
#       the parent scope variable `PUBLIC_PROTOBUF_SRC`.
#
# Example 2: Suppose we wish to compile `csi/v0/csi.proto` in the `csi_v0`
# external library with gRPC enabled, we might pass in the following values for
# the parameters:
#
#   PROTOC_GENERATE(GRPC LIB csi_v0 TARGET csi/v0/csi)
#
# Where `csi/v0/csi.proto` would be the relative path to the .proto file in the
# `csi_v0` library's include directory. In this case, this function would:
#
#   (1) Compile `${CSI_V0_ROOT}/csi/v0/csi.proto`, which would generate the
#       files `build/include/csi/v0/csi.pb.h`, `build/include/csi/v0/csi.pb.cc`,
#       `build/include/csi/v0/csi.grpc.pb.h`, and
#       `build/include/csi/v0/csi.grpc.pb.cc`.
#   (2) Append the path `${CSI_V0_ROOT}` to the parent scope variable
#       `LIB_PROTO_PATHS`.
#   (3) Append the paths `${MESOS_ROOT}/build/include/csi/v0/csi.pb.cc` and
#       `${MESOS_ROOT}/build/include/csi/v0/csi.grpc.pb.cc` to the parent scope
#       variable `PUBLIC_PROTOBUF_SRC`.
#
# NOTE: The `protoc` binary used here is an imported executable target from
# `3rdparty/CMakeLists.txt`. However, this is not strictly necessary, and
# `protoc` could be supplied in `PATH`.
function(PROTOC_GENERATE)
  set(options OPTIONAL INTERNAL JAVA GRPC)
  set(oneValueArgs LIB TARGET)
  cmake_parse_arguments(PROTOC "${options}" "${oneValueArgs}" "" ${ARGN})

  # Fully qualified paths for the input .proto file and the output directories.
  if (PROTOC_LIB)
    if (PROTOC_INTERNAL)
      message(FATAL_ERROR
        "Cannot generate internal proto files from an external library")
    endif()

    get_target_property(
      LIB_INCLUDE_DIR
      ${PROTOC_LIB}
      INTERFACE_INCLUDE_DIRECTORIES)

    list(APPEND LIB_PROTO_PATHS ${LIB_INCLUDE_DIR})
    list(REMOVE_DUPLICATES LIB_PROTO_PATHS)
    set(LIB_PROTO_PATHS ${LIB_PROTO_PATHS} PARENT_SCOPE)

    set(PROTO ${LIB_INCLUDE_DIR}/${PROTOC_TARGET}.proto)
    set(CPP_OUT ${MESOS_BIN_INCLUDE_DIR})
  elseif (PROTOC_INTERNAL)
    set(PROTO ${MESOS_SRC_DIR}/${PROTOC_TARGET}.proto)
    set(CPP_OUT ${MESOS_BIN_SRC_DIR})
  else ()
    set(PROTO ${MESOS_PUBLIC_INCLUDE_DIR}/${PROTOC_TARGET}.proto)
    set(CPP_OUT ${MESOS_BIN_INCLUDE_DIR})
  endif()

  if (PROTOC_JAVA AND HAS_JAVA)
    set(JAVA_OUT ${MESOS_BIN_SRC_DIR}/java/generated)
  endif()

  get_target_property(
    PROTOBUF_INCLUDE_DIR
    protobuf
    INTERFACE_INCLUDE_DIRECTORIES)

  set(PROTOC_OPTIONS
    -I${MESOS_PUBLIC_INCLUDE_DIR}
    -I${MESOS_SRC_DIR}
    -I${PROTOBUF_INCLUDE_DIR}
    --cpp_out=${CPP_OUT})

  foreach (LIB_PROTO_PATH IN LISTS LIB_PROTO_PATHS)
    list(APPEND PROTOC_OPTIONS -I${LIB_PROTO_PATH})
  endforeach (LIB_PROTO_PATH)

  if (PROTOC_GRPC)
    list(APPEND PROTOC_OPTIONS
      --grpc_out=${CPP_OUT}
      --plugin=protoc-gen-grpc=$<TARGET_FILE:grpc_cpp_plugin>)
  endif ()

  if (JAVA_OUT)
    list(APPEND PROTOC_OPTIONS
      --java_out=${JAVA_OUT})
  endif ()

  # Fully qualified paths for the input .proto file and the output .pb.h and
  # .pb.cc files.
  set(CC ${CPP_OUT}/${PROTOC_TARGET}.pb.cc)
  set(H ${CPP_OUT}/${PROTOC_TARGET}.pb.h)

  if (PROTOC_GRPC)
    set(GRPC_CC ${CPP_OUT}/${PROTOC_TARGET}.grpc.pb.cc)
    set(GRPC_H ${CPP_OUT}/${PROTOC_TARGET}.grpc.pb.h)
  endif ()

  # Fully qualified path for the Java file.
  if (JAVA_OUT)
    get_filename_component(PROTOC_JAVA_DIR ${PROTOC_TARGET} DIRECTORY)
    set(JAVA ${JAVA_OUT}/org/apache/${PROTOC_JAVA_DIR}/Protos.java)
  endif ()

  # Export variables holding the target filenames.
  if (PROTOC_INTERNAL)
    list(APPEND INTERNAL_PROTOBUF_SRC ${CC} ${GRPC_CC})
    set(INTERNAL_PROTOBUF_SRC ${INTERNAL_PROTOBUF_SRC} PARENT_SCOPE)
  else ()
    list(APPEND PUBLIC_PROTOBUF_SRC ${CC} ${GRPC_CC})
    set(PUBLIC_PROTOBUF_SRC ${PUBLIC_PROTOBUF_SRC} PARENT_SCOPE)
  endif ()

  if (JAVA)
    list(APPEND JAVA_PROTOBUF_SRC ${JAVA})
    set(JAVA_PROTOBUF_SRC ${JAVA_PROTOBUF_SRC} PARENT_SCOPE)
  endif ()

  # Make the directory that generated files go into.
  if (PROTOC_INTERNAL)
    set(PROTOC_DEPENDS make_bin_src_dir)
  else ()
    set(PROTOC_DEPENDS make_bin_include_dir)
  endif ()

  if (PROTOC_LIB)
    list(APPEND PROTOC_DEPENDS ${PROTOC_LIB})
  endif ()

  if (JAVA_OUT)
    list(APPEND PROTOC_DEPENDS make_bin_java_dir)
  endif ()

  # Make sure that the gRPC plugin is built.
  if (PROTOC_GRPC)
    list(APPEND PROTOC_DEPENDS grpc_cpp_plugin)
  endif ()

  # Compile the .proto file.
  add_custom_command(
    OUTPUT ${CC} ${H} ${GRPC_CC} ${GRPC_H} ${JAVA}
    COMMAND protoc ${PROTOC_OPTIONS} ${PROTO}
    DEPENDS ${PROTOC_DEPENDS} ${PROTO}
    WORKING_DIRECTORY ${MESOS_BIN})
endfunction()
