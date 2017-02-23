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

/**
 * ZooKeeper C++ API.
 *
 * To provide for varying underlying implementations the pimpl idiom
 * (also known as the compiler-firewall, bridge pattern, etc) was used
 * for the ZooKeeper class.
*/
#ifndef __MESOS_ZOOKEEPER_HPP__
#define __MESOS_ZOOKEEPER_HPP__

#include <stdint.h>

#include <zookeeper.h>

#ifdef __WINDOWS__
// NOTE: We need to undefine this macro to prevent it from bleeding
// into our code and thereby break compilation of our namespaced ACLs.
// This macro is defined in zookeeper/src/c/include/winconfig.h.
#undef ACL
#endif // __WINDOWS__

#include <string>
#include <vector>

#include <stout/duration.hpp>


/* Forward declarations of classes we are using. */
class ZooKeeper;
class ZooKeeperProcess;

/**
 * This interface specifies the public interface an event handler
 * class must implement. A ZooKeeper client will get various events
 * from the ZooKeeper server it connects to. An application using such
 * a client handles these events by registering a callback object with
 * the client. The callback object is expected to be an instance of a
 * class that implements Watcher interface.
 *
 * Note that the watcher is invoked by ZooKeeper from a single thread.
 * See http://zookeeper.apache.org/doc/trunk/zookeeperProgrammers.html#C+Binding
 */
class Watcher
{
public:
  virtual void process(
      int type,
      int state,
      int64_t sessionId,
      const std::string& path) = 0;

  virtual ~Watcher() {}
};


/*
 * TODO(benh): Clean up this documentation.
 *
 * This is the main class of ZooKeeper client library. To use a
 * ZooKeeper service, an application must first instantiate an object
 * of ZooKeeper class. All the iterations will be done by calling the
 * methods of ZooKeeper class.
 *
 * Once a connection to a server is established, a session ID is
 * assigned to the client. The client will send heart beats to the
 * server periodically to keep the session valid.
 *
 * The application can call ZooKeeper APIs through a client as long as
 * the session ID of the client remains valid.
 *
 * If for some reason, the client fails to send heart beats to the
 * server for a prolonged period of time (exceeding the sessionTimeout
 * value, for instance), the server will expire the session, and the
 * session ID will become invalid. The client object will no longer be
 * usable. To make ZooKeeper API calls, the application must create a
 * new client object.
 *
 * If the ZooKeeper server the client currently connects to fails or
 * otherwise does not respond, the client will automatically try to
 * connect to another server before its session ID expires. If
 * successful, the application can continue to use the client.
 *
 * Some successful ZooKeeper API calls can leave watches on the "data
 * nodes" in the ZooKeeper server. Other successful ZooKeeper API
 * calls can trigger those watches. Once a watch is triggered, an
 * event will be delivered to the client which left the watch at the
 * first place. Each watch can be triggered only once. Thus, up to one
 * event will be delivered to a client for every watch it leaves.
 *
 * A client needs an object of a class implementing Watcher interface
 * for processing the events delivered to the client. When a client
 * drops current connection and re-connects to a server, all the
 * existing watches are considered as being triggered but the
 * undelivered events are lost. To emulate this, the client will
 * generate a special event to tell the event handler a connection has
 * been dropped. This special event has type EventNone and state
 * sKeeperStateDisconnected.
 */
class ZooKeeper
{
public:
  /**
   * \brief instantiate new ZooKeeper client.
   *
   * The constructor initiates a new session, however session
   * establishment is asynchronous, meaning that the session should
   * not be considered established until (and unless) an event of
   * state ZOO_CONNECTED_STATE is received.
   * \param servers comma-separated host:port pairs, each corresponding
   *    to a ZooKeeper server. e.g. "127.0.0.1:3000,127.0.0.1:3001"
   * \param watcher the instance of Watcher that receives event
   *    callbacks. When notifications are triggered the Watcher::process
   *    method will be invoked.
   */
  ZooKeeper(const std::string& servers,
            const Duration& sessionTimeout,
            Watcher* watcher);

  ~ZooKeeper();

  /**
   * \brief get the state of the zookeeper connection.
   *
   * The return value will be one of the \ref State Consts.
   */
  int getState();

  /**
   * \brief get the current session id.
   *
   * The current session id or 0 if no session is established.
   */
  int64_t getSessionId();

  /**
   * \brief get the current session timeout.
   *
   * The session timeout requested by the client or the negotiated
   * session timeout after the session is established with
   * ZooKeeper. Note that this might differ from the initial
   * `sessionTimeout` specified when this instance was constructed.
   */
  Duration getSessionTimeout() const;

  /**
   * \brief authenticate synchronously.
   */
  int authenticate(const std::string& scheme, const std::string& credentials);

  /**
   * \brief create a node synchronously.
   *
   * This method will create a node in ZooKeeper. A node can only be
   * created if it does not already exists. The Create Flags affect
   * the creation of nodes.  If ZOO_EPHEMERAL flag is set, the node
   * will automatically get removed if the client session goes
   * away. If the ZOO_SEQUENCE flag is set, a unique monotonically
   * increasing sequence number is appended to the path name.
   *
   * \param path The name of the node. Expressed as a file name with
   *    slashes separating ancestors of the node.
   * \param data The data to be stored in the node.
   * \param acl The initial ACL of the node. If null, the ACL of the
   *    parent will be used.
   * \param flags this parameter can be set to 0 for normal create or
   *    an OR of the Create Flags
   * \param result A string which will be filled with the path of
   *    the new node (this might be different than the supplied path
   *    because of the ZOO_SEQUENCE flag).  The path string will always
   *    be null-terminated.
   * \param recursive if true, attempts to create all intermediate
   *   znodes as required; note that 'flags' and 'data' will only be
   *   applied to the creation of 'basename(path)'.
   * \return  one of the following codes are returned:
   * ZOK operation completed successfully
   * ZNONODE the parent node does not exist.
   * ZNODEEXISTS the node already exists
   * ZNOAUTH the client does not have permission.
   * ZNOCHILDRENFOREPHEMERALS cannot create children of ephemeral nodes.
   * ZBADARGUMENTS - invalid input parameters
   * ZINVALIDSTATE - state is ZOO_SESSION_EXPIRED_STATE or ZOO_AUTH_FAILED_STATE
   * ZMARSHALLINGERROR - failed to marshall a request; possibly, out of memory
   */
  int create(
      const std::string& path,
      const std::string& data,
      const ACL_vector& acl,
      int flags,
      std::string* result,
      bool recursive = false);

  /**
   * \brief delete a node in zookeeper synchronously.
   *
   * \param path the name of the node. Expressed as a file name with
   *    slashes separating ancestors of the node.
   * \param version the expected version of the node. The function
   *    will fail if the actual version of the node does not match the
   *    expected version. If -1 is used the version check will not take
   *    place.
   * \return one of the following values is returned.
   * ZOK operation completed successfully
   * ZNONODE the node does not exist.
   * ZNOAUTH the client does not have permission.
   * ZBADVERSION expected version does not match actual version.
   * ZNOTEMPTY children are present; node cannot be deleted.
   * ZBADARGUMENTS - invalid input parameters
   * ZINVALIDSTATE - state is ZOO_SESSION_EXPIRED_STATE or ZOO_AUTH_FAILED_STATE
   * ZMARSHALLINGERROR - failed to marshall a request; possibly, out of memory
   */
  int remove(const std::string& path, int version);

  /**
   * \brief checks the existence of a node in zookeeper synchronously.
   *
   * \param path the name of the node. Expressed as a file name with
   *    slashes separating ancestors of the node.
   * \param watch if true, a watch will be set at the server to
   *    notify the client if the node changes. The watch will be set even
   *    if the node does not exist. This allows clients to watch for
   *    nodes to appear.
   * \param stat the return stat value of the node.
   * \return return code of the function call.
   * ZOK operation completed successfully
   * ZNONODE the node does not exist.
   * ZNOAUTH the client does not have permission.
   * ZBADARGUMENTS - invalid input parameters
   * ZINVALIDSTATE - state is ZOO_SESSION_EXPIRED_STATE or ZOO_AUTH_FAILED_STATE
   * ZMARSHALLINGERROR - failed to marshall a request; possibly, out of memory
   */
  int exists(const std::string& path, bool watch, Stat* stat);

  /**
   * \brief gets the data associated with a node synchronously.
   *
   * \param path the name of the node. Expressed as a file name with
   *    slashes separating ancestors of the node.
   * \param watch if nonzero, a watch will be set at the server to
   *    notify the client if the node changes.
   * \param result the data returned by the server
   * \param stat if not `nullptr`, will hold the value of stat for the path
   *    on return.
   * \return return value of the function call.
   * ZOK operation completed successfully
   * ZNONODE the node does not exist.
   * ZNOAUTH the client does not have permission.
   * ZBADARGUMENTS - invalid input parameters
   * ZINVALIDSTATE - state is ZOO_SESSION_EXPIRED_STATE or ZOO_AUTH_FAILED_STATE
   * ZMARSHALLINGERROR - failed to marshall a request; possibly, out of memory
   */
  int get(
      const std::string& path,
      bool watch,
      std::string* result,
      Stat* stat);

  /**
   * \brief lists the children of a node synchronously.
   *
   * \param path the name of the node. Expressed as a file name with
   *   slashes separating ancestors of the node.
   * \param watch if true, a watch will be set at the server to notify
   *   the client if the node changes.
   * \param results return value of children paths.
   * \return the return code of the function.
   * ZOK operation completed successfully
   * ZNONODE the node does not exist.
   * ZNOAUTH the client does not have permission.
   * ZBADARGUMENTS - invalid input parameters
   * ZINVALIDSTATE - state is ZOO_SESSION_EXPIRED_STATE or ZOO_AUTH_FAILED_STATE
   * ZMARSHALLINGERROR - failed to marshall a request; possibly, out of memory
   */
  int getChildren(
      const std::string& path,
      bool watch,
      std::vector<std::string>* results);

  /**
   * \brief sets the data associated with a node.
   *
   * \param path the name of the node. Expressed as a file name with slashes
   * separating ancestors of the node.
   * \param data the data to be written to the node.
   * \param version the expected version of the node. The function will fail if
   * the actual version of the node does not match the expected version. If -1 is
   * used the version check will not take place.
   * \return the return code for the function call.
   * ZOK operation completed successfully
   * ZNONODE the node does not exist.
   * ZNOAUTH the client does not have permission.
   * ZBADVERSION expected version does not match actual version.
   * ZBADARGUMENTS - invalid input parameters
   * ZINVALIDSTATE - zhandle state is either ZOO_SESSION_EXPIRED_STATE or ZOO_AUTH_FAILED_STATE
   * ZMARSHALLINGERROR - failed to marshall a request; possibly, out of memory
   */
  int set(const std::string& path, const std::string& data, int version);

  /**
   * \brief return a message describing the return code.
   *
   * \return string message corresponding to return code.
   */
  std::string message(int code) const;

  /**
   * \brief returns whether or not the specified return code implies
   * the operation can be retried "as is" (i.e., without needing to
   * change anything).
   *
   * \return bool indicating whether operation can be retried.
   */
  bool retryable(int code);


protected:
  /* Underlying implementation (pimpl idiom). */
  ZooKeeperProcess* process;

private:
  /* ZooKeeper instances are not copyable. */
  ZooKeeper(const ZooKeeper& that);
  ZooKeeper& operator=(const ZooKeeper& that);
};


#endif /* __MESOS_ZOOKEEPER_HPP__ */
