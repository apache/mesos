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

We bundle 1.10.0 for better CMake build support.

## Cherry Picks

- [Fixed undefined `_WIN32_WINNT` for generated files on Windows.](https://github.com/grpc/grpc/pull/15128)
- [Removed unnecessary dependencies of `codegen_init.o` for unsecure build.](https://github.com/grpc/grpc/pull/16323)
