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

#ifndef __URI_UTILS_HPP__
#define __URI_UTILS_HPP__

#include <string>

#include <stout/none.hpp>
#include <stout/option.hpp>

#include <mesos/uri/uri.hpp>

namespace mesos {
namespace uri {

/**
 * Construct an URI with the given parameters. No validation will be
 * performed in this function.
 */
URI construct(
    const std::string& scheme,
    const std::string& path = "",
    const Option<std::string>& host = None(),
    const Option<int>& port = None(),
    const Option<std::string>& query = None(),
    const Option<std::string>& fragment = None(),
    const Option<std::string>& user = None(),
    const Option<std::string>& password = None());

} // namespace uri {
} // namespace mesos {

#endif // __URI_UTILS_HPP__
