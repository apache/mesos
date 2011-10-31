#ifndef __ZOOKEEPER_WATCHER_HPP__
#define __ZOOKEEPER_WATCHER_HPP__

#include <glog/logging.h>

#include <process/dispatch.hpp>

#include "zookeeper/zookeeper.hpp"


// A watcher which dispatches events to a process.
template <typename T>
class ProcessWatcher : public Watcher
{
public:
  ProcessWatcher(const process::PID<T>& _pid)
    : pid(_pid), reconnect(false) {}

  virtual void process(ZooKeeper* zk,
                       int type,
                       int state,
                       const std::string& path)
  {
    if (type == ZOO_SESSION_EVENT) {
      if (state == ZOO_CONNECTED_STATE) {
        // Connected (initial or reconnect).
        process::dispatch(pid, &T::connected, reconnect);
      } else if (state == ZOO_CONNECTING_STATE) {
        // The client library automatically reconnects, taking
        // into account failed servers in the connection string,
        // appropriately handling the "herd effect", etc.
        reconnect = true;
        process::dispatch(pid, &T::reconnecting);
      } else if (state == ZOO_EXPIRED_SESSION_STATE) {
        // If this watcher gets reused then the next connected
        // event shouldn't be perceived as a reconnect.
        reconnect = false;
        process::dispatch(pid, &T::expired);
      } else {
        LOG(FATAL) << "Unhandled ZooKeeper state (" << state << ")"
                   << " for ZOO_SESSION_EVENT";
      }
    } else if (type == ZOO_CHILD_EVENT) {
      process::dispatch(pid, &T::updated, path);
    } else if (type == ZOO_CHANGED_EVENT) {
      process::dispatch(pid, &T::updated, path);
    } else if (type == ZOO_CREATED_EVENT) {
      process::dispatch(pid, &T::created, path);
    } else if (type == ZOO_DELETED_EVENT) {
      process::dispatch(pid, &T::deleted, path);
    } else {
      LOG(FATAL) << "Unhandled ZooKeeper event (" << type << ")"
                 << " in state (" << state << ")";
    }
  }

private:
  const process::PID<T> pid;
  bool reconnect;
};

#endif // __ZOOKEEPER_WATCHER_HPP__
