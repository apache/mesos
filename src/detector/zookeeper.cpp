#include <assert.h>

#include <iostream>
#include <map>

#include <process.hpp>

#include "zookeeper.hpp"

#include "common/fatal.hpp"

using std::map;
using std::string;
using std::vector;


/* Forward (and first) declaration of ZooKeeperProcess. */
class ZooKeeperProcess;


enum {
  /* Generic messages: */
  TERMINATE = PROCESS_MSGID, // Terminate process.

  /* WatcherProcessManager messages: */
  OK, // Generic success response.
  ERROR, // Generic failure response.
  REGISTER, // Register WatcherProcess.
  UNREGISTER, // Unregister WatcherProcess.
  LOOKUP_PROCESS, // Lookup WatcherProcess associated with Watcher.
  LOOKUP_PID, // Lookup WatcherProcess PID associated with Watcher.

  /* WatcherProcess messages: */
  EVENT, // Invoke Watcher::process callback.

  /* ZooKeeperProcess messages: */
  COMPLETED, // After an asynchronous "call" has completed.
  CREATE, // Perform an asynchronous create.
  REMOVE, // Perform an asynchronous remove (delete).
  EXISTS, // Perform an asysnchronous exists.
  GET, // Perform an asynchronous get.
  GET_CHILDREN, // Perform an asynchronous get_children.
  SET, // Perform an asynchronous set.
};


/* Event "message" for invoking Watcher. */
struct Event
{
  ZooKeeper *zk;
  int type;
  int state;
  string path;
};


/* Create "message" for performing ZooKeeper::create. */
struct CreateCall
{
  int ret;
  const string *path;
  const string *data;
  const ACL_vector *acl;
  int flags;
  string *result;
  PID from;
  ZooKeeperProcess *zooKeeperProcess;
};


/* Remove "message" for performing ZooKeeper::remove. */
struct RemoveCall
{
  int ret;
  const string *path;
  int version;
  PID from;
  ZooKeeperProcess *zooKeeperProcess;
};


/* Exists "message" for performing ZooKeeper::exists. */
struct ExistsCall
{
  int ret;
  const string *path;
  bool watch;
  Stat *stat;
  PID from;
  ZooKeeperProcess *zooKeeperProcess;
};


/* Get "message" for performing ZooKeeper::get. */
struct GetCall
{
  int ret;
  const string *path;
  bool watch;
  string *result;
  Stat *stat;
  PID from;
  ZooKeeperProcess *zooKeeperProcess;
};


/* GetChildren "message" for performing ZooKeeper::getChildren. */
struct GetChildrenCall
{
  int ret;
  const string *path;
  bool watch;
  vector<string> *results;
  PID from;
  ZooKeeperProcess *zooKeeperProcess;
};


/* Set "message" for performing ZooKeeper::set. */
struct SetCall
{
  int ret;
  const string *path;
  const string *data;
  int version;
  PID from;
  ZooKeeperProcess *zooKeeperProcess;
};


/* PID of singleton instance of WatcherProcessManager. */
PID manager;


/**
 * In order to make callbacks on Watcher, we create a proxy
 * WatcherProcess. The ZooKeeperProcess (defined below) stores the PID
 * of the WatcherProcess associated with a watcher and sends it an
 * event "message" which it uses to invoke Watcher::process. Care
 * needed to be taken to assure that a WatcherProcess was only valid
 * as long as a Watcher was valid. This was done by ensuring that the
 * WatcherProcess object created gets cleaned up in ~Watcher(). We
 * wanted to keep the Watcher interface clean and simple, so rather
 * than add a member in Watcher that points to a WatcherProcess
 * instance (or points to a WatcherImpl), we choose to create a
 * WatcherProcessManager that stores the Watcher and WatcherProcess
 * associations. The WatcherProcessManager is akin to having a shared
 * dictionary or hashtable and using locks to access it rathe then
 * sending and receiving messages. Their is probably a performance hit
 * here, but it would be interesting to see how bad the perforamnce is
 * across a range of low and high-contention states.
 */
class WatcherProcess : public Process
{
  friend class WatcherProcessManager;

private:
  Watcher *watcher;

protected:
  void operator () ()
  {
    WatcherProcess *process = this;
    send(manager, REGISTER,
         reinterpret_cast<char *>(&process), sizeof(process));
    if (receive() != OK)
      fatal("failed to setup underlying watcher mechanism");
    while (true) {
      switch (receive()) {
      case EVENT: {
	Event *event =
	  *reinterpret_cast<Event **>(const_cast<char *>(body(NULL)));
	watcher->process(event->zk, event->type, event->state, event->path);
	delete event;
	break;
      }
      case TERMINATE:
	send(manager, UNREGISTER,
             reinterpret_cast<char *>(&process), sizeof(process));
        if (receive() != OK)
	  fatal("failed to cleanup underlying watcher mechanism");
	return;
      }
    }
  }

public:
  WatcherProcess(Watcher *_watcher) : watcher(_watcher) {}
};


class WatcherProcessManager : public Process
{
private:
  map<Watcher *, WatcherProcess *> watchers;

protected:
  void operator () ()
  {
    while (true) {
      switch (receive()) {
        case REGISTER: {
	  WatcherProcess *process =
	    *reinterpret_cast<WatcherProcess **>(const_cast<char *>(body(NULL)));
	  Watcher *watcher = process->watcher;
	  assert(watchers.find(watcher) == watchers.end());
	  watchers[watcher] = process;
	  send(from(), OK);
	  break;
	}
        case UNREGISTER: {
	  WatcherProcess *process =
	    *reinterpret_cast<WatcherProcess **>(const_cast<char *>(body(NULL)));
	  Watcher *watcher = process->watcher;
	  assert(watchers.find(watcher) != watchers.end());
	  watchers.erase(watcher);
	  send(from(), OK);
	  break;
	}
        case LOOKUP_PROCESS: {
	  Watcher *watcher =
	    *reinterpret_cast<Watcher **>(const_cast<char *>(body(NULL)));
	  if (watchers.find(watcher) != watchers.end()) {
	    WatcherProcess *process = watchers[watcher];
	    send(from(), OK, reinterpret_cast<char *>(&process), sizeof(process));
	  } else {
	    send(from(), ERROR);
	  }
	  break;
	}
        case LOOKUP_PID: {
	  Watcher *watcher =
	    *reinterpret_cast<Watcher **>(const_cast<char *>(body(NULL)));
	  if (watchers.find(watcher) != watchers.end()) {
	    WatcherProcess *process = watchers[watcher];
	    const PID &pid = process->self();
	    send(from(), OK, reinterpret_cast<const char *>(&pid), sizeof(pid));
	  } else {
	    send(from(), ERROR);
	  }
	  break;
	}
      }
    }
  }
};


Watcher::Watcher()
{
  // Confirm we have allocated the WatcherProcessManager.
  static volatile bool initialized = false;
  static volatile bool initializing = true;

  // Confirm everything is initialized.
  if (!initialized) {
    if (__sync_bool_compare_and_swap(&initialized, false, true)) {
	manager = Process::spawn(new WatcherProcessManager());
	initializing = false;
      }
    }

  while (initializing);

  Process::spawn(new WatcherProcess(this));
}


Watcher::~Watcher()
{
  class WatcherProcessWaiter : public Process
  {
  private:
    Watcher *watcher;

  protected:
    void operator () ()
    {
      send(manager, LOOKUP_PROCESS,
           reinterpret_cast<char *>(&watcher), sizeof(watcher));
      if (receive() != OK)
	fatal("failed to deallocate resources associated with Watcher");
      WatcherProcess *process =
	*reinterpret_cast<WatcherProcess **>(const_cast<char *>(body(NULL)));
      send(process->self(), TERMINATE);
      wait(process->self());
      delete process;
    }

  public:
    WatcherProcessWaiter(Watcher *_watcher) : watcher(_watcher) {}
  } watcherProcessWaiter(this);

  Process::wait(Process::spawn(&watcherProcessWaiter));
}


class ZooKeeperImpl : public Process {};


class ZooKeeperProcess : public ZooKeeperImpl
{
  friend class ZooKeeper;

private:
  ZooKeeper *zk; // ZooKeeper instance.
  string hosts; // ZooKeeper host:port pairs.
  int timeout; // ZooKeeper session timeout.
  zhandle_t *zh; // ZooKeeper connection handle.
  
  Watcher *watcher; // Associated Watcher instance. 
  PID watcherProcess; // PID of WatcherProcess that invokes Watcher.

  static void watch(zhandle_t *zh, int type, int state,
		    const char *path, void *context)
  {
    ZooKeeperProcess *zooKeeperProcess =
      static_cast<ZooKeeperProcess*>(context);
    Event *event = new Event();
    event->zk = zooKeeperProcess->zk;
    event->type = type;
    event->state = state;
    event->path = path;
    zooKeeperProcess->send(zooKeeperProcess->watcherProcess,
			   EVENT,
			   reinterpret_cast<char *>(&event),
			   sizeof(Event *));
  }
  
  static void createCompletion(int ret, const char *value, const void *data)
  {
    CreateCall *createCall =
      static_cast<CreateCall *>(const_cast<void *>((data)));
    createCall->ret = ret;
    if (createCall->result != NULL && value != NULL)
      createCall->result->assign(value);
    createCall->zooKeeperProcess->send(createCall->from, COMPLETED);
  }

  static void removeCompletion(int ret, const void *data)
  {
    RemoveCall *removeCall =
      static_cast<RemoveCall *>(const_cast<void *>((data)));
    removeCall->ret = ret;
    removeCall->zooKeeperProcess->send(removeCall->from, COMPLETED);
  }

  static void existsCompletion(int ret, const Stat *stat, const void *data)
  {
    ExistsCall *existsCall =
      static_cast<ExistsCall *>(const_cast<void *>((data)));
    existsCall->ret = ret;
    if (existsCall->stat != NULL && stat != NULL)
      *(existsCall->stat) = *(stat);      
    existsCall->zooKeeperProcess->send(existsCall->from, COMPLETED);
  }

  static void getCompletion(int ret, const char *value, int value_len,
			    const Stat *stat, const void *data)
  {
    GetCall *getCall =
      static_cast<GetCall *>(const_cast<void *>((data)));
    getCall->ret = ret;
    if (getCall->result != NULL && value != NULL && value_len > 0)
      getCall->result->assign(value, value_len);
    if (getCall->stat != NULL && stat != NULL)
      *(getCall->stat) = *(stat);
    getCall->zooKeeperProcess->send(getCall->from, COMPLETED);
  }

  static void getChildrenCompletion(int ret, const String_vector *results,
				    const void *data)
  {
    GetChildrenCall *getChildrenCall =
      static_cast<GetChildrenCall *>(const_cast<void *>((data)));
    getChildrenCall->ret = ret;
    if (getChildrenCall->results != NULL && results != NULL) {
      for (int i = 0; i < results->count; i++) {
	getChildrenCall->results->push_back(results->data[i]);
      }
    }
    getChildrenCall->zooKeeperProcess->send(getChildrenCall->from, COMPLETED);
  }

  static void setCompletion(int ret, const Stat *stat, const void *data)
  {
    SetCall *setCall =
      static_cast<SetCall *>(const_cast<void *>((data)));
    setCall->ret = ret;
    setCall->zooKeeperProcess->send(setCall->from, COMPLETED);
  }

  bool prepare(int *fd, int *ops, timeval *tv)
  {
    int interest = 0;

    int ret = zookeeper_interest(zh, fd, &interest, tv);

    // If in some disconnected state, try again later.
    if (ret == ZINVALIDSTATE ||
	ret == ZCONNECTIONLOSS ||
	ret == ZOPERATIONTIMEOUT)
      return false;

    if (ret != ZOK)
      fatal("zookeeper_interest failed! (%s)", zerror(ret));

    *ops = 0;

    if ((interest & ZOOKEEPER_READ) && (interest & ZOOKEEPER_WRITE)) {
      *ops |= RDWR;
    } else if (interest & ZOOKEEPER_READ) {
      *ops |= RDONLY;
    } else if (interest & ZOOKEEPER_WRITE) {
      *ops |= WRONLY;
    }

    return true;
  }

  void process(int fd, int ops)
  {
    int events = 0;

    if (ready(fd, RDONLY)) {
      events |= ZOOKEEPER_READ;
    } if (ready(fd, WRONLY)) {
      events |= ZOOKEEPER_WRITE;
    }

    int ret = zookeeper_process(zh, events);

    // If in some disconnected state, try again later.
    if (ret == ZINVALIDSTATE ||
	ret == ZCONNECTIONLOSS)
      return;

    if (ret != ZOK && ret != ZNOTHING)
      fatal("zookeeper_process failed! (%s)", zerror(ret));
  }

protected:
  void operator () ()
  {
    // Lookup and cache the WatcherProcess PID associated with our
    // Watcher _before_ we yield control via calling zookeeper_process
    // so that Watcher callbacks can occur.
    send(manager, LOOKUP_PID,
         reinterpret_cast<char *>(&watcher), sizeof(watcher));
    if (receive() != OK)
      fatal("failed to setup underlying ZooKeeper mechanisms");

    // TODO(benh): Link with WatcherProcess?
    watcherProcess =
      *reinterpret_cast<PID *>(const_cast<char *>(body(NULL)));

    while (true) {
      int fd;
      int ops;
      timeval tv;

      prepare(&fd, &ops, &tv);

      double secs = tv.tv_sec + (tv.tv_usec * 1e-6);

      // Cause await to return immediately if the file descriptor is
      // not valid (for example because the connection timed out) and
      // secs is 0 because that will block indefinitely.
      if (fd == -1 && secs == 0)
	secs = -1;

      if (await(fd, ops, secs, false)) {
	// Either timer expired (might be 0) or data became available on fd.
	process(fd, ops);
      } else {
	// TODO(benh): Don't handle incoming "calls" until we are connected!
	switch (receive()) {
	  case CREATE: {
	    CreateCall *createCall =
	      *reinterpret_cast<CreateCall **>(const_cast<char *>(body(NULL)));
	    createCall->from = from();
	    createCall->zooKeeperProcess = this;
	    int ret = zoo_acreate(zh, createCall->path->c_str(),
				  createCall->data->data(),
				  createCall->data->size(),
				  createCall->acl, createCall->flags,
				  createCompletion, createCall);
	    if (ret != ZOK) {
	      createCall->ret = ret;
	      send(createCall->from, COMPLETED);
	    }
	    break;
	  }
	  case REMOVE: {
	    RemoveCall *removeCall =
	      *reinterpret_cast<RemoveCall **>(const_cast<char *>(body(NULL)));
	    removeCall->from = from();
	    removeCall->zooKeeperProcess = this;
	    int ret = zoo_adelete(zh, removeCall->path->c_str(),
				  removeCall->version,
				  removeCompletion, removeCall);
	    if (ret != ZOK) {
	      removeCall->ret = ret;
	      send(removeCall->from, COMPLETED);
	    }
	    break;
	  }
	  case EXISTS: {
	    ExistsCall *existsCall =
	      *reinterpret_cast<ExistsCall **>(const_cast<char *>(body(NULL)));
	    existsCall->from = from();
	    existsCall->zooKeeperProcess = this;
	    int ret = zoo_aexists(zh, existsCall->path->c_str(),
				  existsCall->watch,
				  existsCompletion, existsCall);
	    if (ret != ZOK) {
	      existsCall->ret = ret;
	      send(existsCall->from, COMPLETED);
	    }
	    break;
	  }
	  case GET: {
	    GetCall *getCall =
	      *reinterpret_cast<GetCall **>(const_cast<char *>(body(NULL)));
	    getCall->from = from();
	    getCall->zooKeeperProcess = this;
	    int ret = zoo_aget(zh, getCall->path->c_str(), getCall->watch,
			       getCompletion, getCall);
	    if (ret != ZOK) {
	      getCall->ret = ret;
	      send(getCall->from, COMPLETED);
	    }
	    break;
	  }
	  case GET_CHILDREN: {
	    GetChildrenCall *getChildrenCall =
	      *reinterpret_cast<GetChildrenCall **>(const_cast<char *>(body(NULL)));
	    getChildrenCall->from = from();
	    getChildrenCall->zooKeeperProcess = this;
	    int ret = zoo_aget_children(zh, getChildrenCall->path->c_str(),
					getChildrenCall->watch,
					getChildrenCompletion,
					getChildrenCall);
	    if (ret != ZOK) {
	      getChildrenCall->ret = ret;
	      send(getChildrenCall->from, COMPLETED);
	    }
	    break;
	  }
	  case SET: {
	    SetCall *setCall =
	      *reinterpret_cast<SetCall **>(const_cast<char *>(body(NULL)));
	    setCall->from = from();
	    setCall->zooKeeperProcess = this;
	    int ret = zoo_aset(zh, setCall->path->c_str(),
			       setCall->data->data(),
			       setCall->data->size(),
			       setCall->version,
			       setCompletion, setCall);
	    if (ret != ZOK) {
	      setCall->ret = ret;
	      send(setCall->from, COMPLETED);
	    }
	    break;
	  }
	  case TERMINATE: {
	    return;
	  }
	  default: {
	    fatal("unexpected interruption during await");
	  }
	}
      }
    }
  }

public:
  ZooKeeperProcess(ZooKeeper *_zk,
		   const string &_hosts,
		   int _timeout,
		   Watcher *_watcher)
    : zk(_zk), hosts(_hosts), timeout(_timeout), watcher(_watcher)
  {
    zh = zookeeper_init(hosts.c_str(), watch, timeout, NULL, this, 0);
    if (zh == NULL)
      fatalerror("failed to create ZooKeeper (zookeeper_init)");
  }

  ~ZooKeeperProcess()
  {
    int ret = zookeeper_close(zh);
    if (ret != ZOK)
      fatal("failed to destroy ZooKeeper (zookeeper_close): %s", zerror(ret));
  }
};



ZooKeeper::ZooKeeper(const string &hosts, int timeout, Watcher *watcher)
{
  impl = new ZooKeeperProcess(this, hosts, timeout, watcher);
  Process::spawn(impl);
}


ZooKeeper::~ZooKeeper()
{
  Process::post(impl->self(), TERMINATE);
  Process::wait(impl->self());
  delete impl;
}


int ZooKeeper::getState()
{
  ZooKeeperProcess *zooKeeperProcess = static_cast<ZooKeeperProcess *>(impl);
  return zoo_state(zooKeeperProcess->zh);
}


int ZooKeeper::create(const string &path,
		      const string &data,
		      const ACL_vector &acl,
		      int flags,
		      string *result)
{
  CreateCall createCall;
  createCall.path = &path;
  createCall.data = &data;
  createCall.acl = &acl;
  createCall.flags = flags;
  createCall.result = result;

  class CreateCallProcess : public Process
  {
  private:
    ZooKeeperProcess *zooKeeperProcess;
    CreateCall *createCall;

  protected:
    void operator () ()
    {
      send(zooKeeperProcess->self(),
           CREATE,
           reinterpret_cast<char *>(&createCall),
           sizeof(CreateCall *));
      if (receive() != COMPLETED)
	createCall->ret = ZSYSTEMERROR;
    }

  public:
    CreateCallProcess(ZooKeeperProcess *_zooKeeperProcess,
		      CreateCall *_createCall)
      : zooKeeperProcess(_zooKeeperProcess),
	createCall(_createCall)
    {}
  } createCallProcess(static_cast<ZooKeeperProcess *>(impl),
		      &createCall);

  Process::wait(Process::spawn(&createCallProcess));

  return createCall.ret;
}



int ZooKeeper::remove(const string &path, int version)
{
  RemoveCall removeCall;
  removeCall.path = &path;
  removeCall.version = version;

  class RemoveCallProcess : public Process
  {
  private:
    ZooKeeperProcess *zooKeeperProcess;
    RemoveCall *removeCall;

  protected:
    void operator () ()
    {
      send(zooKeeperProcess->self(),
           REMOVE,
           reinterpret_cast<char *>(&removeCall),
           sizeof(RemoveCall *));
      if (receive() != COMPLETED)
	removeCall->ret = ZSYSTEMERROR;
    }

  public:
    RemoveCallProcess(ZooKeeperProcess *_zooKeeperProcess,
		      RemoveCall *_removeCall)
      : zooKeeperProcess(_zooKeeperProcess),
	removeCall(_removeCall)
    {}
  } removeCallProcess(static_cast<ZooKeeperProcess *>(impl),
		      &removeCall);

  Process::wait(Process::spawn(&removeCallProcess));

  return removeCall.ret;
}


int ZooKeeper::exists(const string &path,
		      bool watch,
		      Stat *stat)
{
  ExistsCall existsCall;
  existsCall.path = &path;
  existsCall.watch = watch;
  existsCall.stat = stat;

  class ExistsCallProcess : public Process
  {
  private:
    ZooKeeperProcess *zooKeeperProcess;
    ExistsCall *existsCall;

  protected:
    void operator () ()
    {
      send(zooKeeperProcess->self(),
           EXISTS,
           reinterpret_cast<char *>(&existsCall),
           sizeof(ExistsCall *));
      if (receive() != COMPLETED)
	existsCall->ret = ZSYSTEMERROR;
    }

  public:
    ExistsCallProcess(ZooKeeperProcess *_zooKeeperProcess,
		      ExistsCall *_existsCall)
      : zooKeeperProcess(_zooKeeperProcess),
	existsCall(_existsCall)
    {}
  } existsCallProcess(static_cast<ZooKeeperProcess *>(impl),
		      &existsCall);

  Process::wait(Process::spawn(&existsCallProcess));

  return existsCall.ret;
}


int ZooKeeper::get(const string &path,
		   bool watch,
		   string *result,
		   Stat *stat)
{
  GetCall getCall;
  getCall.path = &path;
  getCall.watch = watch;
  getCall.result = result;
  getCall.stat = stat;

  class GetCallProcess : public Process
  {
  private:
    ZooKeeperProcess *zooKeeperProcess;
    GetCall *getCall;

  protected:
    void operator () ()
    {
      send(zooKeeperProcess->self(),
           GET,
           reinterpret_cast<char *>(&getCall),
           sizeof(GetCall *));
      if (receive() != COMPLETED)
	getCall->ret = ZSYSTEMERROR;
    }

  public:
    GetCallProcess(ZooKeeperProcess *_zooKeeperProcess,
		   GetCall *_getCall)
      : zooKeeperProcess(_zooKeeperProcess),
	getCall(_getCall)
    {}
  } getCallProcess(static_cast<ZooKeeperProcess *>(impl),
		   &getCall);

  Process::wait(Process::spawn(&getCallProcess));

  return getCall.ret;
}


int ZooKeeper::getChildren(const string &path,
			   bool watch,
			   vector<string> *results)
{
  GetChildrenCall getChildrenCall;
  getChildrenCall.path = &path;
  getChildrenCall.watch = watch;
  getChildrenCall.results = results;

  class GetChildrenCallProcess : public Process
  {
  private:
    ZooKeeperProcess *zooKeeperProcess;
    GetChildrenCall *getChildrenCall;

  protected:
    void operator () ()
    {
      send(zooKeeperProcess->self(),
           GET_CHILDREN,
           reinterpret_cast<char *>(&getChildrenCall),
           sizeof(GetChildrenCall *));
      if (receive() != COMPLETED)
	getChildrenCall->ret = ZSYSTEMERROR;
    }

  public:
    GetChildrenCallProcess(ZooKeeperProcess *_zooKeeperProcess,
			   GetChildrenCall *_getChidlrenCall)
      : zooKeeperProcess(_zooKeeperProcess),
	getChildrenCall(_getChidlrenCall)
    {}
  } getChildrenCallProcess(static_cast<ZooKeeperProcess *>(impl),
		   &getChildrenCall);

  Process::wait(Process::spawn(&getChildrenCallProcess));

  return getChildrenCall.ret;
}


int ZooKeeper::set(const string &path,
		   const string &data,
		   int version)
{
  SetCall setCall;
  setCall.path = &path;
  setCall.data = &data;
  setCall.version = version;

  class SetCallProcess : public Process
  {
  private:
    ZooKeeperProcess *zooKeeperProcess;
    SetCall *setCall;

  protected:
    void operator () ()
    {
      send(zooKeeperProcess->self(),
           SET,
           reinterpret_cast<char *>(&setCall),
           sizeof(SetCall *));
      if (receive() != COMPLETED)
	setCall->ret = ZSYSTEMERROR;
    }

  public:
    SetCallProcess(ZooKeeperProcess *_zooKeeperProcess,
		   SetCall *_setCall)
      : zooKeeperProcess(_zooKeeperProcess),
	setCall(_setCall)
    {}
  } setCallProcess(static_cast<ZooKeeperProcess *>(impl),
		   &setCall);

  Process::wait(Process::spawn(&setCallProcess));

  return setCall.ret;
}


const char * ZooKeeper::error(int ret) const
{
  return zerror(ret);
}


// class TestWatcher : public Watcher
// {
// public:
//   void process(ZooKeeper *zk, int type, int state, const string &path)
//   {
//     std::cout << "TestWatcher::process" << std::endl;
//   }
// };


// int main(int argc, char** argv)
// {
//   TestWatcher watcher;
//   ZooKeeper zk(argv[1], 10000, &watcher);
//   sleep(10);
//   return 0;
// }
