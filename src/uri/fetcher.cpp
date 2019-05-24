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
using std::vector;

using process::Failure;
using process::Future;
using process::Owned;
using process::Shared;

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

  hashmap<string, Creator> creators = {
    {CurlFetcherPlugin::NAME,
       [flags]() { return CurlFetcherPlugin::create(flags); }},
    {CopyFetcherPlugin::NAME,
       [flags]() { return CopyFetcherPlugin::create(flags); }},
    {HadoopFetcherPlugin::NAME,
       [flags]() { return HadoopFetcherPlugin::create(flags); }},
#ifndef __WINDOWS__
    {DockerFetcherPlugin::NAME,
       [flags]() { return DockerFetcherPlugin::create(flags); }},
#endif // __WINDOWS__
  };

  vector<Owned<Fetcher::Plugin>> plugins;

  foreachpair (const string& name, const Creator& creator, creators) {
    Try<Owned<Fetcher::Plugin>> plugin = creator();
    if (plugin.isError()) {
      // NOTE: We skip the plugin if it cannot be created, instead of
      // returning an Error so that we can still use other plugins.
      LOG(INFO) << "Skipping URI fetcher plugin " << "'" << name << "' "
                << "as it could not be created: " << plugin.error();
      continue;
    }

    plugins.push_back(plugin.get());
  }

  return Owned<Fetcher>(new Fetcher(plugins));
}

} // namespace fetcher {

Fetcher::Fetcher(const vector<Owned<Plugin>>& plugins)
{
  foreach (Owned<Plugin> _plugin, plugins) {
    Shared<Plugin> plugin = _plugin.share();

    if (pluginsByName.contains(plugin->name())) {
      LOG(WARNING) << "Multiple URI fetcher plugins register "
                   << "under name '" << plugin->name() << "'";
    }

    pluginsByName[plugin->name()] = plugin;

    foreach (const string& scheme, plugin->schemes()) {
      if (pluginsByScheme.contains(scheme)) {
        LOG(WARNING) << "Multiple URI fetcher plugins register "
                     << "URI scheme '" << scheme << "'";
      }

      pluginsByScheme[scheme] = plugin;
    }
  }
}


Future<Nothing> Fetcher::fetch(
    const URI& uri,
    const string& directory,
    const Option<string>& data,
    const Option<string>& outputFileName) const
{
  if (!pluginsByScheme.contains(uri.scheme())) {
    return Failure("Scheme '" + uri.scheme() + "' is not supported");
  }

  return pluginsByScheme.at(uri.scheme())->fetch(
      uri,
      directory,
      data,
      outputFileName);
}


Future<Nothing> Fetcher::fetch(
    const URI& uri,
    const string& directory,
    const string& name,
    const Option<string>& data,
    const Option<string>& outputFileName) const
{
  if (!pluginsByName.contains(name)) {
    return Failure("Plugin  '" + name + "' is not registered.");
  }

  return pluginsByName.at(name)->fetch(uri, directory, data, outputFileName);
}

} // namespace uri {
} // namespace mesos {
