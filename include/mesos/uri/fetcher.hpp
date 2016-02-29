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

#ifndef __MESOS_URI_FETCHER_HPP__
#define __MESOS_URI_FETCHER_HPP__

#include <set>
#include <string>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/hashmap.hpp>
#include <stout/nothing.hpp>
#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <mesos/uri/uri.hpp>
#include "uri/fetchers/curl.hpp"
#include "uri/fetchers/docker.hpp"
#include "uri/fetchers/hadoop.hpp"

namespace mesos {
namespace uri {


/**
 * Provides an abstraction for fetching URIs. It is pluggable through
 * plugins. Each plugin is responsible for one or more URI schemes,
 * but there should be only one plugin associated with each URI
 * scheme. The fetching request will be dispatched to the relevant
 * plugin based on the scheme in the URI.
 */
class Fetcher
{
public:
  /**
   * Represents a fetcher plugin that handles one or more URI schemes.
   */
  class Plugin
  {
  public:
    virtual ~Plugin() {}

    /**
     * Returns the URI schemes that this plugin handles.
     */
    virtual std::set<std::string> schemes() = 0;

    /**
     * Fetches a URI to the given directory. To avoid blocking or
     * crashing the current thread, this method might choose to fork
     * subprocesses for third party commands.
     *
     * @param uri the URI to fetch
     * @param directory the directory the URI will be downloaded to
     */
    virtual process::Future<Nothing> fetch(
        const URI& uri,
        const std::string& directory) = 0;
  };

  /**
   * Create the Fetcher instance with the given plugins.
   *
   * @param _plugins a URI scheme to plugin map
   */
  Fetcher(const hashmap<std::string, process::Owned<Plugin>>& _plugins)
    : plugins(_plugins) {}

  /**
   * Fetches a URI to the given directory. This method will dispatch
   * the call to the corresponding plugin based on uri.scheme.
   *
   * @param uri the URI to fetch
   * @param directory the directory the URI will be downloaded to
   */
  // TODO(jieyu): Consider using 'Path' for 'directory' here.
  process::Future<Nothing> fetch(
      const URI& uri,
      const std::string& directory) const;

private:
  Fetcher(const Fetcher&) = delete; // Not copyable.
  Fetcher& operator=(const Fetcher&) = delete; // Not assignable.

  hashmap<std::string, process::Owned<Plugin>> plugins;
};

namespace fetcher {

/**
 * The combined flags for all built-in plugins.
 */
class Flags :
  public CurlFetcherPlugin::Flags,
  public HadoopFetcherPlugin::Flags,
  public DockerFetcherPlugin::Flags {};


/**
 * Factory method for creating a Fetcher instance.
 */
Try<process::Owned<Fetcher>> create(const Option<Flags>& flags = None());

} // namespace fetcher {
} // namespace uri {
} // namespace mesos {

#endif // __MESOS_URI_FETCHER_HPP__
