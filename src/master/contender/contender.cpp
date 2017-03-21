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

#include <mesos/master/contender.hpp>

#include <mesos/module/contender.hpp>

#include <mesos/zookeeper/url.hpp>

#include <stout/check.hpp>
#include <stout/os.hpp>

#include "master/contender/standalone.hpp"
#include "master/contender/zookeeper.hpp"

#include "module/manager.hpp"

using std::string;

namespace mesos {
namespace master {
namespace contender {

Try<MasterContender*> MasterContender::create(
    const Option<string>& zk_,
    const Option<string>& masterContenderModule_)
{
  if (masterContenderModule_.isSome()) {
    return modules::ModuleManager::create<MasterContender>(
        masterContenderModule_.get());
  }

  if (zk_.isNone()) {
    return new StandaloneMasterContender();
  }

  const string& zk = zk_.get();

  if (strings::startsWith(zk, "zk://")) {
    Try<zookeeper::URL> url = zookeeper::URL::parse(zk);
    if (url.isError()) {
      return Error(url.error());
    }
    if (url.get().path == "/") {
      return Error(
          "Expecting a (chroot) path for ZooKeeper ('/' is not supported)");
    }
    return new ZooKeeperMasterContender(url.get());
  } else if (strings::startsWith(zk, "file://")) {
    // Load the configuration out of a file. While Mesos and related
    // programs always use <stout/flags> to process the command line
    // arguments (and therefore file://) this entrypoint is exposed by
    // libmesos, with frameworks currently calling it and expecting it
    // to do the argument parsing for them which roughly matches the
    // argument parsing Mesos will do.
    // TODO(cmaloney): Rework the libmesos exposed APIs to expose
    // A "flags" endpoint where the framework can pass the command
    // line arguments and they will be parsed by <stout/flags> and the
    // needed flags extracted, and then change this interface to
    // require final values from the flags. This means that a
    // framework doesn't need to know how the flags are passed to
    // match mesos' command line arguments if it wants, but if it
    // needs to inspect/manipulate arguments, it can.
    LOG(WARNING) << "Specifying master election mechanism / ZooKeeper URL to "
                    "be read out of a file via 'file://' is deprecated inside "
                    "Mesos and will be removed in a future release.";
    const string& path = zk.substr(7);
    const Try<string> read = os::read(path);
    if (read.isError()) {
      return Error("Failed to read from file at '" + path + "'");
    }

    return create(strings::trim(read.get()));
  }

  CHECK(!strings::startsWith(zk, "file://"));

  return Error("Failed to parse '" + zk + "'");
}


MasterContender::~MasterContender() {}

} // namespace contender {
} // namespace master {
} // namespace mesos {
