/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <list>

#include <process/collect.hpp>
#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/io.hpp>
#include <process/process.hpp>
#include <process/subprocess.hpp>


#include <stout/foreach.hpp>
#include <stout/os.hpp>

#include "common/status_utils.hpp"

#include "slave/containerizer/mesos/provisioner/backends/copy.hpp"


using namespace process;

using std::string;
using std::list;
using std::vector;

namespace mesos {
namespace internal {
namespace slave {

class CopyBackendProcess : public Process<CopyBackendProcess>
{
public:
  Future<Nothing> provision(const vector<string>& layers, const string& rootfs);

  Future<bool> destroy(const string& rootfs);

private:
  Future<Nothing> _provision(string layer, const string& rootfs);
};


Try<Owned<Backend>> CopyBackend::create(const Flags&)
{
  return Owned<Backend>(new CopyBackend(
      Owned<CopyBackendProcess>(new CopyBackendProcess())));
}


CopyBackend::~CopyBackend()
{
  terminate(process.get());
  wait(process.get());
}


CopyBackend::CopyBackend(Owned<CopyBackendProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


Future<Nothing> CopyBackend::provision(
    const vector<string>& layers,
    const string& rootfs)
{
  return dispatch(
      process.get(), &CopyBackendProcess::provision, layers, rootfs);
}


Future<bool> CopyBackend::destroy(const string& rootfs)
{
  return dispatch(process.get(), &CopyBackendProcess::destroy, rootfs);
}


Future<Nothing> CopyBackendProcess::provision(
    const vector<string>& layers,
    const string& rootfs)
{
  if (layers.size() == 0) {
    return Failure("No filesystem layers provided");
  }

  if (os::exists(rootfs)) {
    return Failure("Rootfs is already provisioned");
  }

  Try<Nothing> mkdir = os::mkdir(rootfs);
  if (mkdir.isError()) {
    return Failure("Failed to create rootfs directory: " + mkdir.error());
  }

  list<Future<Nothing>> futures{Nothing()};

  foreach (const string layer, layers) {
    futures.push_back(
        futures.back().then(
            defer(self(), &Self::_provision, layer, rootfs)));
  }

  return collect(futures)
    .then([]() -> Future<Nothing> { return Nothing(); });
}


Future<Nothing> CopyBackendProcess::_provision(
  string layer,
  const string& rootfs)
{
  VLOG(1) << "Copying layer path '" << layer << "' to rootfs '" << rootfs
          << "'";

#ifdef __APPLE__
  if (!strings::endsWith(layer, "/")) {
    layer += "/";
  }

  // OSX cp doesn't support -T flag, but supports source trailing
  // slash so we only copy the content but not the folder.
  vector<string> args{"cp", "-a", layer, rootfs};
#else
  vector<string> args{"cp", "-aT", layer, rootfs};
#endif // __APPLE__

  Try<Subprocess> s = subprocess(
      "cp",
      args,
      Subprocess::PATH("/dev/null"),
      Subprocess::PATH("/dev/null"),
      Subprocess::PIPE());

  if (s.isError()) {
    return Failure("Failed to create 'cp' subprocess: " + s.error());
  }

  Subprocess cp = s.get();

  return cp.status()
    .then([cp](const Option<int>& status) -> Future<Nothing> {
      if (status.isNone()) {
        return Failure("Failed to reap subprocess to copy image");
      } else if (status.get() != 0) {
        return io::read(cp.err().get())
          .then([](const string& err) -> Future<Nothing> {
            return Failure("Failed to copy layer: " + err);
          });
      }

      return Nothing();
    });
}


Future<bool> CopyBackendProcess::destroy(const string& rootfs)
{
  vector<string> argv{"rm", "-rf", rootfs};

  Try<Subprocess> s = subprocess(
      "rm",
      argv,
      Subprocess::PATH("/dev/null"),
      Subprocess::FD(STDOUT_FILENO),
      Subprocess::FD(STDERR_FILENO));

  if (s.isError()) {
    return Failure("Failed to create 'rm' subprocess: " + s.error());
  }

  return s.get().status()
    .then([](const Option<int>& status) -> Future<bool> {
      if (status.isNone()) {
        return Failure("Failed to reap subprocess to destroy rootfs");
      } else if (status.get() != 0) {
        return Failure("Failed to destroy rootfs, exit status: " +
                       WSTRINGIFY(status.get()));
      }

      return true;
    });
}

} // namespace slave {
} // namespace internal {
} // namespace mesos {
