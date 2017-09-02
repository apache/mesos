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
#       directory, or with the option `INTERNAL` the Mesos `src/` directory.
#       The `JAVA` option flag will generate the Java Protobuf files to
#       `src/java/generated`.
#   (2) Place the generated files in the build folder, but with an identical
#       directory structure.
#   (3) Append to list variables `PUBLIC_PROTOBUF_SRC`, `INTERNAL_PROTOBUF_SRC`,
#       and `JAVA_PROTOBUF_SRC` (depending on options passed in) the fully
#       qualified path to the generated files. This export is a *side effect*
#       and modifies the variables in the parent scope.
#
# For example, if suppose wish to compile `include/mesos/mesos.proto`,
# we might pass in the following values for the parameters:
#
#   PROTOC_GENERATE(TARGET mesos/mesos)
#
# Where `mesos/mesos.proto` would be the relative path to the .proto file,
# we'd use this "root name" to generate files like `mesos/mesos.pb.cc`
#
# In this case, this function would:
#
#   (1) Compile the `include/mesos/mesos.proto`, which would generate the files
#       `build/include/mesos/mesos.pb.h` and `build/include/mesos/mesos.pb.cc`.
#   (2) Append the path `${MESOS_ROOT}/build/include/mesos/mesos.pb.cc` to
#       the parent scope variable `PUBLIC_PROTOBUF_SRC`.
#
# NOTE: The `protoc` binary used here is an imported executable target from
# `3rdparty/CMakeLists.txt`. However, this is not strictly necessary, and
# `protoc` could be supplied in `PATH`.
function(PROTOC_GENERATE)
  set(options OPTIONAL INTERNAL JAVA)
  set(oneValueArgs TARGET)
  cmake_parse_arguments(PROTOC "${options}" "${oneValueArgs}" "" ${ARGN})

  if (PROTOC_INTERNAL)
    set(CPP_OUT ${MESOS_BIN_SRC_DIR})
  else ()
    set(CPP_OUT ${MESOS_BIN_INCLUDE_DIR})
  endif ()

  set(PROTOC_OPTIONS
    -I${MESOS_PUBLIC_INCLUDE_DIR}
    -I${MESOS_SRC_DIR}
    --cpp_out=${CPP_OUT})

  if (PROTOC_JAVA AND HAS_JAVA)
    list(APPEND PROTOC_OPTIONS
      --java_out=${MESOS_BIN_SRC_DIR}/java/generated)
  endif ()

  # Fully qualified paths for the input .proto files and the output C file.
  if (PROTOC_INTERNAL) # to src dir
    set(PROTO ${MESOS_SRC_DIR}/${PROTOC_TARGET}.proto)
  else () # to public include dir
    set(PROTO ${MESOS_PUBLIC_INCLUDE_DIR}/${PROTOC_TARGET}.proto)
  endif ()

  set(CC ${CPP_OUT}/${PROTOC_TARGET}.pb.cc)

  # Fully qualified path for the Java file.
  if (PROTOC_JAVA AND HAS_JAVA)
    get_filename_component(PROTOC_JAVA_DIR ${PROTOC_TARGET} DIRECTORY)
    set(JAVA ${MESOS_BIN_SRC_DIR}/java/generated/org/apache/${PROTOC_JAVA_DIR}/Protos.java)
  endif ()

  # Export variables holding the target filenames.
  if (PROTOC_INTERNAL)
    list(APPEND INTERNAL_PROTOBUF_SRC ${CC})
    set(INTERNAL_PROTOBUF_SRC ${INTERNAL_PROTOBUF_SRC} PARENT_SCOPE)
  else ()
    list(APPEND PUBLIC_PROTOBUF_SRC ${CC})
    set(PUBLIC_PROTOBUF_SRC ${PUBLIC_PROTOBUF_SRC} PARENT_SCOPE)
  endif ()

  if (PROTOC_JAVA AND HAS_JAVA)
    list(APPEND JAVA_PROTOBUF_SRC ${JAVA})
    set(JAVA_PROTOBUF_SRC ${JAVA_PROTOBUF_SRC} PARENT_SCOPE)
  endif ()

  if (PROTOC_INTERNAL)
    set(PROTOC_DEPENDS make_bin_src_dir)
  else ()
    set(PROTOC_DEPENDS make_bin_include_dir)
  endif ()

  if (PROTOC_JAVA AND HAS_JAVA)
    list(APPEND PROTOC_DEPENDS make_bin_java_dir)
  endif ()

  # Compile the .proto file.
  add_custom_command(
    OUTPUT ${CC} ${H} ${JAVA}
    COMMAND protoc ${PROTOC_OPTIONS} ${PROTO}
    DEPENDS ${PROTOC_DEPENDS} ${PROTO}
    WORKING_DIRECTORY ${MESOS_BIN})
endfunction()
