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

#ifndef __SASL_AUTHENTICATOR_HPP__
#define __SASL_AUTHENTICATOR_HPP__

#include <sasl/sasl.h>
#include <sasl/saslplug.h>

#include <string>
#include <vector>

#include <mesos/mesos.hpp>

#include <process/future.hpp>
#include <process/id.hpp>
#include <process/once.hpp>
#include <process/process.hpp>
#include <process/protobuf.hpp>

#include "messages/messages.hpp"

#include "sasl/auxprop.hpp"

namespace mesos {
namespace internal {
namespace sasl {

// Forward declaration.
class AuthenticatorProcess;


class Authenticator
{
public:
  Authenticator(const process::UPID& pid);
  ~Authenticator();

  process::Future<bool> authenticate();

private:
  AuthenticatorProcess* process;
};


class AuthenticatorProcess : public ProtobufProcess<AuthenticatorProcess>
{
public:
  AuthenticatorProcess(const process::UPID& _pid)
    : ProcessBase(process::ID::generate("authenticator")),
      status(READY),
      pid(_pid),
      connection(NULL) {}

  virtual ~AuthenticatorProcess()
  {
    if (connection != NULL) {
      sasl_dispose(&connection);
    }
  }

  process::Future<bool> authenticate()
  {
    static process::Once* initialize = new process::Once();
    static bool initialized = false;

    if (!initialize->once()) {
      LOG(INFO) << "Initializing server SASL";

      int result = sasl_server_init(NULL, "mesos");

      if (result != SASL_OK) {
        std::string error = "Failed to initialize SASL: ";
        error += sasl_errstring(result, NULL, NULL);
        LOG(ERROR) << error;
        AuthenticationErrorMessage message;
        message.set_error(error);
        send(pid, message);
        status = ERROR;
        promise.fail(error);
        initialize->done();
        return promise.future();
      }

      result = sasl_auxprop_add_plugin(
          InMemoryAuxiliaryPropertyPlugin::name(),
          &InMemoryAuxiliaryPropertyPlugin::initialize);

      if (result != SASL_OK) {
        std::string error =
          "Failed to add \"in-memory\" auxiliary property plugin: ";
        error += sasl_errstring(result, NULL, NULL);
        LOG(ERROR) << error;
        AuthenticationErrorMessage message;
        message.set_error(error);
        send(pid, message);
        status = ERROR;
        promise.fail(error);
        initialize->done();
        return promise.future();
      }

      initialized = true;

      initialize->done();
    }

    if (!initialized) {
      promise.fail("Failed to initialize SASL");
      return promise.future();
    }

    if (status != READY) {
      return promise.future();
    }

    callbacks[0].id = SASL_CB_GETOPT;
    callbacks[0].proc = (int(*)()) &getopt;
    callbacks[0].context = NULL;

    callbacks[1].id = SASL_CB_LIST_END;
    callbacks[1].proc = NULL;
    callbacks[1].context = NULL;

    LOG(INFO) << "Creating new server SASL connection";

    int result = sasl_server_new(
        "mesos",    // Registered name of service.
        NULL,       // Server's FQDN; NULL uses gethostname().
        NULL,       // The user realm used for password lookups;
                    // NULL means default to FQDN.
                    // NOTE: This does not affect Kerberos.
        NULL, NULL, // IP address information strings.
        callbacks,  // Callbacks supported only for this connection.
        0,          // Security flags (security layers are enabled
                    // using security properties, separately).
        &connection);

    if (result != SASL_OK) {
      std::string error = "Failed to create server SASL connection: ";
      error += sasl_errstring(result, NULL, NULL);
      LOG(ERROR) << error;
      AuthenticationErrorMessage message;
      message.set_error(error);
      send(pid, message);
      status = ERROR;
      promise.fail(error);
      return promise.future();
    }

    // Get the list of mechanisms.
    const char* output = NULL;
    unsigned length = 0;
    int count = 0;

    result = sasl_listmech(
        connection,  // The context for this connection.
        NULL,        // Not supported.
        "",          // What to prepend to the output string.
        ",",         // What to separate mechanisms with.
        "",          // What to append to the output string.
        &output,     // The output string.
        &length,     // The length of the output string.
        &count);     // The count of the mechanisms in output.

    if (result != SASL_OK) {
      std::string error = "Failed to get list of mechanisms: ";
      LOG(WARNING) << error << sasl_errstring(result, NULL, NULL);
      AuthenticationErrorMessage message;
      error += sasl_errdetail(connection);
      message.set_error(error);
      send(pid, message);
      status = ERROR;
      promise.fail(error);
      return promise.future();
    }

    std::vector<std::string> mechanisms = strings::tokenize(output, ",");

    // Send authentication mechanisms.
    AuthenticationMechanismsMessage message;
    foreach (const std::string& mechanism, mechanisms) {
      message.add_mechanisms(mechanism);
    }

    send(pid, message);

    status = STARTING;

    return promise.future();
  }

protected:
  virtual void initialize()
  {
    link(pid); // Don't bother waiting for a lost authenticatee.

    // Anticipate start and steps messages from the client.
    install<AuthenticationStartMessage>(
        &AuthenticatorProcess::start,
        &AuthenticationStartMessage::mechanism,
        &AuthenticationStartMessage::data);

    install<AuthenticationStepMessage>(
        &AuthenticatorProcess::step,
        &AuthenticationStepMessage::data);
  }

  virtual void exited(const process::UPID& _pid)
  {
    if (pid == _pid) {
      status = ERROR;
      promise.fail("Failed to communicate with authenticatee");
    }
  }

  void start(const std::string& mechanism, const std::string& data)
  {
    if (status != STARTING) {
      AuthenticationErrorMessage message;
      message.set_error("Unexpected authentication 'start' received");
      send(pid, message);
      status = ERROR;
      promise.fail(message.error());
      return;
    }

    LOG(INFO) << "Received SASL authentication start";

    // Start the server.
    const char* output = NULL;
    unsigned length = 0;

    int result = sasl_server_start(
        connection,
        mechanism.c_str(),
        data.length() == 0 ? NULL : data.data(),
        data.length(),
        &output,
        &length);

    handle(result, output, length);
  }

  void step(const std::string& data)
  {
    if (status != STEPPING) {
      AuthenticationErrorMessage message;
      message.set_error("Unexpected authentication 'step' received");
      send(pid, message);
      status = ERROR;
      promise.fail(message.error());
      return;
    }

    LOG(INFO) << "Received SASL authentication step";

    const char* output = NULL;
    unsigned length = 0;

    int result = sasl_server_step(
        connection,
        data.length() == 0 ? NULL : data.data(),
        data.length(),
        &output,
        &length);

    handle(result, output, length);
  }

private:
  static int getopt(
      void* context,
      const char* plugin,
      const char* option,
      const char** result,
      unsigned* length)
  {
    bool found = false;
    if (std::string(option) == "auxprop_plugin") {
      *result = "in-memory-auxprop";
      found = true;
    } else if (std::string(option) == "mech_list") {
      *result = "CRAM-MD5";
      found = true;
    } else if (std::string(option) == "pwcheck_method") {
      *result = "auxprop";
      found = true;
    }

    if (found && length != NULL) {
      *length = strlen(*result);
    }

    return SASL_OK;
  }

  // Helper for handling result of server start and step.
  void handle(int result, const char* output, unsigned length)
  {
    if (result == SASL_OK) {
      LOG(INFO) << "Authentication success";
      // Note that we're not using SASL_SUCCESS_DATA which means that
      // we should not have any data to send when we get a SASL_OK.
      CHECK(output == NULL);
      send(pid, AuthenticationCompletedMessage());
      status = COMPLETED;
      promise.set(true);
    } else if (result == SASL_CONTINUE) {
      LOG(INFO) << "Authentication requires more steps";
      AuthenticationStepMessage message;
      message.set_data(CHECK_NOTNULL(output), length);
      send(pid, message);
      status = STEPPING;
    } else if (result == SASL_NOUSER || result == SASL_BADAUTH) {
      LOG(WARNING) << "Authentication failure: "
                   << sasl_errstring(result, NULL, NULL);
      send(pid, AuthenticationFailedMessage());
      status = FAILED;
      promise.set(false);
    } else {
      LOG(ERROR) << "Authentication error: "
                 << sasl_errstring(result, NULL, NULL);
      AuthenticationErrorMessage message;
      std::string error(sasl_errdetail(connection));
      message.set_error(error);
      send(pid, message);
      status = ERROR;
      promise.fail(message.error());
    }
  }

  enum {
    READY,
    STARTING,
    STEPPING,
    COMPLETED,
    FAILED,
    ERROR
  } status;

  sasl_callback_t callbacks[2];

  const process::UPID pid;

  sasl_conn_t* connection;

  process::Promise<bool> promise;
};


Authenticator::Authenticator(const process::UPID& pid)
{
  process = new AuthenticatorProcess(pid);
  process::spawn(process);
}


Authenticator::~Authenticator()
{
  process::terminate(process);
  process::wait(process);
  delete process;
}


process::Future<bool> Authenticator::authenticate()
{
  return process::dispatch(process, &AuthenticatorProcess::authenticate);
}


namespace secrets {

// Loads secrets (principal -> secret) into the in-memory auxiliary
// property plugin that is used by the authenticators.
void load(const std::map<std::string, std::string>& secrets)
{
  Multimap<std::string, Property> properties;

  foreachpair (const std::string& principal,
               const std::string& secret, secrets) {
    Property property;
    property.name = SASL_AUX_PASSWORD_PROP;
    property.values.push_back(secret);
    properties.put(principal, property);
  }

  InMemoryAuxiliaryPropertyPlugin::load(properties);
}


// Load credentials into the in-memory auxiliary propery plugin
// that is used by the authenticators.
void load(const std::vector<Credential>& credentials)
{
  std::map<std::string, std::string> secrets;

  foreach (const Credential& credential, credentials) {
    secrets[credential.principal()] = credential.secret();
  }

  load(secrets);
}

} // namespace secrets {

} // namespace sasl {
} // namespace internal {
} // namespace mesos {

#endif //__SASL_AUTHENTICATOR_HPP__
