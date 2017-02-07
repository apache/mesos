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

#include <vector>

#include <stout/protobuf.hpp>

#include <mesos/oci/spec.hpp>

using std::string;
using std::vector;

namespace oci {
namespace spec {
namespace image {
namespace v1 {

namespace internal {

Option<Error> validateDigest(const string& digest)
{
  vector<string> split = strings::split(digest, ":");
  if (split.size() != 2) {
    return Error("Incorrect 'digest' format: " + digest);
  }

  // TODO(qianzhang): Validate algorithm (split[0]) and hex (split[1]).

  return None();
}


Option<Error> validate(const ManifestList& manifestList)
{
  if (manifestList.schemaversion() != 2) {
    return Error(
        "Incorrect 'schemaVersion': " +
        stringify(manifestList.schemaversion()));
  }

  foreach (const ManifestDescriptor& manifest, manifestList.manifests()) {
    Option<Error> error = validateDigest(manifest.digest());
    if (error.isSome()) {
      return Error(
          "Failed to validate 'digest' of the 'manifest': " + error->message);
    }

    if (manifest.mediatype() != MEDIA_TYPE_MANIFEST) {
      return Error(
          "Incorrect 'mediaType' of the 'manifest': " + manifest.mediatype());
    }
  }

  return None();
}


Option<Error> validate(const Manifest& manifest)
{
  if (manifest.schemaversion() != 2) {
    return Error(
        "Incorrect 'schemaVersion': " +
        stringify(manifest.schemaversion()));
  }

  const Descriptor& config = manifest.config();

  Option<Error> error = validateDigest(config.digest());
  if (error.isSome()) {
    return Error(
        "Failed to validate 'digest' of the 'config': " + error->message);
  }

  if (config.mediatype() != MEDIA_TYPE_CONFIG) {
    return Error(
        "Incorrect 'mediaType' of the 'config': " + config.mediatype());
  }

  if (manifest.layers_size() <= 0) {
    return Error("'layers' field size must be at least one");
  }

  foreach (const Descriptor& layer, manifest.layers()) {
    Option<Error> error = validateDigest(layer.digest());
    if (error.isSome()) {
      return Error(
          "Failed to validate 'digest' of the 'layer': " + error->message);
    }

    if (layer.mediatype() != MEDIA_TYPE_LAYER &&
        layer.mediatype() != MEDIA_TYPE_NONDIST_LAYER) {
      return Error(
          "Incorrect 'mediaType' of the 'layer': " + layer.mediatype());
    }
  }

  return None();
}

} // namespace internal {


template <>
Try<Descriptor> parse(const string& s)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(s);
  if (json.isError()) {
    return Error("JSON parse failed: " + json.error());
  }

  Try<Descriptor> descriptor = protobuf::parse<Descriptor>(json.get());
  if (descriptor.isError()) {
    return Error("Protobuf parse failed: " + descriptor.error());
  }

  Option<Error> error = internal::validateDigest(descriptor->digest());
  if (error.isSome()) {
    return Error(
        "OCI v1 image descriptor validation failed: " + error->message);
  }

  return descriptor.get();
}


template <>
Try<ManifestList> parse(const string& s)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(s);
  if (json.isError()) {
    return Error("JSON parse failed: " + json.error());
  }

  Try<ManifestList> manifestList = protobuf::parse<ManifestList>(json.get());
  if (manifestList.isError()) {
    return Error("Protobuf parse failed: " + manifestList.error());
  }

  // Manually parse 'manifest.platform.os.version' and
  // 'manifest.platform.os.features'.
  Result<JSON::Array> manifests = json->at<JSON::Array>("manifests");
  if (manifests.isError()) {
    return Error("Failed to find 'manifests': " + manifests.error());
  } else if (manifests.isNone()) {
    return Error("Unable to find 'manifests'");
  }

  foreach (const JSON::Value& value, manifests->values) {
    if (!value.is<JSON::Object>()) {
      return Error("Expecting 'manifest' to be JSON object type");
    }

    const JSON::Object manifest = value.as<JSON::Object>();
    Result<JSON::String> digest = manifest.at<JSON::String>("digest");
    if (digest.isError()) {
      return Error("Failed to find 'digest': " + digest.error());
    } else if (digest.isNone()) {
      return Error("Unable to find 'digest'");
    }

    ManifestDescriptor* _manifest = nullptr;
    for (int i = 0; i < manifestList->manifests_size(); i++) {
      if (manifestList->manifests(i).digest() == digest.get()) {
        _manifest = manifestList->mutable_manifests(i);
        break;
      }
    }

    if (_manifest == nullptr) {
      return Error(
          "Unable to find the manifest whose digest is '" +
          digest->value + "'");
    }

    Result<JSON::Object> platform = manifest.at<JSON::Object>("platform");
    if (platform.isError()) {
      return Error("Failed to find 'platform': " + platform.error());
    } else if (platform.isNone()) {
      return Error("Unable to find 'platform'");
    }

    Result<JSON::String> osVersion = platform->at<JSON::String>("os.version");
    if (osVersion.isError()) {
      return Error(
          "Failed to find 'platform.os.version': " + osVersion.error());
    }

    if (osVersion.isSome()) {
      Platform* platform = _manifest->mutable_platform();
      platform->set_os_version(osVersion->value);
    }

    Result<JSON::Array> osFeatures = platform->at<JSON::Array>("os.features");
    if (osFeatures.isError()) {
      return Error(
          "Failed to find 'platform.os.features': " + osFeatures.error());
    }

    if (osFeatures.isSome()) {
      const vector<JSON::Value>& values = osFeatures->values;
      if (values.size() != 0) {
        Platform* platform = _manifest->mutable_platform();
        foreach (const JSON::Value& value, values) {
          if (!value.is<JSON::String>()) {
            return Error("Expecting OS feature to be string type");
          }

          platform->add_os_features(value.as<JSON::String>().value);
        }
      }
    }
  }

  // Manually parse 'annotations' field.
  Result<JSON::Value> annotations = json->find<JSON::Value>("annotations");
  if (annotations.isError()) {
    return Error(
        "Failed to find 'annotations': " + annotations.error());
  }

  if (annotations.isSome() && !annotations->is<JSON::Null>()) {
    foreachpair (const string& key,
                 const JSON::Value& value,
                 annotations->as<JSON::Object>().values) {
      if (!value.is<JSON::String>()) {
        return Error(
            "The value of annotation key '" + key + "' is not a JSON string");
      }

      Label* annotation = manifestList->add_annotations();
      annotation->set_key(key);
      annotation->set_value(value.as<JSON::String>().value);
    }
  }

  Option<Error> error = internal::validate(manifestList.get());
  if (error.isSome()) {
    return Error(
        "OCI v1 image manifest list validation failed: " + error->message);
  }

  return manifestList.get();
}


template <>
Try<Manifest> parse(const string& s)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(s);
  if (json.isError()) {
    return Error("JSON parse failed: " + json.error());
  }

  Try<Manifest> manifest = protobuf::parse<Manifest>(json.get());
  if (manifest.isError()) {
    return Error("Protobuf parse failed: " + manifest.error());
  }

  // Manually parse 'annotations' field.
  Result<JSON::Value> annotations = json->find<JSON::Value>("annotations");
  if (annotations.isError()) {
    return Error(
        "Failed to find 'annotations': " + annotations.error());
  }

  if (annotations.isSome() && !annotations->is<JSON::Null>()) {
    foreachpair (const string& key,
                 const JSON::Value& value,
                 annotations->as<JSON::Object>().values) {
      if (!value.is<JSON::String>()) {
        return Error(
            "The value of annotation key '" + key + "' is not a JSON string");
      }

      Label* annotation = manifest->add_annotations();
      annotation->set_key(key);
      annotation->set_value(value.as<JSON::String>().value);
    }
  }

  Option<Error> error = internal::validate(manifest.get());
  if (error.isSome()) {
    return Error(
        "OCI v1 image manifest validation failed: " + error->message);
  }

  return manifest.get();
}


template <>
Try<Configuration> parse(const string& s)
{
  Try<JSON::Object> json = JSON::parse<JSON::Object>(s);
  if (json.isError()) {
    return Error("JSON parse failed: " + json.error());
  }

  Try<Configuration> configuration =
      protobuf::parse<Configuration>(json.get());

  if (configuration.isError()) {
    return Error("Protobuf parse failed: " + configuration.error());
  }

  Result<JSON::Object> config = json->find<JSON::Object>("config");
  if (config.isError()) {
    return Error("Failed to find 'config': " + config.error());
  }

  if (config.isSome()) {
    // Manually parse 'ExposedPorts' field.
    Result<JSON::Value> value = config->find<JSON::Value>("ExposedPorts");
    if (value.isError()) {
      return Error("Failed to find 'ExposedPorts': " + value.error());
    }

    if (value.isSome() && !value->is<JSON::Null>()) {
      foreachkey (const string& key, value->as<JSON::Object>().values) {
        configuration->mutable_config()->add_exposedports(key);
      }
    }

    // Manually parse 'Volumes' field.
    value = config->find<JSON::Value>("Volumes");
    if (value.isError()) {
      return Error("Failed to find 'Volumes': " + value.error());
    }

    if (value.isSome() && !value->is<JSON::Null>()) {
      foreachkey (const string& key, value->as<JSON::Object>().values) {
        configuration->mutable_config()->add_volumes(key);
      }
    }

    // Manually parse 'Labels' field.
    value = config->find<JSON::Value>("Labels");
    if (value.isError()) {
      return Error("Failed to find 'Labels': " + value.error());
    }

    if (value.isSome() && !value->is<JSON::Null>()) {
      foreachpair (const string& key,
                   const JSON::Value& value,
                   value->as<JSON::Object>().values) {
        if (!value.is<JSON::String>()) {
          return Error(
              "The value of label key '" + key + "' is not a JSON string");
        }

        Label* label = configuration->mutable_config()->add_labels();
        label->set_key(key);
        label->set_value(value.as<JSON::String>().value);
      }
    }
  }

  return configuration.get();
}

} // namespace v1 {
} // namespace image {
} // namespace spec {
} // namespace oci {
