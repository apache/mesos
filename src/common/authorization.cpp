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

#include "common/authorization.hpp"

#include <algorithm>

#include <process/collect.hpp>

#include <stout/foreach.hpp>

using std::vector;

using process::Future;

namespace mesos {
namespace authorization {

Future<bool> collectAuthorizations(const vector<Future<bool>>& authorizations)
{
  return process::collect(authorizations)
    .then([](const vector<bool>& results) -> Future<bool> {
      return std::find(results.begin(), results.end(), false) == results.end();
    });
}

} // namespace authorization {
} // namespace mesos {
