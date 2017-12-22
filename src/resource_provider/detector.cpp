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

#include "resource_provider/detector.hpp"

#include <memory>
#include <utility>

#include <stout/lambda.hpp>

namespace http = process::http;

using process::Future;
using process::Promise;

namespace mesos {
namespace internal {

ConstantEndpointDetector::ConstantEndpointDetector(const http::URL& _url)
  : url(_url) {}


Future<Option<http::URL>> ConstantEndpointDetector::detect(
    const Option<http::URL>& previous)
{
  if (previous.isNone() || stringify(previous.get()) != stringify(url)) {
    return url;
  } else {
    // Use a promise here to properly handle discard semantics.
    std::unique_ptr<Promise<Option<http::URL>>> promise(
        new Promise<Option<http::URL>>());

    Future<Option<http::URL>> future = promise->future();

    // TODO(jieyu): There is a cyclic dependency here because `future`
    // holds a reference to `promise` and `promise` holds a reference
    // to the `future`. It won't get properly cleaned up if
    // `future.discard()` is not called and `future` is not terminal.
    // Currently, it's OK because the caller always do a
    // `future.discard()` before removing the reference to `future`.
    future.onDiscard(lambda::partial(
        [](std::unique_ptr<Promise<Option<http::URL>>> promise) {
          promise->discard();
        },
        std::move(promise)));

    return future;
  }
}

} // namespace internal {
} // namespace mesos {
