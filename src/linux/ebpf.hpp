// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// TODO(dleamy): Look into using libbpf: https://github.com/libbpf/libbpf
//               to simplify and or replace the low-level BPF operations.

#ifndef __EBPF_HPP__
#define __EBPF_HPP__

#include <linux/bpf.h>

#include <string>
#include <vector>

#include "stout/nothing.hpp"
#include "stout/try.hpp"

namespace ebpf {

// eBPF program.
class Program
{
public:
  explicit Program(bpf_prog_type type);

  // Append instructions to the end of the eBPF program.
  void append(std::vector<bpf_insn>&& instructions);

  // Type of eBPF program.
  const bpf_prog_type type;

  // Instructions of the eBPF program.
  std::vector<bpf_insn> program;
};


// Loads the provided eBPF program into the kernel and returns the file
// descriptor of loaded program.
Try<int> load(const Program& program);


namespace cgroups2 {

// Attaches the eBPF program identified by the provided fd to a cgroup.
//
// TODO(dleamy): This currently does not replace existing programs attached
// to the cgroup, we will need to add replacement to support adding / removing
// device access dynamically.
Try<Nothing> attach(int fd, const std::string& cgroup);

} // namespace cgroups2 {


// Utility macros for constructing eBPF instructions.
#define BPF_ALU32_IMM(OP, DST, IMM)       \
  ((bpf_insn){                            \
    .code = BPF_ALU | BPF_OP(OP) | BPF_K, \
    .dst_reg = DST,                       \
    .src_reg = 0,                         \
    .off = 0,                             \
    .imm = IMM})


#define BPF_LDX_MEM(SIZE, DST, SRC, OFF)        \
  ((bpf_insn){                                  \
    .code = BPF_LDX | BPF_SIZE(SIZE) | BPF_MEM, \
    .dst_reg = DST,                             \
    .src_reg = SRC,                             \
    .off = OFF,                                 \
    .imm = 0})


#define BPF_MOV64_REG(DST, SRC)          \
  ((bpf_insn){                           \
    .code = BPF_ALU64 | BPF_MOV | BPF_X, \
    .dst_reg = DST,                      \
    .src_reg = SRC,                      \
    .off = 0,                            \
    .imm = 0})


#define BPF_JMP_A(OFF)        \
  ((bpf_insn){                \
    .code = BPF_JMP | BPF_JA, \
    .dst_reg = 0,             \
    .src_reg = 0,             \
    .off = OFF,               \
    .imm = 0})


#define BPF_JMP_IMM(OP, DST, IMM, OFF)    \
  ((bpf_insn){                            \
    .code = BPF_JMP | BPF_OP(OP) | BPF_K, \
    .dst_reg = DST,                       \
    .src_reg = 0,                         \
    .off = OFF,                           \
    .imm = IMM})


#define BPF_JMP_REG(OP, DST, SRC, OFF)    \
  ((bpf_insn){                            \
    .code = BPF_JMP | BPF_OP(OP) | BPF_X, \
    .dst_reg = DST,                       \
    .src_reg = SRC,                       \
    .off = OFF,                           \
    .imm = 0})


#define BPF_MOV64_IMM(DST, IMM)          \
  ((bpf_insn){                           \
    .code = BPF_ALU64 | BPF_MOV | BPF_K, \
    .dst_reg = DST,                      \
    .src_reg = 0,                        \
    .off = 0,                            \
    .imm = IMM})


#define BPF_MOV32_REG(DST, SRC)        \
  ((bpf_insn){                         \
    .code = BPF_ALU | BPF_MOV | BPF_X, \
    .dst_reg = DST,                    \
    .src_reg = SRC,                    \
    .off = 0,                          \
    .imm = 0})


#define BPF_EXIT_INSN()         \
  ((bpf_insn){                  \
    .code = BPF_JMP | BPF_EXIT, \
    .dst_reg = 0,               \
    .src_reg = 0,               \
    .off = 0,                   \
    .imm = 0})

} // namespace ebpf {

#endif // __EBPF_HPP__
