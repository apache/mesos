---
title: Apache Mesos - Authentication
layout: documentation
---

# Authentication

Authentication permits only trusted entities to interact with a Mesos cluster. Authentication can be used by Mesos in three ways:

1. To require that frameworks be authenticated in order to register with the master.
2. To require that agents be authenticated in order to register with the master.
3. To require that operators be authenticated to use many [HTTP endpoints](endpoints/index.md).

Authentication is disabled by default. When authentication is enabled, operators
can configure Mesos to either use the default authentication module or to use a
_custom_ authentication module.

The default Mesos authentication module uses the
[Cyrus SASL](http://asg.web.cmu.edu/sasl/) library.  SASL is a flexible
framework that allows two endpoints to authenticate with each other using a
variety of methods. By default, Mesos uses
[CRAM-MD5](https://en.wikipedia.org/wiki/CRAM-MD5) authentication.

## Credentials, Principals, and Secrets

When using the default CRAM-MD5 authentication method, an entity that wants to
authenticate with Mesos must provide a *credential*, which consists of a
*principal* and a *secret*. The principal is the identity that the entity would
like to use; the secret is an arbitrary string that is used to verify that
identity. Principals are similar to user names, while secrets are similar to
passwords.

Principals are used primarily for authentication and
[authorization](authorization.md); note that a principal is different from a
framework's *user*, which is the operating system account used by the agent to
run executors, and the framework's *[roles](roles.md)*, which are used to
determine which resources a framework can use.

## Configuration

Authentication is configured by specifying command-line flags when starting the
Mesos master and agent processes. For more information, refer to the
[configuration](configuration.md) documentation.

### Master

* `--[no-]authenticate` - If `true`, only authenticated frameworks are allowed
  to register. If `false` (the default), unauthenticated frameworks are also
  allowed to register.

* `--[no-]authenticate_http_readonly` - If `true`, authentication is required to
  make HTTP requests to the read-only HTTP endpoints that support
  authentication. If `false` (the default), these endpoints can be used without
  authentication. Read-only endpoints are those which cannot be used to modify
  the state of the cluster.

* `--[no-]authenticate_http_readwrite` - If `true`, authentication is required
  to make HTTP requests to the read-write HTTP endpoints that support
  authentication. If `false` (the default), these endpoints can be used without
  authentication. Read-write endpoints are those which can be used to modify the
  state of the cluster.

* `--[no-]authenticate_agents` - If `true`, only authenticated agents are
  allowed to register. If `false` (the default), unauthenticated agents are also
  allowed to register.

* `--authentication_v0_timeout` - The timeout within which an authentication is
  expected to complete against a v0 framework or agent. This does not apply to
  the v0 or v1 HTTP APIs.(default: `15secs`)

* `--authenticators` - Specifies which authenticator module to use.  The default
  is `crammd5`, but additional modules can be added using the `--modules`
  option.

* `--http_authenticators` - Specifies which HTTP authenticator module to use.
  The default is `basic` (basic HTTP authentication), but additional modules can
  be added using the `--modules` option.

* `--credentials` - The path to a text file which contains a list of accepted
  credentials.  This may be optional depending on the authenticator being used.

### Agent

* `--authenticatee` - Analog to the master's `--authenticators` option to
  specify what module to use.  Defaults to `crammd5`.

* `--credential` - Just like the master's `--credentials` option except that
  only one credential is allowed. This credential is used to identify the agent
  to the master.

* `--[no-]authenticate_http_readonly` - If `true`, authentication is required to
  make HTTP requests to the read-only HTTP endpoints that support
  authentication. If `false` (the default), these endpoints can be used without
  authentication. Read-only endpoints are those which cannot be used to modify
  the state of the agent.

* `--[no-]authenticate_http_readwrite` - If `true`, authentication is required
  to make HTTP requests to the read-write HTTP endpoints that support
  authentication. If `false` (the default), these endpoints can be used without
  authentication. Read-write endpoints are those which can be used to modify the
  state of the agent. Note that for backward compatibility reasons, the V1
  executor API is not affected by this flag.

* `--[no-]authenticate_http_executors` - If `true`, authentication is required
  to make HTTP requests to the V1 executor API. If `false` (the default), that
  API can be used without authentication. If this flag is `true` and custom
  HTTP authenticators are not specified, then the default `JWT` authenticator is
  loaded to handle executor authentication.

* `--http_authenticators` - Specifies which HTTP authenticator module to use.
  The default is `basic`, but additional modules can be added using the
  `--modules` option.

* `--http_credentials` - The path to a text file which contains a list (in JSON
  format) of accepted credentials.  This may be optional depending on the
  authenticator being used.

* `--authentication_backoff_factor` - The agent will time out its authentication
  with the master based on exponential backoff. The timeout will be randomly
  chosen within the range `[min, min + factor*2^n]` where `n` is the number of
  failed attempts. To tune these parameters, set the
  `--authentication_timeout_[min|max|factor]` flags. (default: 1secs)

* `--authentication_timeout_min` - The minimum amount of time the agent waits
  before retrying authenticating with the master. See
  `--authentication_backoff_factor` for more details. (default: 5secs)

* `--authentication_timeout_max` - The maximum amount of time the agent waits
  before retrying authenticating with the master. See
  `--authentication_backoff_factor` for more details. (default: 1mins)

### Scheduler Driver

* `--authenticatee` - Analog to the master's `--authenticators` option to
  specify what module to use.  Defaults to `crammd5`.

* `--authentication_backoff_factor` - The scheduler will time out its
  authentication with the master based on exponential backoff. The timeout will
  be randomly chosen within the range `[min, min + factor*2^n]` where `n` is
  the number of failed attempts. To tune these parameters, set the
  `--authentication_timeout_[min|max|factor]` flags. (default: 1secs)

* `--authentication_timeout_min` - The minimum amount of time the scheduler
  waits before retrying authenticating with the master. See
  `--authentication_backoff_factor` for more details. (default: 5secs)

* `--authentication_timeout_max` - The maximum amount of time the scheduler
  waits before retrying authenticating with the master. See
  `--authentication_backoff_factor` for more details. (default: 1mins)

### Multiple HTTP Authenticators

Multiple HTTP authenticators may be loaded into the Mesos master and agent. In
order to load multiple authenticators, specify them as a comma-separated list
using the `--http_authenticators` flag. The authenticators will be called
serially, and the result of the first successful authentication attempt will be
returned.

If you wish to specify the default basic HTTP authenticator in addition to
custom authenticator modules, add the name `basic` to your authenticator list.
To specify the default JWT HTTP authenticator in addition to custom
authenticator modules, add the name `jwt` to your authenticator list.

### Executor

If HTTP executor authentication is enabled on the agent, then all requests from
HTTP executors must be authenticated. This includes the default executor, HTTP
command executors, and custom HTTP executors. By default, the agent's JSON web
token (JWT) HTTP authenticator is loaded to handle executor authentication on
both the executor and operator API endpoints. Note that command and custom
executors not using the HTTP API will remain unauthenticated.

When a secret key is loaded via the `--jwt_secret_key` flag, the agent will
generate a default JWT for each executor before it is launched. This token is
passed into the executor's environment via the
`MESOS_EXECUTOR_AUTHENTICATION_TOKEN` environment variable. In order to
authenticate with the agent, the executor should place this token into the
`Authorization` header of all its requests as follows:

        Authorization: Bearer MESOS_EXECUTOR_AUTHENTICATION_TOKEN

In order to upgrade an existing cluster to require executor authentication, the
following procedure should be followed:

1. Upgrade all agents, and provide each agent with a cryptographic key via the
   `--jwt_secret_key` flag. This key will be used to sign executor
   authentication tokens using the HMAC-SHA256 procedure.

2. Before executor authentication can be enabled successfully, all HTTP
   executors must have executor authentication tokens in their environment and
   support authentication. To accomplish this, executors which were already
   running before the upgrade must be restarted. This could either be done all
   at once, or the cluster may be left in this intermediate state while
   executors gradually turn over.

3. Once all running default/HTTP command executors have been launched by
   upgraded agents, and any custom HTTP executors have been upgraded, the agent
   processes can be restarted with the `--authenticate_http_executors` flag set.
   This will enable required HTTP executor authentication, and since all
   executors now have authentication tokens and support authentication, their
   requests to the agent will authenticate successfully.

Note that HTTP executors make use of the agent operator API in order to make
nested container calls. This means that authentication of the v1 agent operator
API should not be enabled (via `--authenticate_http_readwrite`) when HTTP
executor authentication is disabled, or HTTP executors will not be able to
function correctly.

### Framework

If framework authentication is enabled, each framework must be configured to
supply authentication credentials when registering with the Mesos master. How to
configure this differs between frameworks; consult your framework's
documentation for more information.

As a framework developer, supporting authentication is straightforward: the
scheduler driver handles the details of authentication when a `Credential`
object is passed to its constructor. To enable [authorization](authorization.md)
based on the authenticated principal, the framework developer should also copy
the `Credential.principal` into `FrameworkInfo.principal` when registering.

## CRAM-MD5 Example

1. Create the master's credentials file with the following content:

        {
          "credentials" : [
            {
              "principal": "principal1",
              "secret": "secret1"
            },
            {
              "principal": "principal2",
              "secret": "secret2"
            }
          ]
        }

2. Start the master using the credentials file (assuming the file is `/home/user/credentials`):

        ./bin/mesos-master.sh --ip=127.0.0.1 --work_dir=/var/lib/mesos --authenticate --authenticate_agents --credentials=/home/user/credentials

3. Create another file with a single credential in it (`/home/user/agent_credential`):

        {
          "principal": "principal1",
          "secret": "secret1"
        }

4. Start the agent:

        ./bin/mesos-agent.sh --master=127.0.0.1:5050 --credential=/home/user/agent_credential

5. Your new agent should have now successfully authenticated with the master.

6. You can test out framework authentication using one of the test frameworks
provided with Mesos as follows:

        MESOS_AUTHENTICATE=true DEFAULT_PRINCIPAL=principal2 DEFAULT_SECRET=secret2 ./src/test-framework --master=127.0.0.1:5050
