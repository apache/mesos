# rapidjson

Found here:
https://github.com/Tencent/rapidjson

## License Notes
rapidjson has an MIT license but includes a binary that has the JSON license which cannot be included in Apache products:

https://www.apache.org/legal/resolved.html#json

Therefore we must strip content from the release.

## How to upgrade

Be sure to strip the release in order to comply with the licensing
requirements noted above, as well as to lower the size impact on
the mesos git repository:

```
# Download the latest release, e.g.:
$ curl --location https://github.com/Tencent/rapidjson/archive/v1.1.0.tar.gz -o rapidjson-1.1.0.tar.gz

# Extract, strip everything but the license and include
# folder, and re-bundle.
$ mkdir tmp
$ tar -zxvf rapidjson-1.1.0.tar.gz --directory=tmp
$ mkdir rapidjson-1.1.0
$ mv tmp/rapidjson-1.1.0/license.txt rapidjson-1.1.0
$ mv tmp/rapidjson-1.1.0/include rapidjson-1.1.0
$ tar -zcvf rapidjson-1.1.0.tar.gz rapidjson-1.1.0

# Place it into 3rdparty.
# Update 3rdparty/versions.am with the new version.
# Update 3rdparty/cmake/Versions.cmake with the new version and
# its SHA-256 hash. You can obtain the hash as follows, make sure
# to do this on the stripped release:
$ openssl sha -sha256 3rdparty/rapidjson-1.1.0.tar.gz
```
