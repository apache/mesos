// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License

#ifndef __PROCESS_PID_HPP__
#define __PROCESS_PID_HPP__

#include <stdint.h>

#include <iosfwd>
#include <string>

#include <boost/functional/hash.hpp>

#include <process/address.hpp>

#include <stout/ip.hpp>

namespace process {

// Forward declaration to break cyclic dependencies.
class ProcessBase;

/**
 * An "untyped" `PID`, used to encapsulate the process ID for
 * lower-layer abstractions (eg, when receiving incoming requests)
 * in the dispatching mechanism.
 *
 * @see process::PID
 */
struct UPID
{
  UPID() = default;

  UPID(const UPID& that) = default;

  UPID(UPID&& that) = default;

  UPID(const char* id_, const net::IP& ip_, uint16_t port_)
    : id(id_), address(ip_, port_) { resolve(); }

  UPID(const char* id_, const network::inet::Address& address_)
    : id(id_), address(address_) { resolve(); }

  UPID(const std::string& id_, const net::IP& ip_, uint16_t port_)
    : id(id_), address(ip_, port_) { resolve(); }

  UPID(const std::string& id_, const network::inet::Address& address_)
    : id(id_), address(address_) { resolve(); }

  /*implicit*/ UPID(const char* s);

  /*implicit*/ UPID(const std::string& s);

  /*implicit*/ UPID(const ProcessBase& process);

  UPID& operator=(const UPID& that) = default;

  UPID& operator=(UPID&& that) = default;

  operator std::string() const;

  operator bool() const
  {
    return id != "" && !address.ip.isAny() && address.port != 0;
  }

  bool operator!() const // NOLINT(whitespace/operators)
  {
    return id == "" && address.ip.isAny() && address.port == 0;
  }

  bool operator<(const UPID& that) const
  {
    if (address == that.address) {
      return id < that.id;
    } else {
      return address < that.address;
    }
  }

  bool operator==(const UPID& that) const
  {
    return (id == that.id && address == that.address);
  }

  bool operator!=(const UPID& that) const
  {
    return !(*this == that);
  }

  // Attempts to resolve and cache a weak pointer to the ProcessBase
  // to which this UPID refers.
  void resolve();

  std::string id;

  // TODO(asridharan): Ideally, the following `address` field should be of
  // type `network::Address` so that the default address of the PID
  // could be a unix domain socket or an IPv4/v6 address. This change
  // however is disruptive at this point and should be done after we have
  // introduced support for unix domain and IPv6 sockets into
  // `libprocess`.
  network::inet::Address address = network::inet4::Address::ANY_ANY();

  // TODO(asridharan): Currently we are introducing only an `Optional`
  // IPv6 address in the following `addresses` structure. This will
  // help us initiate some basic IPv6 support for the
  // `DockerContainerizer`.  However, going forward, once we start
  // supporting unix domain sockets and IPv4/IPv6 socket in
  // `libprocess` we will add the following fields to this structure.
  //
  // Option<network::unix::Address> unix;
  // Option<network::inet4::Address> v4;
  //
  // With the introduction of the above fields `libprocess` PID will
  // be able to support unix, IPv4 and IPv6 sockets simultaneously.
  struct
  {
    Option<network::inet6::Address> v6;
  } addresses = {None()};

protected:
  friend class ProcessBase;
  friend class ProcessManager;

  // A weak pointer to the actual process used to optimize enqueuing
  // events without having to go through a shared lock in the
  // `ProcessManager`. This is `None` if someone creates a UPID and
  // doesn't call `resolve()` or if `resolve()` doesn't find a valid
  // process (i.e., the process hasn't started or has terminated).
  Option<std::weak_ptr<ProcessBase*>> reference = None();
};


/**
 * A "process identifier" used to uniquely identify a process when
 * dispatching messages.
 *
 * Typed with the actual process class's type, which must be
 * derived from `process::ProcessBase`.
 *
 * Use it like this:
 *
 *    using namespace process;
 *
 *    class SimpleProcess : public Process<SimpleProcess>
 *    {
 *       // ...
 *    };
 *
 *
 *    SimpleProcess process;
 *    PID<SimpleProcess> pid = spawn(process);
 *
 *    // ...
 *
 *    dispatchpid, &SimpleProcess::method, "argument");
 *
 * @see process::ProcessBase
 */
template <typename T = ProcessBase>
struct PID : UPID
{
  // Need to declare PID<U> as a friend in order to write `reference`.
  template <typename U>
  friend class PID;

  PID() : UPID() {}

  /*implicit*/ PID(const T* t) : UPID(static_cast<const ProcessBase&>(*t)) {}
  /*implicit*/ PID(const T& t) : UPID(static_cast<const ProcessBase&>(t)) {}

  template <typename Base>
  operator PID<Base>() const
  {
    // Only allow upcasts!
    T* t = nullptr;
    Base* base = t;
    (void)base; // Eliminate unused base warning.
    PID<Base> pid;
    pid.id = id;
    pid.address = address;
    pid.addresses = addresses;
    pid.reference = reference;
    return pid;
  }
};


// Outputing UPIDs and generating UPIDs using streams.
std::ostream& operator<<(std::ostream&, const UPID&);
std::istream& operator>>(std::istream&, UPID&);

} // namespace process {

namespace std {

template <>
struct hash<process::UPID>
{
  typedef size_t result_type;

  typedef process::UPID argument_type;

  result_type operator()(const argument_type& upid) const
  {
    size_t seed = 0;
    boost::hash_combine(seed, upid.id);
    boost::hash_combine(seed, std::hash<net::IP>()(upid.address.ip));
    boost::hash_combine(seed, upid.address.port);
    return seed;
  }
};

} // namespace std {

#endif // __PROCESS_PID_HPP__
