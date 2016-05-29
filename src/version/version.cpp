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

#include <mesos/mesos.hpp>

#include <process/help.hpp>
#include <process/http.hpp>

#include <stout/json.hpp>

#include "version/version.hpp"

using namespace process;

using process::DESCRIPTION;
using process::HELP;
using process::TLDR;

using process::http::OK;

using std::string;

namespace mesos {
namespace internal {

static const string VERSION_HELP()
{
  // TODO(haosdent): generate the example in description automatically after we
  // have json pretty printing.
  return HELP(
    TLDR(
        "Provides version information."),
    DESCRIPTION(
        "Example:",
        "",
        "```",
        "{",
        "  \"version\":\"0.26.0\",",
        "  \"build_user\":\"username\",",
        "  \"build_time\":1443894750,",
        "  \"build_date\":\"2015-10-04 01:52:30\"",
        "  \"git_branch\":\"branch\",  // Optional",
        "  \"git_tag\":\"0.26.0-rc1\",  // Optional",
        "  \"git_sha\":\"d31f096a4665650ad4b9eda372ac41d2c472a77c\","
        "  // Optional",
        "}",
        "```"));
}


VersionProcess::VersionProcess()
  : ProcessBase("version")
{}


void VersionProcess::initialize()
{
  route("/", VERSION_HELP(), &VersionProcess::version);
}


Future<http::Response> VersionProcess::version(const http::Request& request)
{
  return OK(internal::version(), request.url.query.get("jsonp"));
}

}  // namespace internal {
}  // namespace mesos {
