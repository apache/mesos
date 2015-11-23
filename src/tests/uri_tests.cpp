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

#include <gtest/gtest.h>

#include <stout/stringify.hpp>

#include <mesos/uri/uri.hpp>

#include "uri/schemes/file.hpp"
#include "uri/schemes/http.hpp"

namespace mesos {
namespace internal {
namespace tests {

TEST(UriTest, SchemeFile)
{
  EXPECT_EQ("file:///etc/hosts", stringify(uri::file("/etc/hosts")));
  EXPECT_EQ("file:///usr", stringify(uri::file("/usr")));
}


TEST(UriTest, SchemeHttp)
{
  EXPECT_EQ("http://www.google.com/",
            stringify(uri::http("www.google.com")));

  EXPECT_EQ("http://twitter.com/apachemesos",
            stringify(uri::http("twitter.com", "/apachemesos")));

  EXPECT_EQ("http://localhost:5051/metrics/snapshot",
            stringify(uri::http("localhost", "/metrics/snapshot", 5051)));

  EXPECT_EQ("http://host/path?query#fragment",
            stringify(uri::http("host", "/path", None(), "query", "fragment")));

  EXPECT_EQ("http://user:password@host",
            stringify(uri::http(
                "host", "", None(), None(), None(), "user", "password")));
}

} // namespace tests {
} // namespace internal {
} // namespace mesos {
