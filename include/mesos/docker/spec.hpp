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

#ifndef __MESOS_DOCKER_SPEC_HPP__
#define __MESOS_DOCKER_SPEC_HPP__

#include <iostream>
#include <string>

#include <stout/error.hpp>
#include <stout/json.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

// ONLY USEFUL AFTER RUNNING PROTOC.
#include <mesos/docker/spec.pb.h>

#include <mesos/docker/v1.hpp>
#include <mesos/docker/v2.hpp>
#include <mesos/docker/v2_2.hpp>

namespace docker {
namespace spec {

// The prefix of whiteout files in a docker image.
constexpr char WHITEOUT_PREFIX[] = ".wh.";


// The prefix of opaque whiteout files in a docker image.
constexpr char WHITEOUT_OPAQUE_PREFIX[] = ".wh..wh..opq";


// Parse the docker image reference. Docker expects the image
// reference to be in the following format:
//   [REGISTRY_HOST[:REGISTRY_PORT]/]REPOSITORY[:TAG|@TYPE:DIGEST]
//
// This format is inherently ambiguous when dealing with repository
// names that include forward slashes. To disambiguate, the docker
// code looks for '.', or ':', or 'localhost' to decide if the first
// component is a registry or a repository name. For more detail,
// drill into the implementation of docker pull.
//
// See docker implementation:
// https://github.com/docker/distribution/blob/master/reference/reference.go
Try<ImageReference> parseImageReference(const std::string& s);


std::ostream& operator<<(std::ostream& stream, const ImageReference& reference);


// Returns the port of a docker registry.
Result<int> getRegistryPort(const std::string& registry);


// Returns the scheme of a docker registry.
Try<std::string> getRegistryScheme(const std::string& registry);


// Returns the host of a docker registry.
std::string getRegistryHost(const std::string& registry);


// Returns the hashmap<registry_URL, spec::DockerConfigAuth> by
// parsing the docker config file from a JSON object.
Try<hashmap<std::string, Config::Auth>> parseAuthConfig(
    const JSON::Object& _config);


// Returns the hashmap<registry_URL, spec::DockerConfigAuth> by
// parsing the docker config file from a string.
Try<hashmap<std::string, Config::Auth>> parseAuthConfig(const std::string& s);


// Find the host from a docker config auth url.
std::string parseAuthUrl(const std::string& _url);


namespace v1 {

// Validates if the specified docker v1 image manifest conforms to the
// Docker v1 spec. Returns the error if the validation fails.
Option<Error> validate(const ImageManifest& manifest);


// Returns the docker v1 image manifest from the given JSON object.
Try<ImageManifest> parse(const JSON::Object& json);


// Returns the docker v1 image manifest from the given string.
Try<ImageManifest> parse(const std::string& s);

} // namespace v1 {


namespace v2 {

// Validates if the specified v2 image manifest conforms to the Docker
// v2 spec. Returns the error if the validation fails.
Option<Error> validate(const ImageManifest& manifest);


// Returns the docker v2 image manifest from the given JSON object.
Try<ImageManifest> parse(const JSON::Object& json);


// Returns the docker v2 image manifest from the given string.
Try<ImageManifest> parse(const std::string& s);

} // namespace v2 {


namespace v2_2 {

// Validates if the specified docker v2 s2 image manifest conforms to the
// Docker v2 s2 spec. Returns the error if the validation fails.
Option<Error> validate(const ImageManifest& manifest);


// Returns the docker v2 s2 image manifest from the given JSON object.
Try<ImageManifest> parse(const JSON::Object& json);


// Returns the docker v2 s2 image manifest from the given string.
Try<ImageManifest> parse(const std::string& s);

} // namespace v2_2 {
} // namespace spec {
} // namespace docker {

#endif // __MESOS_DOCKER_SPEC_HPP__
