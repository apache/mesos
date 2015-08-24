/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __TESTS_UTILS_HPP__
#define __TESTS_UTILS_HPP__

#include <gtest/gtest.h>

#include <string>

#include <stout/json.hpp>
#include <stout/option.hpp>

namespace mesos {
namespace internal {
namespace tests {

// Test fixture for creating a temporary directory for each test.
// TODO(vinod): Fold this into stout/tests/utils.hpp.
class TemporaryDirectoryTest : public ::testing::Test
{
protected:
  virtual void SetUp();
  virtual void TearDown();

  Option<std::string> sandbox;

private:
  std::string cwd;
};


// Get the metrics snapshot.
// TODO(vinod): Move this into a libprocess utility header.
JSON::Object Metrics();

} // namespace tests {
} // namespace internal {
} // namespace mesos {

#endif // __TESTS_UTILS_HPP__
