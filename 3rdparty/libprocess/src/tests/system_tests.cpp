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

#include <gmock/gmock.h>

#include <process/future.hpp>
#include <process/gtest.hpp>
#include <process/http.hpp>
#include <process/system.hpp>

#include <process/metrics/metrics.hpp>

#include <stout/gtest.hpp>

namespace http = process::http;

using process::Future;

// MESOS-1433
// This test is disabled as the pull-gauges that are used for these metrics
// may return Failures. In this case we do not put the metric into the
// endpoint. This has been observed specifically for the memory
// metrics. If in the future we put the error message from the Failure
// in the endpoint, or the memory metric is always available, we
// should reenable this test.
TEST(SystemTest, DISABLED_Metrics)
{
  Future<http::Response> response =
    http::get(process::metrics::internal::metrics, "snapshot");

  AWAIT_READY(response);

  EXPECT_SOME_EQ("application/json", response->headers.get("Content-Type"));

  Try<JSON::Object> parse = JSON::parse<JSON::Object>(response->body);
  ASSERT_SOME(parse);

  JSON::Object stats = parse.get();

  EXPECT_EQ(1u, stats.values.count("system/load_1min"));
  EXPECT_EQ(1u, stats.values.count("system/load_5min"));
  EXPECT_EQ(1u, stats.values.count("system/load_15min"));
  EXPECT_EQ(1u, stats.values.count("system/cpus_total"));
  EXPECT_EQ(1u, stats.values.count("system/mem_total_bytes"));
  EXPECT_EQ(1u, stats.values.count("system/mem_free_bytes"));
}
