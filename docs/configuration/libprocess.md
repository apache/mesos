---
title: Apache Mesos - Libprocess Options
layout: documentation
---

# Libprocess Options

*The bundled libprocess library can be controlled with the following environment variables.*

<table class="table table-striped">
  <thead>
    <tr>
      <th width="30%">
        Variable
      </th>
      <th>
        Explanation
      </th>
    </tr>
  </thead>
  <tr>
    <td>
      LIBPROCESS_IP
    </td>
    <td>
      Sets the IP address for communication to and from libprocess.
    </td>
  </tr>
  <tr>
    <td>
      LIBPROCESS_PORT
    </td>
    <td>
      Sets the port for communication to and from libprocess.
    </td>
  </tr>
  <tr>
    <td>
      LIBPROCESS_ADVERTISE_IP
    </td>
    <td>
      If set, this provides the IP address that will be advertised to
      the outside world for communication to and from libprocess.
      This is useful, for example, for containerized tasks in which
      communication is bound locally to a non-public IP that will be
      inaccessible to the master.
    </td>
  </tr>
  <tr>
    <td>
      LIBPROCESS_ADVERTISE_PORT
    </td>
    <td>
      If set, this provides the port that will be advertised to the
      outside world for communication to and from libprocess. Note that
      this port will not actually be bound (the local LIBPROCESS_PORT
      will be), so redirection to the local IP and port must be
      provided separately.
    </td>
  </tr>
  <tr>
    <td>
      LIBPROCESS_REQUIRE_PEER_ADDRESS_IP_MATCH
    </td>
    <td>
      If set, the IP address portion of the libprocess UPID in
      incoming messages is required to match the IP address
      of the socket from which the message was sent. This can be a
      security enhancement since it prevents unauthorized senders
      impersonating other libprocess actors. This check may
      break configurations that require setting LIBPROCESS_IP,
      or LIBPROCESS_ADVERTISE_IP. Additionally, multi-homed
      configurations may be affected since the address on
      which libprocess is listening may not match the address from
      which libprocess connects to other actors.
    </td>
  </tr>
  <tr>
    <td>
      LIBPROCESS_ENABLE_PROFILER
    </td>
    <td>
      To enable the profiler, this variable must be set to 1. Note that this
      variable will only work if Mesos has been configured with
      <code>--enable-perftools</code>.
    </td>
  </tr>
  <tr>
    <td>
      LIBPROCESS_METRICS_SNAPSHOT_ENDPOINT_RATE_LIMIT
    </td>
    <td>
      If set, this variable can be used to configure the rate limit
      applied to the /metrics/snapshot endpoint. The format is
      `<number of requests>/<interval duration>`.
      Examples: `10/1secs`, `100/10secs`, etc.
    </td>
  </tr>
  <tr>
    <td>
      LIBPROCESS_NUM_WORKER_THREADS
    </td>
    <td>
      If set to an integer value in the range 1 to 1024, it overrides
      the default setting of the number of libprocess worker threads,
      which is the maximum of 8 and the number of cores on the machine.
    </td>
  </tr>
</table>
