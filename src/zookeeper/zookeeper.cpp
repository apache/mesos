// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <stdint.h>

#include <iostream>
#include <map>
#include <tuple>

#include <glog/logging.h>

#include <mesos/zookeeper/zookeeper.hpp>

#include <process/defer.hpp>
#include <process/dispatch.hpp>
#include <process/id.hpp>
#include <process/process.hpp>
#include <process/timeout.hpp>

#include <stout/duration.hpp>
#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>
#include <stout/unreachable.hpp>

using namespace process;

using std::map;
using std::string;
using std::tuple;
using std::vector;

class ZooKeeperProcess : public Process<ZooKeeperProcess>
{
public:
  ZooKeeperProcess(
      ZooKeeper* zk,
      const string& servers,
      const Duration& sessionTimeout,
      Watcher* watcher)
    : ProcessBase(ID::generate("zookeeper")),
      servers(servers),
      sessionTimeout(sessionTimeout),
      zh(nullptr)
  {
    // We bind the Watcher::process callback so we can pass it to the
    // C callback as a pointer and invoke it directly.
    callback = lambda::bind(
        &Watcher::process,
        watcher,
        lambda::_1,
        lambda::_2,
        lambda::_3,
        lambda::_4);
  }

  void initialize() override
  {
    // There are two different timeouts here:
    //
    // (1) `sessionTimeout` is the client's proposed value for the
    // ZooKeeper session timeout.
    //
    // (2) `initLoopTimeout` is how long we are prepared to wait,
    // calling `zookeeper_init` in a loop, until a call succeeds.
    //
    // `sessionTimeout` is used to determine the liveness of our
    // ZooKeeper session. `initLoopTimeout` determines how long to
    // retry erroneous calls to `zookeeper_init`, because there are
    // cases when temporary DNS outages cause `zookeeper_init` to
    // return failure. ZooKeeper masks EAI_AGAIN as EINVAL and a name
    // resolution timeout may be upwards of 30 seconds. As such, a 10
    // second timeout (the default `sessionTimeout`) is not enough. We
    // hardcode `initLoopTimeout` to 10 minutes ensure we're trying
    // again in the face of temporary name resolution failures. See
    // MESOS-1523 for more information.
    //
    // Note that there are cases where `zookeeper_init` returns
    // success but we don't see a subsequent ZooKeeper event
    // indicating that our connection has been established. A common
    // cause for this situation is that the ZK hostname list resolves
    // to unreachable IP addresses. ZooKeeper will continue looping,
    // trying to connect to the list of IPs but never attempting to
    // re-resolve the input hostnames. Since DNS may have changed, we
    // close the ZK handle and create a new handle to ensure that ZK
    // will try to re-resolve the configured list of hostnames.
    // However, since we can't easily check if the `connected` ZK
    // event has been fired for this session yet, we implement this
    // timeout in `GroupProcess`. See MESOS-4546 for more information.
    const Timeout initLoopTimeout = Timeout::in(Minutes(10));

    while (!initLoopTimeout.expired()) {
      zh = zookeeper_init(
          servers.c_str(),
          event,
          static_cast<int>(sessionTimeout.ms()),
          nullptr,
          &callback,
          0);

      // Unfortunately, EINVAL is highly overloaded in zookeeper_init
      // and can correspond to:
      //   (1) Empty / invalid 'host' string format.
      //   (2) Any getaddrinfo error other than EAI_NONAME,
      //       EAI_NODATA, and EAI_MEMORY are mapped to EINVAL.
      // The errors EAI_NONAME and EAI_NODATA are mapped to ENOENT.
      // Either way, retrying is not problematic.
      if (zh == nullptr && (errno == EINVAL || errno == ENOENT)) {
        ErrnoError error("zookeeper_init failed");
        LOG(WARNING) << error.message << " ; retrying in 1 second";
        os::sleep(Seconds(1));
        continue;
      }

      break;
    }

    if (zh == nullptr) {
      PLOG(FATAL) << "Failed to create ZooKeeper, zookeeper_init";
    }
  }

  void finalize() override
  {
    int ret = zookeeper_close(zh);
    if (ret != ZOK) {
      LOG(FATAL) << "Failed to cleanup ZooKeeper, zookeeper_close: "
                 << zerror(ret);
    }
  }

  int getState()
  {
    return zoo_state(zh);
  }

  int64_t getSessionId()
  {
    return zoo_client_id(zh)->client_id;
  }

  Duration getSessionTimeout()
  {
    // ZooKeeper server uses int representation of milliseconds for
    // session timeouts.
    // See:
    // http://zookeeper.apache.org/doc/trunk/zookeeperProgrammers.html
    return Milliseconds(zoo_recv_timeout(zh));
  }

  Future<int> authenticate(const string& scheme, const string& credentials)
  {
    Promise<int>* promise = new Promise<int>();

    Future<int> future = promise->future();

    tuple<Promise<int>*>* args = new tuple<Promise<int>*>(promise);

    int ret = zoo_add_auth(
        zh,
        scheme.c_str(),
        credentials.data(),
        static_cast<int>(credentials.size()),
        voidCompletion,
        args);

    if (ret != ZOK) {
      delete promise;
      delete args;
      return ret;
    }

    return future;
  }

  Future<int> create(
      const string& path,
      const string& data,
      const ACL_vector& acl,
      int flags,
      string* result)
  {
    Promise<int>* promise = new Promise<int>();

    Future<int> future = promise->future();

    tuple<Promise<int>*, string*>* args =
      new tuple<Promise<int>*, string*>(promise, result);

    int ret = zoo_acreate(
        zh,
        path.c_str(),
        data.data(),
        static_cast<int>(data.size()),
        &acl,
        flags,
        stringCompletion,
        args);

    if (ret != ZOK) {
      delete promise;
      delete args;
      return ret;
    }

    return future;
  }

  Future<int> create(
      const string& path,
      const string& data,
      const ACL_vector& acl,
      int flags,
      string* result,
      bool recursive)
  {
    if (!recursive) {
      return create(path, data, acl, flags, result);
    }

    // First check if the path exists.
    return exists(path, false, nullptr)
      .then(defer(self(),
                  &Self::_create,
                  path,
                  data,
                  acl,
                  flags,
                  result,
                  lambda::_1));
  }

  Future<int> _create(
      const string& path,
      const string& data,
      const ACL_vector& acl,
      int flags,
      string* result,
      int code)
  {
    if (code == ZOK) {
      return ZNODEEXISTS;
    }

    // Now recursively create the parent path.
    // NOTE: We don't use 'dirname()' to get the parent path here
    // because, it doesn't return the expected path when a path ends
    // with "/". For example, to create path "/a/b/", we want to
    // recursively create "/a/b", instead of just creating "/a".
    const string& parent = path.substr(0, path.find_last_of('/'));
    if (!parent.empty()) {
      return create(parent, "", acl, 0, result, true)
        .then(defer(self(),
                    &Self::__create,
                    path,
                    data,
                    acl,
                    flags,
                    result,
                    lambda::_1));
    }

    return __create(path, data, acl, flags, result, ZOK);
  }

  Future<int> __create(
      const string& path,
      const string& data,
      const ACL_vector& acl,
      int flags,
      string* result,
      int code)
  {
    if (code != ZOK && code != ZNODEEXISTS) {
      return code;
    }

    // Finally create the path.
    // TODO(vinod): Delete any intermediate nodes created if this fails.
    // This requires synchronization because the deletion might affect
    // other callers (different threads/processes) acting on this path.
    return create(path, data, acl, flags, result);
  }

  Future<int> remove(const string& path, int version)
  {
    Promise<int>* promise = new Promise<int>();

    Future<int> future = promise->future();

    tuple<Promise<int>*>* args = new tuple<Promise<int>*>(promise);

    int ret = zoo_adelete(zh, path.c_str(), version, voidCompletion, args);

    if (ret != ZOK) {
      delete promise;
      delete args;
      return ret;
    }

    return future;
  }

  Future<int> exists(const string& path, bool watch, Stat* stat)
  {
    Promise<int>* promise = new Promise<int>();

    Future<int> future = promise->future();

    tuple<Promise<int>*, Stat*>* args =
      new tuple<Promise<int>*, Stat*>(promise, stat);

    int ret = zoo_aexists(zh, path.c_str(), watch, statCompletion, args);

    if (ret != ZOK) {
      delete promise;
      delete args;
      return ret;
    }

    return future;
  }

  Future<int> get(const string& path, bool watch, string* result, Stat* stat)
  {
    Promise<int>* promise = new Promise<int>();

    Future<int> future = promise->future();

    tuple<Promise<int>*, string*, Stat*>* args =
      new tuple<Promise<int>*, string*, Stat*>(promise, result, stat);

    int ret = zoo_aget(zh, path.c_str(), watch, dataCompletion, args);

    if (ret != ZOK) {
      delete promise;
      delete args;
      return ret;
    }

    return future;
  }

  Future<int> getChildren(
      const string& path,
      bool watch,
      vector<string>* results)
  {
    Promise<int>* promise = new Promise<int>();

    Future<int> future = promise->future();

    tuple<Promise<int>*, vector<string>*>* args =
      new tuple<Promise<int>*, vector<string>*>(promise, results);

    int ret =
      zoo_aget_children(zh, path.c_str(), watch, stringsCompletion, args);

    if (ret != ZOK) {
      delete promise;
      delete args;
      return ret;
    }

    return future;
  }

  Future<int> set(const string& path, const string& data, int version)
  {
    Promise<int>* promise = new Promise<int>();

    Future<int> future = promise->future();

    tuple<Promise<int>*, Stat*>* args =
      new tuple<Promise<int>*, Stat*>(promise, nullptr);

    int ret = zoo_aset(
        zh,
        path.c_str(),
        data.data(),
        static_cast<int>(data.size()),
        version,
        statCompletion,
        args);

    if (ret != ZOK) {
      delete promise;
      delete args;
      return ret;
    }

    return future;
  }

private:
  // This method is registered as a watcher callback function and is
  // invoked by a single ZooKeeper event thread.
  static void event(
      zhandle_t* zh,
      int type,
      int state,
      const char* path,
      void* context)
  {
    lambda::function<void(int, int, int64_t, const string&)>* callback =
      static_cast<lambda::function<void(int, int, int64_t, const string&)>*>(
          context);

    (*callback)(type, state, zoo_client_id(zh)->client_id, string(path));
  }

  static void voidCompletion(int ret, const void *data)
  {
    const tuple<Promise<int>*>* args =
      reinterpret_cast<const tuple<Promise<int>*>*>(data);

    Promise<int>* promise = std::get<0>(*args);

    promise->set(ret);

    delete promise;
    delete args;
  }

  static void stringCompletion(int ret, const char* value, const void* data)
  {
    const tuple<Promise<int>*, string*> *args =
      reinterpret_cast<const tuple<Promise<int>*, string*>*>(data);

    Promise<int>* promise = std::get<0>(*args);
    string* result = std::get<1>(*args);

    if (ret == 0) {
      if (result != nullptr) {
        result->assign(value);
      }
    }

    promise->set(ret);

    delete promise;
    delete args;
  }

  static void statCompletion(int ret, const Stat* stat, const void* data)
  {
    const tuple<Promise<int>*, Stat*>* args =
      reinterpret_cast<const tuple<Promise<int>*, Stat*>*>(data);

    Promise<int>* promise = std::get<0>(*args);
    Stat *stat_result = std::get<1>(*args);

    if (ret == 0) {
      if (stat_result != nullptr) {
        *stat_result = *stat;
      }
    }

    promise->set(ret);

    delete promise;
    delete args;
  }

  static void dataCompletion(
      int ret,
      const char* value,
      int value_len,
      const Stat* stat,
      const void* data)
  {
    const tuple<Promise<int>*, string*, Stat*>* args =
      reinterpret_cast<const tuple<Promise<int>*, string*, Stat*>*>(data);

    Promise<int>* promise = std::get<0>(*args);
    string* result = std::get<1>(*args);
    Stat* stat_result = std::get<2>(*args);

    if (ret == 0) {
      if (result != nullptr) {
        result->assign(value, value_len);
      }

      if (stat_result != nullptr) {
        *stat_result = *stat;
      }
    }

    promise->set(ret);

    delete promise;
    delete args;
  }

  static void stringsCompletion(
      int ret,
      const String_vector* values,
      const void* data)
  {
    const tuple<Promise<int>*, vector<string>*>* args =
      reinterpret_cast<const tuple<Promise<int>*, vector<string>*>*>(data);

    Promise<int>* promise = std::get<0>(*args);
    vector<string>* results = std::get<1>(*args);

    if (ret == 0) {
      if (results != nullptr) {
        for (int i = 0; i < values->count; i++) {
          results->push_back(values->data[i]);
        }
      }
    }

    promise->set(ret);

    delete promise;
    delete args;
  }

private:
  friend class ZooKeeper;

  const string servers; // ZooKeeper host:port pairs.
  const Duration sessionTimeout; // ZooKeeper session timeout.

  zhandle_t* zh; // ZooKeeper connection handle.

  // Callback for invoking Watcher::process with the 'Watcher*'
  // receiver already bound.
  lambda::function<void(int, int, int64_t, const string&)> callback;
};


ZooKeeper::ZooKeeper(
    const string& servers,
    const Duration& sessionTimeout,
    Watcher* watcher)
{
  process = new ZooKeeperProcess(this, servers, sessionTimeout, watcher);
  spawn(process);
}


ZooKeeper::~ZooKeeper()
{
  terminate(process);
  wait(process);
  delete process;
}


int ZooKeeper::getState()
{
  return dispatch(process, &ZooKeeperProcess::getState).get();
}


int64_t ZooKeeper::getSessionId()
{
  return dispatch(process, &ZooKeeperProcess::getSessionId).get();
}


Duration ZooKeeper::getSessionTimeout() const
{
  return dispatch(process, &ZooKeeperProcess::getSessionTimeout).get();
}


int ZooKeeper::authenticate(const string& scheme, const string& credentials)
{
  return dispatch(
      process,
      &ZooKeeperProcess::authenticate,
      scheme,
      credentials).get();
}


int ZooKeeper::create(
    const string& path,
    const string& data,
    const ACL_vector& acl,
    int flags,
    string* result,
    bool recursive)
{
  return dispatch(
      process,
      &ZooKeeperProcess::create,
      path,
      data,
      acl,
      flags,
      result,
      recursive).get();
}


int ZooKeeper::remove(const string& path, int version)
{
  return dispatch(process, &ZooKeeperProcess::remove, path, version).get();
}


int ZooKeeper::exists(const string& path, bool watch, Stat* stat)
{
  return dispatch(
      process,
      &ZooKeeperProcess::exists,
      path,
      watch,
      stat).get();
}


int ZooKeeper::get(const string& path, bool watch, string* result, Stat* stat)
{
  return dispatch(
      process,
      &ZooKeeperProcess::get,
      path,
      watch,
      result,
      stat).get();
}


int ZooKeeper::getChildren(
    const string& path,
    bool watch,
    vector<string>* results)
{
  return dispatch(
      process,
      &ZooKeeperProcess::getChildren,
      path,
      watch,
      results).get();
}


int ZooKeeper::set(const string& path, const string& data, int version)
{
  return dispatch(
      process,
      &ZooKeeperProcess::set,
      path,
      data,
      version).get();
}


string ZooKeeper::message(int code) const
{
  return string(zerror(code));
}


bool ZooKeeper::retryable(int code)
{
  switch (code) {
    case ZCONNECTIONLOSS:
    case ZOPERATIONTIMEOUT:
    case ZSESSIONEXPIRED:
    case ZSESSIONMOVED:
      return true;

    case ZOK: // No need to retry!

    case ZSYSTEMERROR: // Should not be encountered, here for completeness.
    case ZRUNTIMEINCONSISTENCY:
    case ZDATAINCONSISTENCY:
    case ZMARSHALLINGERROR:
    case ZUNIMPLEMENTED:
    case ZBADARGUMENTS:
    case ZINVALIDSTATE:

    case ZAPIERROR: // Should not be encountered, here for completeness.
    case ZNONODE:
    case ZNOAUTH:
    case ZBADVERSION:
    case ZNOCHILDRENFOREPHEMERALS:
    case ZNODEEXISTS:
    case ZNOTEMPTY:
    case ZINVALIDCALLBACK:
    case ZINVALIDACL:
    case ZAUTHFAILED:
    case ZCLOSING:
    case ZNOTHING: // Is this used? It's not exposed in the Java API.
      return false;

    default:
      LOG(FATAL) << "Unknown ZooKeeper code: " << code;
      UNREACHABLE(); // Make compiler happy.
  }
}
