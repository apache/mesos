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

#include <stout/path.hpp>

#include <stout/os/mkdir.hpp>

#include "uri/fetchers/hadoop.hpp"

using std::set;
using std::string;

using process::Failure;
using process::Future;
using process::Owned;

namespace mesos {
namespace uri {

HadoopFetcherPlugin::Flags::Flags()
{
  add(&Flags::hadoop,
      "hadoop",
      "The path to the hadoop client\n");
}


Try<Owned<Fetcher::Plugin>> HadoopFetcherPlugin::create(const Flags& flags)
{
  Try<Owned<HDFS>> hdfs = HDFS::create(flags.hadoop);
  if (hdfs.isError()) {
    return Error("Failed to create HDFS client: " + hdfs.error());
  }

  return Owned<Fetcher::Plugin>(new HadoopFetcherPlugin(hdfs.get()));
}


set<string> HadoopFetcherPlugin::schemes()
{
  return {"hdfs", "hftp", "s3", "s3n"};
}


Future<Nothing> HadoopFetcherPlugin::fetch(
    const URI& uri,
    const string& directory)
{
  // TODO(jieyu): Validate the given URI.

  if (!uri.has_path()) {
    return Failure("URI path is not specified");
  }

  Try<Nothing> mkdir = os::mkdir(directory);
  if (mkdir.isError()) {
    return Failure(
        "Failed to create directory '" +
        directory + "': " + mkdir.error());
  }

  // NOTE: We ignore the scheme prefix if the host in URI is not
  // specified. This is the case when the host is set using the hadoop
  // configuration file.
  //
  // TODO(jieyu): Allow user to specify the name of the output file.
  return hdfs.get()->copyToLocal(
      (uri.has_host() ? stringify(uri) : uri.path()),
      path::join(directory, Path(uri.path()).basename()));
}

} // namespace uri {
} // namespace mesos {
