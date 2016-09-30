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

#include "log/metrics.hpp"

#include <process/defer.hpp>

#include <process/metrics/metrics.hpp>

#include "log/log.hpp"

using std::string;

using process::defer;

namespace mesos {
namespace internal {
namespace log {

Metrics::Metrics(
    const LogProcess& process,
    const Option<string>& prefix)
  : recovered(
        prefix.getOrElse("") + "log/recovered",
        defer(process, &LogProcess::_recovered)),
    ensemble_size(
        prefix.getOrElse("") + "log/ensemble_size",
        defer(process, &LogProcess::_ensemble_size))
{
  process::metrics::add(recovered);
  process::metrics::add(ensemble_size);
}


Metrics::~Metrics()
{
  process::metrics::remove(recovered);
  process::metrics::remove(ensemble_size);
}

} // namespace log {
} // namespace internal {
} // namespace mesos {
