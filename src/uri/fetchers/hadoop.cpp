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
#include <stout/strings.hpp>

#include <stout/os/mkdir.hpp>

#include "uri/fetchers/hadoop.hpp"

using std::vector;
using std::set;
using std::string;

using process::Failure;
using process::Future;
using process::Owned;

namespace mesos {
namespace uri {

HadoopFetcherPlugin::Flags::Flags()
{
  add(&Flags::hadoop_client,
      "hadoop_client",
      "The path to the hadoop client\n");

  add(&Flags::hadoop_client_supported_schemes,
      "hadoop_client_supported_schemes",
      "A comma-separated list of the schemes supported by the hadoop client.\n",
      "hdfs,hftp,s3,s3n");
}


const char HadoopFetcherPlugin::NAME[] = "hadoop";


Try<Owned<Fetcher::Plugin>> HadoopFetcherPlugin::create(const Flags& flags)
{
  Try<Owned<HDFS>> hdfs = HDFS::create(flags.hadoop_client);
  if (hdfs.isError()) {
    return Error("Failed to create HDFS client: " + hdfs.error());
  }

  vector<string> schemes = strings::tokenize(
      flags.hadoop_client_supported_schemes, ",");

  return Owned<Fetcher::Plugin>(new HadoopFetcherPlugin(
      hdfs.get(),
      set<string>(schemes.begin(), schemes.end())));
}


set<string> HadoopFetcherPlugin::schemes() const
{
  return schemes_;
}


string HadoopFetcherPlugin::name() const
{
  return NAME;
}


Future<Nothing> HadoopFetcherPlugin::fetch(
    const URI& uri,
    const string& directory,
    const Option<string>& data,
    const Option<string>& outputFileName) const
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
  return hdfs->copyToLocal(
      (uri.has_host() ? stringify(uri) : uri.path()),
      path::join(directory, Path(uri.path()).basename()));
}

} // namespace uri {
} // namespace mesos {
