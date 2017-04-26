set(BOOST_VERSION       "1.53.0")
set(CURL_VERSION        "7.43.0")
set(ELFIO_VERSION       "3.2")
set(GMOCK_VERSION       "1.7.0")
set(HTTP_PARSER_VERSION "2.6.2")
set(LEVELDB_VERSION     "1.19")
set(LIBAPR_VERSION      "1.5.2")
set(LIBEV_VERSION       "4.22")
# TODO(hausdorff): (MESOS-3529) transition this back to a non-beta version.
set(LIBEVENT_VERSION    "2.1.5-beta")
set(NVML_VERSION        "352.79")
set(PICOJSON_VERSION    "1.3.0")
set(ZLIB_VERSION        "1.2.8")

# Platform-dependent versions.
if (NOT WIN32)
  set(GLOG_VERSION      "0.3.3")
  set(PROTOBUF_VERSION  "2.6.1")
  set(ZOOKEEPER_VERSION "3.4.8")
else (NOT WIN32)
  # TODO(hausdorff): (MESOS-3394) Upgrade Windows to use glog v0.3.5 when they
  # release it, as that will contain fixes that will allow us to build glog on
  # Windows, as well as build using CMake directly. For now, we simply point
  # Windows builds at a commit hash in the glog history that has all the
  # functionality we want.
  set(GLOG_VERSION      "da816ea70")

  # TODO(hausdorff): (MESOS-3453) this is a patched version of the protobuf
  # library that compiles on Windows. We need to send this as a PR back to the
  # protobuf project.
  set(PROTOBUF_VERSION  "3.0.0-beta-2")

  # The latest release of ZK, 3.4.7, does not compile on Windows. Therefore, we
  # pick a recent commit that does until the next release stabilizes.
  set(ZOOKEEPER_VERSION "3.5.2-alpha")
endif (NOT WIN32)
