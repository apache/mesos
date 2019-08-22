# Boost

README for the Mesos third party Boost distribution.

We use the Boost `bcp` utility to bundle only the minimum subset of Boost headers used by Mesos.
See: http://www.boost.org/doc/libs/1_65_0/tools/bcp/doc/html/index.html

## Instructions for upgrading Boost. (Based on Boost 1.65.0)
```
# Download Boost tarball e.g. boost_1_65_0.tar.gz

# Uncompress Boost and build bcp.
   $ tar -zxvf boost_1_65_0.tar.gz
   $ cd boost_1_65_0
   $ ./bootstrap.sh
   $ ./b2 tools/bcp

# Get list of all C++ source files in Mesos.
   $ find </path/to/mesos> -name "*.cpp" -o -name "*.hpp" > files.txt

# Scan the source files using bcp to produce a subset of the headers.
   $ mkdir ../boost-1.65.0
   $ cat files.txt | xargs -I {} ./dist/bin/bcp --scan --boost=./ {} ../boost-1.65.0

# Inspect contents of extracted headers and remove unnecessary lib files.
   $ cd ../boost-1.65.0
   $ rm -r libs

# Compress Boost directory and bundle it into Mesos.
   $ cd ..
   $ GZIP=--best tar -zcf boost-1.65.0.tar.gz boost-1.65.0
   $ cp boost-1.65.0.tar.gz </path/to/mesos>/3rdparty/

# Update 3rdparty/cmake/Versions.cmake with the new version and
#  its SHA-256 hash. You can obtain the hash as follows, make sure
#  to do this on the stripped release:
  $ openssl sha256 3rdparty/boost-1.65.0.tar.gz


# Update this README if needed.
```
