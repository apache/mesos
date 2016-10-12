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

#include <iostream>
#include <string>

#include <stout/json.hpp>
#include <stout/none.hpp>
#include <stout/os.hpp>
#include <stout/protobuf.hpp>
#include <stout/try.hpp>

#include "slave/containerizer/mesos/isolators/network/cni/plugins/port_mapper/port_mapper.hpp"

namespace spec = mesos::internal::slave::cni::spec;

using std::cout;
using std::endl;
using std::string;

using process::Owned;

using mesos::internal::slave::cni::PortMapper;
using mesos::internal::slave::cni::spec::PluginError;


constexpr int STDIN_READ_LENGTH = 1000;


int main(int argc, char** argv)
{
  string config;

  // TODO(asridharan): Currently, `stout` doesn't support reading from
  // a file descriptor till it finds an EOF. Such a functionality
  // would be useful for cases such as reading from STDIN, where the
  // assumption is to read a limited number of bytes. We should
  // revisit this code once we have such functionality implemented in
  // `stout`. Tracking this in MESOS-6105.
  Result<string> input = os::read(STDIN_FILENO, STDIN_READ_LENGTH);
  while (input.isSome()) {
    config += input.get();
    input = os::read(STDIN_FILENO, STDIN_READ_LENGTH);
  }

  if (input.isError()) {
    cout << spec::error(input.error(), PortMapper::ERROR_READ_FAILURE) << endl;
    return EXIT_FAILURE;
  }

  Try<Owned<PortMapper>, PluginError> portMapper = PortMapper::create(config);
  if (portMapper.isError()) {
    cout << portMapper.error() << endl;
    return EXIT_FAILURE;
  }

  Try<Option<string>, PluginError> result = portMapper.get()->execute();
  if (result.isError()) {
    cout << result.error() << endl;
    return EXIT_FAILURE;
  } else if (result->isSome()) {
    cout << result->get() << endl;
  }

  return EXIT_SUCCESS;
}
