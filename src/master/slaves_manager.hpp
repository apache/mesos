#ifndef __SLAVES_MANAGER_HPP__
#define __SLAVES_MANAGER_HPP__

#include <process.hpp>

#include "configurator/configurator.hpp"

#include "common/multimap.hpp"


namespace mesos { namespace internal { namespace master {

class Master;


class SlavesManagerStorage : public process::Process<SlavesManagerStorage>
{
public:
  virtual process::Promise<bool> add(const std::string& hostname, uint16_t port) { return true; }
  virtual process::Promise<bool> remove(const std::string& hostname, uint16_t port) { return true; }
  virtual process::Promise<bool> activate(const std::string& hostname, uint16_t port) { return true; }
  virtual process::Promise<bool> deactivate(const std::string& hostname, uint16_t port) { return true; }
};


class SlavesManager : public process::Process<SlavesManager>
{
public:
  SlavesManager(const Configuration& conf, const process::PID<Master>& _master);

  virtual ~SlavesManager();

  static void registerOptions(Configurator* configurator);

  bool add(const std::string& hostname, uint16_t port);
  bool remove(const std::string& hostname, uint16_t port);
  bool activate(const std::string& hostname, uint16_t port);
  bool deactivate(const std::string& hostname, uint16_t port);

  void updateActive(const multimap<std::string, uint16_t>& updated);
  void updateInactive(const multimap<std::string, uint16_t>& updated);

private:
  process::Promise<process::HttpResponse> add(const process::HttpRequest& request);
  process::Promise<process::HttpResponse> remove(const process::HttpRequest& request);
  process::Promise<process::HttpResponse> activate(const process::HttpRequest& request);
  process::Promise<process::HttpResponse> deactivate(const process::HttpRequest& request);
  process::Promise<process::HttpResponse> activated(const process::HttpRequest& request);
  process::Promise<process::HttpResponse> deactivated(const process::HttpRequest& request);

  const process::PID<Master> master;

  multimap<std::string, uint16_t> active;
  multimap<std::string, uint16_t> inactive;

  SlavesManagerStorage* storage;
};

}}} // namespace mesos { namespace internal { namespace master {

#endif // __SLAVES_MANAGER_HPP__
