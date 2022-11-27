# gRPC

## Project website

https://github.com/grpc/grpc

## How To Bundle

```sh
git clone -b v<version> https://github.com/grpc/grpc.git grpc-<version>
(cd grpc-<version> && git submodule update --init third_party/cares)
tar zcvf grpc-<version>.tar.gz --exclude .git grpc-<version>
```

## Bundled Version

We bundle 1.11.1 for better CMake build support, and s390x support.

## Cherry Picks

- [Fixed undefined `_WIN32_WINNT` for generated files on Windows.](https://github.com/chhsia0/grpc/commit/90869cffbd0cd05ee663e1b81cda169dd40cdf22)

  Upstream PR: https://github.com/grpc/grpc/pull/15128

- [Removed unnecessary dependencies of `codegen_init.o` for unsecure build.](https://github.com/chhsia0/grpc/commit/6531532de6a35ed8e00e24d3b60e88fd90d01335)

  Upstream PR: https://github.com/grpc/grpc/pull/16323

- [CMake: Automatic fallbacking on system's OpenSSL if it only has NPN.](https://github.com/chhsia0/grpc/commit/5c13ad2a3df1108184c716379818eab6fc0ba72d)

  Upstream PR: https://github.com/grpc/grpc/pull/17726
