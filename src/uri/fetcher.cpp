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

#include <string>

#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/hashmap.hpp>

#include "uri/fetcher.hpp"

using std::string;

using process::Failure;
using process::Future;
using process::Owned;

namespace mesos {
namespace uri {
namespace fetcher {

Try<Owned<Fetcher>> create(const Option<Flags>& _flags)
{
  // Use the default flags if not specified.
  Flags flags;
  if (_flags.isSome()) {
    flags = _flags.get();
  }

  // Load built-in plugins.
  typedef lambda::function<Try<Owned<Fetcher::Plugin>>()> Creator;

  hashmap<string, Creator> creators;
  creators.put("curl", lambda::bind(&CurlFetcherPlugin::create, flags));
  creators.put("file", lambda::bind(&CopyFetcherPlugin::create, flags));
  creators.put("hadoop", lambda::bind(&HadoopFetcherPlugin::create, flags));
  creators.put("docker", lambda::bind(&DockerFetcherPlugin::create, flags));

  hashmap<string, Owned<Fetcher::Plugin>> plugins;

  foreachpair (const string& name, const Creator& creator, creators) {
    Try<Owned<Fetcher::Plugin>> plugin = creator();
    if (plugin.isError()) {
      // NOTE: We skip the plugin if it cannot be created, instead of
      // returning an Error so that we can still use other plugins.
      LOG(ERROR) << "Failed to create URI fetcher plugin "
                 << "'"  << name << "': " << plugin.error();
      continue;
    }

    foreach (const string& scheme, plugin.get()->schemes()) {
      if (plugins.contains(scheme)) {
        LOG(WARNING) << "Multiple URI fetcher plugins register "
                     << "URI scheme '" << scheme << "'";
      }

      plugins.put(scheme, plugin.get());
    }
  }

  return Owned<Fetcher>(new Fetcher(plugins));
}

} // namespace fetcher {


Future<Nothing> Fetcher::fetch(
    const URI& uri,
    const string& directory) const
{
  if (!plugins.contains(uri.scheme())) {
    return Failure("Scheme '" + uri.scheme() + "' is not supported");
  }

  return plugins.at(uri.scheme())->fetch(uri, directory);
}

} // namespace uri {
} // namespace mesos {
