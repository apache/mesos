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

#ifndef __DOCKER_SPEC_HPP__
#define __DOCKER_SPEC_HPP__

#include <stout/error.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>

#include "docker/v1.hpp"
#include "docker/v2.hpp"

namespace docker {
namespace spec {
namespace v1 {

/**
 * Validates if the specified docker v1 image manifest conforms to the
 * Docker v1 spec. Returns the error if the validation fails.
 */
Option<Error> validate(const ImageManifest& manifest);


/**
 * Returns the docker v1 image manifest from the given JSON object.
 */
Try<ImageManifest> parse(const JSON::Object& json);

} // namespace v1 {


namespace v2 {

/**
 * Validates if the specified v2 image manifest conforms to the Docker
 * v2 spec. Returns the error if the validation fails.
 */
Option<Error> validate(const ImageManifest& manifest);


/**
 * Returns the docker v1 image manifest from the given JSON object.
 */
Try<ImageManifest> parse(const JSON::Object& json);

} // namespace v2 {
} // namespace spec {
} // namespace docker {

#endif // __DOCKER_SPEC_HPP__
