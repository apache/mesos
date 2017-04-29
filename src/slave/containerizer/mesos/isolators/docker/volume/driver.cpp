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

#include <glog/logging.h>

#include <process/collect.hpp>
#include <process/io.hpp>
#include <process/subprocess.hpp>

#include <stout/stringify.hpp>

#include <stout/os/constants.hpp>
#include <stout/os/killtree.hpp>

#include "common/status_utils.hpp"

#include "slave/containerizer/mesos/isolators/docker/volume/driver.hpp"

namespace io = process::io;

using std::string;
using std::tuple;
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Subprocess;

namespace mesos {
namespace internal {
namespace slave {
namespace docker {
namespace volume {

constexpr Duration MOUNT_TIMEOUT = Seconds(120);
constexpr Duration UNMOUNT_TIMEOUT = Minutes(10);

Try<Owned<DriverClient>> DriverClient::create(
    const string& dvdcli)
{
  return Owned<DriverClient>(new DriverClient(dvdcli));
}


Future<string> DriverClient::mount(
    const string& driver,
    const string& name,
    const hashmap<string, string>& options)
{
  // Refer to https://github.com/emccode/dvdcli for how dvdcli works.
  // TODO(gyliu513): Add `explicitcreate` if 'options' is None. Refer
  // to https://github.com/emccode/dvdcli/pull/20 for detail.
  vector<string> argv = {
    dvdcli,
    "mount",
    "--volumedriver=" + driver,
    "--volumename=" + name,
  };

  foreachpair (const string& key, const string& value, options) {
    argv.push_back("--volumeopts=" + key + "=" + value);
  }

  string command = strings::join(
      ", ",
      dvdcli,
      strings::join(", ", argv));

  VLOG(1) << "Invoking Docker Volume Driver 'mount' "
          << "command '" << command << "'";

  Try<Subprocess> s = subprocess(
      dvdcli,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      None(),
      None(),
      {},
      {Subprocess::ChildHook::SUPERVISOR()});

  if (s.isError()) {
    return Failure("Failed to execute '" + command + "': " + s.error());
  }

  return await(
      s->status(),
      io::read(s->out().get()),
      io::read(s->err().get()))
    .then([](const tuple<
        Future<Option<int>>,
        Future<string>,
        Future<string>>& t) -> Future<string> {
      const Future<Option<int>>& status = std::get<0>(t);
      if (!status.isReady()) {
        return Failure(
            "Failed to get the exit status of the subprocess: " +
            (status.isFailed() ? status.failure() : "discarded"));
      }

      if (status->isNone()) {
        return Failure("Failed to reap the subprocess");
      }

      if (status->get() != 0) {
        const Future<string>& error = std::get<2>(t);
        if (!error.isReady()) {
          return Failure(
              "Unexpected termination of the subprocess: " +
              WSTRINGIFY(status->get()));
        }

        return Failure(
            "Unexpected termination of the subprocess: " + error.get());
      }

      const Future<string>& output = std::get<1>(t);
      if (!output.isReady()) {
         return Failure(
            "Failed to read stdout from the subprocess: " +
            (output.isFailed() ? output.failure() : "discarded"));
      }

      return output;
    })
    .after(MOUNT_TIMEOUT, [s](Future<string> future) -> Future<string> {
      future.discard();
      os::killtree(s->pid(), SIGKILL);

      return Failure("'mount' timed out in " + stringify(MOUNT_TIMEOUT));
    });
}


Future<Nothing> DriverClient::unmount(
    const string& driver,
    const string& name)
{
  vector<string> argv = {
    dvdcli,
    "unmount",
    "--volumedriver=" + driver,
    "--volumename=" + name,
  };

  string command = strings::join(
      ", ",
      dvdcli,
      strings::join(", ", argv));

  VLOG(1) << "Invoking Docker Volume Driver 'unmount' "
          << "command '" << command << "'";

  Try<Subprocess> s = subprocess(
      dvdcli,
      argv,
      Subprocess::PATH(os::DEV_NULL),
      Subprocess::PIPE(),
      Subprocess::PIPE(),
      nullptr,
      None(),
      None(),
      {},
      {Subprocess::ChildHook::SUPERVISOR()});

  if (s.isError()) {
    return Failure("Failed to execute '" + command + "': " + s.error());
  }

  return await(
      s->status(),
      io::read(s->err().get()))
    .then([](const tuple<
        Future<Option<int>>,
        Future<string>>& t) -> Future<Nothing> {
      const Future<Option<int>>& status = std::get<0>(t);
      if (!status.isReady()) {
        return Failure(
            "Failed to get the exit status of the subprocess: " +
            (status.isFailed() ? status.failure() : "discarded"));
      }

      if (status->isNone()) {
        return Failure("Failed to reap the subprocess");
      }

      if (status->get() != 0) {
        const Future<string>& error = std::get<1>(t);
        if (!error.isReady()) {
          return Failure(
            "Unexpected termination of the subprocess: " +
            WSTRINGIFY(status->get()));
        }

        return Failure(
            "Unexpected termination of the subprocess: " + error.get());
      }

      return Nothing();
    })
    .after(UNMOUNT_TIMEOUT, [s](Future<Nothing> future) -> Future<Nothing> {
      future.discard();
      os::killtree(s->pid(), SIGKILL);

      return Failure("'unmount' timed out in " + stringify(UNMOUNT_TIMEOUT));
    });
}

} // namespace volume {
} // namespace docker {
} // namespace slave {
} // namespace internal {
} // namespace mesos {
