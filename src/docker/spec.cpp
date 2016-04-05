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

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/none.hpp>
#include <stout/protobuf.hpp>
#include <stout/strings.hpp>

#include <mesos/docker/spec.hpp>

using std::ostream;
using std::string;
using std::vector;

namespace docker {
namespace spec {

// TODO(jieyu): Use regex to parse and verify the reference.
Try<ImageReference> parseImageReference(const string& _s)
{
  ImageReference reference;
  string s(_s);

  // Extract the digest.
  if (strings::contains(s, "@")) {
    vector<string> split = strings::split(s, "@");
    if (split.size() != 2) {
      return Error("Multiple '@' symbols found");
    }

    s = split[0];
    reference.set_digest(split[1]);
  }

  // Remove the tag. We need to watch out for a
  // host:port registry, which also contains ':'.
  if (strings::contains(s, ":")) {
    vector<string> split = strings::split(s, ":");

    // The tag must be the last component. If a slash is
    // present there is a registry port and no tag.
    if (!strings::contains(split.back(), "/")) {
      reference.set_tag(split.back());
      split.pop_back();

      s = strings::join(":", split);
    }
  }

  // Extract the registry and repository. The first component can
  // either be the registry, or the first part of the repository!
  // We resolve this ambiguity using the same hacks used in the
  // docker code ('.', ':', 'localhost' indicate a registry).
  vector<string> split = strings::split(s, "/", 2);

  if (split.size() == 1) {
    reference.set_repository(s);
  } else if (strings::contains(split[0], ".") ||
             strings::contains(split[0], ":") ||
             split[0] == "localhost") {
    reference.set_registry(split[0]);
    reference.set_repository(split[1]);
  } else {
    reference.set_repository(s);
  }

  return reference;
}


ostream& operator<<(ostream& stream, const ImageReference& reference)
{
  if (reference.has_registry()) {
    stream << reference.registry() << "/" << reference.repository();
  } else {
    stream << reference.repository();
  }

  if (reference.has_tag()) {
    stream << ":" << reference.tag();
  }

  return stream;
}


Result<int> getRegistryPort(const string& registry)
{
  if (registry.empty()) {
    return None();
  }

  Option<int> port;

  vector<string> split = strings::split(registry, ":", 2);
  if (split.size() != 1) {
    Try<int> numified = numify<int>(split[1]);
    if (numified.isError()) {
      return Error("Failed to numify '" + split[1] + "'");
    }

    port = numified.get();
  }

  return port;
}


Try<string> getRegistryScheme(const string& registry)
{
  Result<int> port = getRegistryPort(registry);
  if (port.isError()) {
    return Error("Failed to get registry port: " + port.error());
  } else if (port.isSome() && port.get() == 80) {
    return "http";
  }

  return "https";
}


string getRegistryHost(const string& registry)
{
  if (registry.empty()) {
    return "";
  }

  vector<string> split = strings::split(registry, ":", 2);

  return split[0];
}


namespace v1 {

Option<Error> validate(const ImageManifest& manifest)
{
  // TODO(gilbert): Add validations.
  return None();
}


Try<ImageManifest> parse(const JSON::Object& json)
{
  Try<ImageManifest> manifest = protobuf::parse<ImageManifest>(json);
  if (manifest.isError()) {
    return Error("Protobuf parse failed: " + manifest.error());
  }

  Option<Error> error = validate(manifest.get());
  if (error.isSome()) {
    return Error("Docker v1 image manifest validation failed: " +
                 error.get().message);
  }

  return manifest.get();
}


Try<ImageManifest> parse(const string& s)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(s);
  if (json.isError()) {
    return Error("JSON parse failed: " + json.error());
  }

  return parse(json.get());
}

} // namespace v1 {


namespace v2 {

Option<Error> validate(const ImageManifest& manifest)
{
  // Validate required fields are present,
  // e.g., repeated fields that has to be >= 1.
  if (manifest.fslayers_size() <= 0) {
    return Error("'fsLayers' field size must be at least one");
  }

  if (manifest.history_size() <= 0) {
    return Error("'history' field size must be at least one");
  }

  if (manifest.signatures_size() <= 0) {
    return Error("'signatures' field size must be at least one");
  }

  // Verify that blobSum and v1Compatibility numbers are equal.
  if (manifest.fslayers_size() != manifest.history_size()) {
    return Error("The size of 'fsLayers' should be equal "
                 "to the size of 'history'");
  }

  // Verify 'fsLayers' field.
  foreach (const ImageManifest::FsLayer& fslayer, manifest.fslayers()) {
    const string& blobSum = fslayer.blobsum();
    if (!strings::contains(blobSum, ":")) {
      return Error("Incorrect 'blobSum' format: " + blobSum);
    }
  }

  return None();
}


Try<ImageManifest> parse(const JSON::Object& json)
{
  Try<ImageManifest> manifest = protobuf::parse<ImageManifest>(json);
  if (manifest.isError()) {
    return Error("Protobuf parse failed: " + manifest.error());
  }

  for (int i = 0; i < manifest.get().history_size(); i++) {
    Try<JSON::Object> v1Compatibility = JSON::parse<JSON::Object>(
        manifest.get().history(i).v1compatibility());

    if (v1Compatibility.isError()) {
      return Error("Parsing v1Compatibility JSON failed: " +
                   v1Compatibility.error());
    }

    Try<v1::ImageManifest> v1 = v1::parse(v1Compatibility.get());
    if (v1.isError()) {
      return Error("Parsing v1Compatibility protobuf failed: " + v1.error());
    }

    CHECK(!manifest.get().history(i).has_v1());

    manifest->mutable_history(i)->mutable_v1()->CopyFrom(v1.get());
  }

  Option<Error> error = validate(manifest.get());
  if (error.isSome()) {
    return Error("Docker v2 image manifest validation failed: " +
                 error.get().message);
  }

  return manifest.get();
}


Try<ImageManifest> parse(const string& s)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(s);
  if (json.isError()) {
    return Error("JSON parse failed: " + json.error());
  }

  return parse(json.get());
}

} // namespace v2 {
} // namespace spec {
} // namespace docker {
