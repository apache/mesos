/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <glog/logging.h>

#include <iostream>
#include <map>

#include <boost/tuple/tuple.hpp>

#include <process/dispatch.hpp>
#include <process/once.hpp>
#include <process/process.hpp>

#include <stout/duration.hpp>
#include <stout/fatal.hpp>
#include <stout/foreach.hpp>
#include <stout/path.hpp>
#include <stout/strings.hpp>

#include "zookeeper/zookeeper.hpp"

using boost::tuple;

using process::Future;
using process::PID;
using process::Process;
using process::Promise;

using std::map;
using std::string;
using std::vector;


class ZooKeeperImpl
{
public:
  ZooKeeperImpl(ZooKeeper* zk,
                const string& servers,
                const Duration& timeout,
                Watcher* watcher)
    : servers(servers),
      zk(zk),
      watcher(watcher)
  {
    if (watcher == NULL) {
      LOG(FATAL) << "Cannot instantiate ZooKeeper with NULL watcher";
    }

    zh = zookeeper_init(
        servers.c_str(),
        event,
        static_cast<int>(timeout.ms()),
        NULL,
        this,
        0);

    if (zh == NULL) {
      PLOG(FATAL) << "Failed to create ZooKeeper, zookeeper_init";
    }
  }

  ~ZooKeeperImpl()
  {
    int ret = zookeeper_close(zh);
    if (ret != ZOK) {
      LOG(FATAL) << "Failed to cleanup ZooKeeper, zookeeper_close: "
                 << zerror(ret);
    }
  }

  Future<int> authenticate(const string& scheme, const string& credentials)
  {
    Promise<int>* promise = new Promise<int>();

    Future<int> future = promise->future();

    tuple<Promise<int>*>* args = new tuple<Promise<int>*>(promise);

    int ret = zoo_add_auth(zh, scheme.c_str(), credentials.data(),
                           credentials.size(), voidCompletion, args);

    if (ret != ZOK) {
      delete promise;
      delete args;
      return ret;
    }

    return future;
  }

  Future<int> create(const string& path, const string& data,
                     const ACL_vector& acl, int flags, string* result)
  {
    Promise<int>* promise = new Promise<int>();

    Future<int> future = promise->future();

    tuple<Promise<int>*, string*>* args =
      new tuple<Promise<int>*, string*>(promise, result);

    int ret = zoo_acreate(zh, path.c_str(), data.data(), data.size(), &acl,
                          flags, stringCompletion, args);

    if (ret != ZOK) {
      delete promise;
      delete args;
      return ret;
    }

    return future;
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

  Future<int> getChildren(const string& path,
                          bool watch,
                          vector<string>* results)
  {
    Promise<int>* promise = new Promise<int>();

    Future<int> future = promise->future();

    tuple<Promise<int>*, vector<string>*>* args =
      new tuple<Promise<int>*, vector<string>*>(promise, results);

    int ret = zoo_aget_children(zh, path.c_str(), watch, stringsCompletion,
                                args);

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
      new tuple<Promise<int>*, Stat*>(promise, NULL);

    int ret = zoo_aset(zh, path.c_str(), data.data(), data.size(),
                       version, statCompletion, args);

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
      void* ctx)
  {
    ZooKeeperImpl* impl = static_cast<ZooKeeperImpl*>(ctx);
    impl->watcher->process(impl->zk, type, state, string(path));
  }


  static void voidCompletion(int ret, const void *data)
  {
    const tuple<Promise<int>*>* args =
      reinterpret_cast<const tuple<Promise<int>*>*>(data);

    Promise<int>* promise = (*args).get<0>();

    promise->set(ret);

    delete promise;
    delete args;
  }


  static void stringCompletion(int ret, const char* value, const void* data)
  {
    const tuple<Promise<int>*, string*> *args =
      reinterpret_cast<const tuple<Promise<int>*, string*>*>(data);

    Promise<int>* promise = (*args).get<0>();
    string* result = (*args).get<1>();

    if (ret == 0) {
      if (result != NULL) {
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

    Promise<int>* promise = (*args).get<0>();
    Stat *stat_result = (*args).get<1>();

    if (ret == 0) {
      if (stat_result != NULL) {
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

    Promise<int>* promise = (*args).get<0>();
    string* result = (*args).get<1>();
    Stat* stat_result = (*args).get<2>();

    if (ret == 0) {
      if (result != NULL) {
        result->assign(value, value_len);
      }

      if (stat_result != NULL) {
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

    Promise<int>* promise = (*args).get<0>();
    vector<string>* results = (*args).get<1>();

    if (ret == 0) {
      if (results != NULL) {
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

  ZooKeeper* zk; // ZooKeeper instance.
  zhandle_t* zh; // ZooKeeper connection handle.

  Watcher* watcher; // Associated Watcher instance.
};


ZooKeeper::ZooKeeper(const string& servers,
                     const Duration& timeout,
                     Watcher* watcher)
{
  impl = new ZooKeeperImpl(this, servers, timeout, watcher);
}


ZooKeeper::~ZooKeeper()
{
  delete impl;
}


int ZooKeeper::getState()
{
  return zoo_state(impl->zh);
}


int64_t ZooKeeper::getSessionId()
{
  return zoo_client_id(impl->zh)->client_id;
}


Duration ZooKeeper::getSessionTimeout() const
{
  // ZooKeeper server uses int representation of milliseconds for
  // session timeouts.
  // See:
  // http://zookeeper.apache.org/doc/trunk/zookeeperProgrammers.html
  return Milliseconds(zoo_recv_timeout(impl->zh));
}


int ZooKeeper::authenticate(const string& scheme, const string& credentials)
{
  return impl->authenticate(scheme, credentials).get();
}


int ZooKeeper::create(
    const string& path,
    const string& data,
    const ACL_vector& acl,
    int flags,
    string* result,
    bool recursive)
{
  if (!recursive) {
    return impl->create(path, data, acl, flags, result).get();
  }

  // First check if the path exists.
  int code = impl->exists(path, false, NULL).get();
  if (code == ZOK) {
    return ZNODEEXISTS;
  }

  // Now recursively create the parent path.
  // NOTE: We don't use 'dirname()' to get the parent path here
  // because, it doesn't return the expected path when a path ends
  // with "/". For example, to create path "/a/b/", we want to
  // recursively create "/a/b", instead of just creating "/a".
  const string& parent = path.substr(0, path.find_last_of("/"));
  if (!parent.empty()) {
    code = create(parent, "", acl, 0, result, true);
    if (code != ZOK && code != ZNODEEXISTS) {
      return code;
    }
  }

  // Finally create the path.
  // TODO(vinod): Delete any intermediate nodes created if this fails.
  // This requires synchronization because the deletion might affect
  // other callers (different threads/processes) acting on this path.
  return impl->create(path, data, acl, flags, result).get();
}


int ZooKeeper::remove(const string& path, int version)
{
  return impl->remove(path, version).get();
}


int ZooKeeper::exists(const string& path, bool watch, Stat* stat)
{
  return impl->exists(path, watch, stat).get();
}


int ZooKeeper::get(const string& path, bool watch, string* result, Stat* stat)
{
  return impl->get(path, watch, result, stat).get();
}


int ZooKeeper::getChildren(const string& path, bool watch,
                           vector<string>* results)
{
  return impl->getChildren(path, watch, results).get();
}


int ZooKeeper::set(const string& path, const string& data, int version)
{
  return impl->set(path, data, version).get();
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
  }
}
