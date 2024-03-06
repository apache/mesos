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

#include "linux/ebpf.hpp"

#include <linux/bpf.h>
#include <sys/syscall.h>

#include <string>
#include <vector>

#include "stout/check.hpp"
#include "stout/error.hpp"
#include "stout/try.hpp"

using std::string;
using std::vector;

namespace ebpf {

Try<int, ErrnoError> bpf(int cmd, bpf_attr* attr, size_t size)
{
  // We retry the system call `attempts` times on EAGAIN. The default is 5,
  // as per what's done by libbpf:
  // https://github.com/libbpf/libbpf/blob/master/src/bpf.h#L71-L75
  int result, attempts = 5;
  do {
    // glibc does not expose the bpf() function, requiring us to make the
    // syscall directly: https://lwn.net/Articles/655028/
    result = (int)syscall(__NR_bpf, cmd, attr, size);
  } while (result == -1 && errno == EAGAIN && --attempts > 0);

  if (result == -1) {
    return ErrnoError();
  }
  return result;
}


Program::Program(bpf_prog_type _type) : type(_type) {}


void Program::append(vector<bpf_insn>&& instructions)
{
  program.insert(
      program.end(),
      std::make_move_iterator(instructions.begin()),
      std::make_move_iterator(instructions.end()));
}


Try<int> load(const Program& program)
{
  bpf_attr attribute;
  std::memset(&attribute, 0, sizeof(attribute));
  attribute.prog_type = program.type;
  attribute.insn_cnt = program.program.size();
  attribute.insns = reinterpret_cast<uint64_t>(program.program.data());
  attribute.license = reinterpret_cast<uint64_t>("Apache 2.0");

  Try<int, ErrnoError> fd = bpf(BPF_PROG_LOAD, &attribute, sizeof(attribute));

  if (fd.isError() && fd.error().code == EACCES) {
    // If bpf() fails with EACCES (a verifier error) the system call is called
    // again with an additional buffer to capture the verifier error logs.
    string verifier_logs(8196, '\0');
    attribute.log_level = 1;
    attribute.log_buf = reinterpret_cast<uint64_t>(&verifier_logs.front());
    attribute.log_size = verifier_logs.size();

    fd = bpf(BPF_PROG_LOAD, &attribute, sizeof(attribute));

    CHECK_ERROR(fd);
    CHECK_EQ(EACCES, fd.error().code)
      << "Expected BPF syscall to fail again with EACCES";
    return Error(string("BPF verifier failed: ") + verifier_logs.c_str());
  }

  if (fd.isError()) {
    return Error("Unexpected error from BPF syscall: " + fd.error().message);
  }

  return *fd;
}

} // namespace ebpf {
