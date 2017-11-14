# Protobuf

## Project website:

https://github.com/google/protobuf

## How To Bundle:

```
curl -L -O https://github.com/google/protobuf/archive/<version>.tar.gz
tar -xzf <version>.tar.gz
cd <version>
./autogen.sh
cd ..
tar -czf <version>.tar.gz <version>
```

## Bundled Version

We bundle 3.5.0 in order to take advantage of C++ move support and
additional optimizations.
