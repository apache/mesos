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

# PROTO_TO_INCLUDE_DIR is a convenience function that will: (1) compile .proto
# files found in the Mesos public-facing `include/` directory, (2) place the
# generated files in the build folder, but with an identical directory
# structure, and (3) export variables holding the fully qualified path to the
# generated files, based on a name structure the user specifies with the
# `BASE_NAME` and `BASE_DIR_STRUCTURE` parameters.
#
# For example, if suppose wish to compile `include/mesos/mesos.proto`. We might
# pass in the following values for the parameters:
#
#   BASE_NAME:          MESOS       (i.e., basis for the exported var names)
#   BASE_DIR_STRUCTURE: mesos/mesos (i.e., where `mesos/mesos.proto` would be
#                                    the relative path to the .proto file, we'd
#                                    use this "root name" to generate files
#                                    like `mesos/mesos.pb.cc`
#
# In this case, this function would:
#
#   (1) compile the `include/mesos/mesos.proto`, which would generate the files
#       `build/include/mesos/mesos.pb.h` and `build/include/mesos/mesos.pb.cc`
#   (2) export the following variables, based on the `BASE_NAME` parameter
#       (a) MESOS_PROTO:    ${MESOS_ROOT}/include/mesos/mesos.proto
#       (b) MESOS_PROTO_CC: ${MESOS_ROOT}/build/include/mesos/mesos.pb.cc
#       (a) MESOS_PROTO_H:   ${MESOS_ROOT}/build/include/mesos/mesos.pb.h
function(PROTOC_TO_INCLUDE_DIR BASE_NAME BASE_DIR_STRUCTURE)

  set(TO_INCLUDE_DIR
    -I${MESOS_PUBLIC_INCLUDE_DIR}
    -I${MESOS_SRC_DIR}
    --cpp_out=${MESOS_BIN_INCLUDE_DIR})

  # Names of variables we will be publicly exporting.
  set(PROTO_VAR ${BASE_NAME}_PROTO)    # e.g., MESOS_PROTO
  set(CC_VAR    ${BASE_NAME}_PROTO_CC) # e.g., MESOS_PROTO_CC
  set(H_VAR     ${BASE_NAME}_PROTO_H)  # e.g., MESOS_PROTO_H

  # Fully qualified paths for the input .proto files and the output C files.
  set(PROTO ${MESOS_PUBLIC_INCLUDE_DIR}/${BASE_DIR_STRUCTURE}.proto)
  set(CC    ${MESOS_BIN_INCLUDE_DIR}/${BASE_DIR_STRUCTURE}.pb.cc)
  set(H     ${MESOS_BIN_INCLUDE_DIR}/${BASE_DIR_STRUCTURE}.pb.h)

  # Export variables holding the target filenames.
  set(${PROTO_VAR} ${PROTO} PARENT_SCOPE) # e.g., mesos/mesos.proto
  set(${CC_VAR}    ${CC}    PARENT_SCOPE) # e.g., mesos/mesos.pb.cc
  set(${H_VAR}     ${H}     PARENT_SCOPE) # e.g., mesos/mesos.pb.h

  # Compile the .proto file.
  ADD_CUSTOM_COMMAND(
    OUTPUT ${CC} ${H}
    COMMAND ${PROTOC} ${TO_INCLUDE_DIR} ${PROTO}
    DEPENDS make_bin_include_dir ${PROTO}
    WORKING_DIRECTORY ${MESOS_BIN})
endfunction()

# PROTO_TO_SRC_DIR is similar to `PROTO_TO_INCLUDE_DIR`, except it acts on the
# Mesos `src/` directory instead of the public-facing `include/` directory (see
# documentation for `PROTO_TO_INCLUDE_DIR` for details).
function(PROTOC_TO_SRC_DIR BASE_NAME BASE_DIR_STRUCTURE)

  set(TO_SRC_DIR
    -I${MESOS_PUBLIC_INCLUDE_DIR}
    -I${MESOS_SRC_DIR}
    --cpp_out=${MESOS_BIN_SRC_DIR})

  # Names of variables we will be publicly exporting.
  set(PROTO_VAR ${BASE_NAME}_PROTO)    # e.g., MESOS_PROTO
  set(CC_VAR    ${BASE_NAME}_PROTO_CC) # e.g., MESOS_PROTO_CC
  set(H_VAR     ${BASE_NAME}_PROTO_H)  # e.g., MESOS_PROTO_H

  # Fully qualified paths for the input .proto files and the output C files.
  set(PROTO ${MESOS_SRC_DIR}/${BASE_DIR_STRUCTURE}.proto)
  set(CC    ${MESOS_BIN_SRC_DIR}/${BASE_DIR_STRUCTURE}.pb.cc)
  set(H     ${MESOS_BIN_SRC_DIR}/${BASE_DIR_STRUCTURE}.pb.h)

  # Export variables holding the target filenames.
  set(${PROTO_VAR} ${PROTO} PARENT_SCOPE) # e.g., mesos/mesos.proto
  set(${CC_VAR}    ${CC}    PARENT_SCOPE) # e.g., mesos/mesos.pb.cc
  set(${H_VAR}     ${H}     PARENT_SCOPE) # e.g., mesos/mesos.pb.h

  # Compile the .proto file.
  ADD_CUSTOM_COMMAND(
    OUTPUT ${CC} ${H}
    COMMAND ${PROTOC} ${TO_SRC_DIR} ${PROTO}
    DEPENDS make_bin_src_dir ${PROTO}
    WORKING_DIRECTORY ${MESOS_BIN})
endfunction()
