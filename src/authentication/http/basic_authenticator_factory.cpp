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

#include <mesos/authentication/http/basic_authenticator_factory.hpp>

#include <mesos/module/http_authenticator.hpp>

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/protobuf.hpp>

#include "credentials/credentials.hpp"

#include "master/constants.hpp"

#include "module/manager.hpp"

namespace mesos {
namespace http {
namespace authentication {

using std::string;

using google::protobuf::RepeatedPtrField;

using process::http::authentication::Authenticator;
using process::http::authentication::BasicAuthenticator;


Try<Authenticator*> BasicAuthenticatorFactory::create(
    const string& realm,
    const Credentials& credentials)
{
  hashmap<string, string> credentialMap;

  foreach (const Credential& credential, credentials.credentials()) {
    credentialMap.put(credential.principal(), credential.secret());
  }

  return create(realm, credentialMap);
}


Try<Authenticator*> BasicAuthenticatorFactory::create(
    const Parameters& parameters)
{
  Credentials credentials;
  Option<string> realm;

  foreach (const Parameter& parameter, parameters.parameter()) {
    if (parameter.key() == "credentials") {
      Try<JSON::Value> json = JSON::parse(parameter.value());
      if (json.isError()) {
        return Error(
            "Unable to parse HTTP credentials as JSON: " +
            json.error());
      }

      Try<RepeatedPtrField<Credential>> credentials_ =
        protobuf::parse<RepeatedPtrField<Credential>>(json.get());
      if (credentials_.isError()) {
        return Error(
            "Unable to parse credentials for basic HTTP authenticator: " +
            credentials_.error());
      }

      credentials.mutable_credentials()->CopyFrom(credentials_.get());
    } else if (parameter.key() == "authentication_realm") {
        realm = parameter.value();
    } else {
      return Error(
          "Unknown basic authenticator parameter: " + parameter.key());
    }
  }

  if (realm.isNone()) {
    return Error("Must specify a realm for the basic HTTP authenticator");
  }

  return create(realm.get(), credentials);
}


Try<Authenticator*> BasicAuthenticatorFactory::create(
    const string& realm,
    const hashmap<string, string>& credentials)
{
  Authenticator* authenticator = new BasicAuthenticator(realm, credentials);

  return authenticator;
}

} // namespace authentication {
} // namespace http {
} // namespace mesos {
