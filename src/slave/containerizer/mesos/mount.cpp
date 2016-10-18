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

#include "slave/containerizer/mesos/mount.hpp"

#include <iostream>
#include <string>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

#ifdef __linux__
#include "linux/fs.hpp"
#endif // __linux__

using std::cerr;
using std::endl;
using std::string;

namespace mesos {
namespace internal {
namespace slave {

const string MesosContainerizerMount::NAME = "mount";
const string MesosContainerizerMount::MAKE_RSLAVE = "make-rslave";


MesosContainerizerMount::Flags::Flags()
{
  add(&Flags::operation,
      "operation",
      "The mount operation to apply.");

  add(&Flags::path,
      "path",
      "The path to apply mount operation to.");
}


int MesosContainerizerMount::execute()
{
  if (flags.help) {
    cerr << flags.usage();
    return EXIT_SUCCESS;
  }

#ifdef __linux__
  if (flags.operation.isNone()) {
    cerr << "Flag --operation is not specified" << endl;
    return 1;
  }

  if (flags.operation.get() == MAKE_RSLAVE) {
    if (flags.path.isNone()) {
      cerr << "Flag --path is required for " << MAKE_RSLAVE << endl;
      return 1;
    }

    Try<Nothing> mount = mesos::internal::fs::mount(
        None(),
        flags.path.get(),
        None(),
        MS_SLAVE | MS_REC,
        nullptr);

    if (mount.isError()) {
      cerr << "Failed to mark rslave with path '" << flags.path.get() << "': "
           << mount.error();
      return 1;
    }
  } else {
    cerr << "Unsupported mount operation '" << flags.operation.get() << "'";
    return 1;
  }

  return 0;
#else
  cerr << "Mount is only supported on Linux";

  return 1;
#endif // __linux__
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
