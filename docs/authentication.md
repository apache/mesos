---
title: Apache Mesos - Authentication
layout: documentation
---

# Authentication

Authentication permits only trusted entities to interact with a Mesos cluster. Authentication can be used by Mesos in three ways:

1. To require that frameworks be authenticated in order to register with the master.
2. To require that slaves be authenticated in order to register with the master.
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
framework's *user*, which is the operating system account used by the slave to
run executors, and a framework's *[role](roles.md)*, which is used to determine
which resources a framework can use.

## Configuration

Authentication is configured by specifying command-line flags when starting the
Mesos master and slave processes. For more information, refer to the
[configuration](configuration.md) documentation.

### Master

* `--[no-]authenticate` - If `true`, only authenticated frameworks are allowed
  to register. If `false` (the default), unauthenticated frameworks are also
  allowed to register.

* `--[no-]authenticate_http` - If `true`, authentication is required to make
  HTTP requests to the HTTP endpoints that support authentication. If `false`
  (the default), all endpoints can be used without authentication.

* `--[no-]authenticate_slaves` - If `true`, only authenticated slaves are
  allowed to register. If `false` (the default), unauthenticated slaves are also
  allowed to register.

* `--authenticators` - Specifies which authenticator module to use.  The default
  is `crammd5`, but additional modules can be added using the `--modules`
  option.

* `--http_authenticators` - Specifies which HTTP authenticator module to use.
  The default is `basic` (basic HTTP authentication), but additional modules can
  be added using the `--modules` option.

* `--credentials` - The path to a text file which contains a list (in plaintext
  or JSON format) of accepted credentials.  This may be optional depending on
  the authenticator being used.

### Slave

* `--authenticatee` - Analog to the master's `--authenticators` option to
  specify what module to use.  Defaults to `crammd5`.

* `--credential` - Just like the master's `--credentials` option except that
  only one credential is allowed. This credential is used to identify the slave
  to the master.

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

        principal1 secret1
        principal2 secret2

2. Start the master using the credentials file (assuming the file is `~/credentials`):

        ./bin/mesos-master.sh --ip=127.0.0.1 --work_dir=/var/lib/mesos --authenticate --authenticate_slaves --credentials=~/credentials

3. Create another file with a single credential in it (`~/slave_credential`):

        principal1 secret1

4. Start the slave:

        ./bin/mesos-slave.sh --master=127.0.0.1:5050 --credential=~/slave_credential

5. Your new slave should have now successfully authenticated with the master.

6. You can test out framework authentication using one of the test frameworks
provided with Mesos as follows:

        MESOS_AUTHENTICATE=true DEFAULT_PRINCIPAL=principal2 DEFAULT_SECRET=secret2 ./src/test-framework --master=127.0.0.1:5050
