---
title: Apache Mesos - Secrets Handling
layout: documentation
---

# Secrets
Starting 1.4.0 release, Mesos allows tasks to populate environment variables and
file volumes with secret contents that are retrieved using a secret-resolver
interface. It also allows specifying image-pull secrets for private container
registry. This allows users to avoid exposing critical secrets in task
definitions. Secrets are fetched/resolved using a secret-resolver module (see
below).

NOTE: Secrets are only supported for Mesos containerizer and not for the Docker
containerizer.

## Secrets Message
Secrets can be specified using the following protobuf message:

```
message Secret {
  enum Type {
    UNKNOWN = 0;
    REFERENCE = 1;
    VALUE = 2;
  }

  message Reference {
    required string name = 1;
    optional string key = 2;
  }

  message Value {
    required bytes data = 1;
  }

  optional Type type = 1;

  optional Reference reference = 2;
  optional Value value = 3;
}
```

Secrets can be of type `reference` or `value` (only one of `reference` and `value` must be set).
A secret reference can be used by modules to refer to a secret stored in a secure back-end.
The `key` field can be used to reference a single value within a secret containing arbitrary key-value pairs.

For example, given a back-end secret store with a secret named "/my/secret" containing the following key-value pairs:

```
{
  "username": "my-user",
  "password": "my-password
}
```

The username could be referred to in a `Secret` by specifying "my/secret" for the `name` and "username" for the `key`.

Secret also supports pass-by-value where the value of a secret can be directly
passed in the message.

## Environment-based Secrets
Environment variables can either be traditional value-based or secret-based. For
the latter, one can specify a secret as part of environment definition as shown
in the following example:

```
{
  "variables" : [
    {
      "name": "MY_SECRET_ENV",
      "type": "SECRET",
      "secret": {
        "type": "REFERENCE",
        "reference": {
          "name": "/my/secret",
          "key": "username"
        }
      }
    },
    {
      "name": "MY_NORMAL_ENV",
      "value": "foo"
    }
  ]
}
```

## File-based Secrets
A new `volume/secret` isolator is available to create secret-based files inside
the task container. To use a secret, one can specify a new volume as follows:

```
{
  "mode": "RW",
  "container_path": "path/to/secret/file",
  "source":
  {
    "type": "SECRET",
    "secret": {
      "type": "REFERENCE",
      "reference": {
        "name": "/my/secret",
        "key": "username"
      }
    }
  }
}
```

This will create a tmpfs-based file mount in the container at "path/to/secret/file" which will contain the secret text fetched from the back-end secret store.

The `volume/secret` isolator is not enabled by default. To enable it, it must be specified in `--isolator=volume/secret` agent flag.

## Image-pull Secrets
Currently, image-pull secrets only support Docker images for Mesos
containerizer. Appc images are not supported.
One can store Docker config containing credentials to authenticate with Docker registry in the secret store.
The secret is expected to be a Docker config file in JSON format with UTF-8 character encoding.
The secret can then be referenced in the `Image` protobuf as follows:

```
{
  "type": "DOCKER",
  "docker":
  message Docker {
    "name": "<REGISTRY_HOST>/path/to/image",
    "secret": {
      "type": "REFERENCE",
      "reference": {
        "name": "/my/secret/docker/config"
      }
    }
  }
}
```

## SecretResolver Module
The SecretResolver module is called from Mesos agent to fetch/resolve any image-pull, environment-based, or file-based secrets. (See [Mesos Modules](modules.md) for more information on using Mesos modules).

```
class SecretResolver
{
  virtual process::Future<Secret::Value> resolve(const Secret& secret) const;
};
```

The default implementation simply resolves value-based Secrets. A custom secret-resolver module can be specified using the `--secret_resolver=<module-name>` agent flag.
