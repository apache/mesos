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

#ifndef __URI_FETCHER_HPP__
#define __URI_FETCHER_HPP__

#include <process/owned.hpp>

#include <stout/none.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

#include <mesos/uri/fetcher.hpp>

#include "uri/fetchers/copy.hpp"
#include "uri/fetchers/curl.hpp"
#include "uri/fetchers/hadoop.hpp"

#ifndef __WINDOWS__
#include "uri/fetchers/docker.hpp"
#endif // __WINDOWS__

namespace mesos {
namespace uri {
namespace fetcher {

/**
 * The combined flags for all built-in plugins.
 */
class Flags :
  public virtual CopyFetcherPlugin::Flags,
  public virtual CurlFetcherPlugin::Flags,
#ifndef __WINDOWS__
  public virtual DockerFetcherPlugin::Flags,
#endif // __WINDOWS__
  public virtual HadoopFetcherPlugin::Flags {};


/**
 * Factory method for creating a Fetcher instance.
 */
Try<process::Owned<Fetcher>> create(const Option<Flags>& _flags = None());

} // namespace fetcher {
} // namespace uri {
} // namespace mesos {

#endif // __URI_FETCHER_HPP__
