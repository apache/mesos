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

#include <process/dispatch.hpp>
#include <process/process.hpp>

#include "uri/fetchers/docker.hpp"

using std::set;
using std::string;

using process::dispatch;
using process::spawn;
using process::terminate;
using process::wait;

using process::Failure;
using process::Future;
using process::Owned;
using process::Process;

namespace mesos {
namespace uri {

class DockerFetcherPluginProcess : public Process<DockerFetcherPluginProcess>
{
public:
  DockerFetcherPluginProcess() {}

  Future<Nothing> fetch(const URI& uri, const string& directory);
};


Try<Owned<Fetcher::Plugin>> DockerFetcherPlugin::create(const Flags& flags)
{
  Owned<DockerFetcherPluginProcess> process(
      new DockerFetcherPluginProcess());

  return Owned<Fetcher::Plugin>(new DockerFetcherPlugin(process));
}


DockerFetcherPlugin::DockerFetcherPlugin(
    Owned<DockerFetcherPluginProcess> _process)
  : process(_process)
{
  spawn(CHECK_NOTNULL(process.get()));
}


DockerFetcherPlugin::~DockerFetcherPlugin()
{
  terminate(process.get());
  wait(process.get());
}


set<string> DockerFetcherPlugin::schemes()
{
  return {"docker"};
}


Future<Nothing> DockerFetcherPlugin::fetch(
    const URI& uri,
    const string& directory)
{
  return dispatch(
      process.get(),
      &DockerFetcherPluginProcess::fetch,
      uri,
      directory);
}


Future<Nothing> DockerFetcherPluginProcess::fetch(
    const URI& uri,
    const string& directory)
{
  return Failure("Unimplemented");
}

} // namespace uri {
} // namespace mesos {
