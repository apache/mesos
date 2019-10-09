---
title: Apache Mesos - SSL in Mesos
layout: documentation
---

# SSL in Mesos

By default, all the messages that flow through the Mesos cluster are
unencrypted, making it possible for anyone with access to the cluster to
intercept and potentially control arbitrary tasks.

SSL/TLS support was added to libprocess in Mesos 0.23.0, which encrypts the
data that Mesos uses for network communication between Mesos components.
Additionally, HTTPS support was added to the Mesos WebUI.

# Build Configuration

There are currently two implementations of the
[libprocess socket interface](https://github.com/apache/mesos/blob/master/3rdparty/libprocess/include/process/socket.hpp)
that support SSL.

The first implementation, added in Mesos 0.23.0, uses
[libevent](https://github.com/libevent/libevent).
Specifically it relies on the `libevent-openssl` library that wraps `openssl`.

The second implementation, added in Mesos 1.10.0, is a generic socket
wrapper which only relies on the OpenSSL (1.1+) library.

Before building Mesos from source, assuming you have installed the
required [Dependencies](#Dependencies), you can modify your configure line
to enable SSL as follows:

~~~
../configure --enable-ssl
# Or:
../configure --enable-libevent --enable-ssl
~~~


# Runtime Configuration

TLS support in Mesos can be configured with different levels of security. This section aims to help
Mesos operators to better understand the trade-offs involved in them.

On a high level, one can imagine to choose between three available layers of security, each
providing additional security guarantees but also increasing the deployment complexity.

1) `LIBPROCESS_SSL_ENABLED=true`. This provides external clients (e.g. curl) with the ability to
   connect to Mesos HTTP endpoints securely via TLS, verifying that the server certificate is valid
   and trusted.

2) `LIBPROCESS_SSL_VERIFY_SERVER_CERT=true`. In addition to the above, this ensures that Mesos components
   themselves are verifying the presence of valid and trusted server certificates when making
   outgoing connections. This prevents man-in-the-middle attacks on communications between Mesos
   components, and on communications between a Mesos component and an external server.

   **WARNING:** This setting only makes sense if `LIBPROCESS_SSL_ENABLE_DOWNGRADE` is set
   to `false`, otherwise a malicious actor can simply bypass certificate verification by
   downgrading to a non-TLS connection.

3) `LIBPROCESS_SSL_REQUIRE_CLIENT_CERT=true`. In addition to the above, this enforces the use of TLS
   client certificates on all connections to any Mesos component. This ensures that only trusted
   clients can connect to any Mesos component, preventing reception of forged or malformed messages.

   This implies that all schedulers or other clients (including the web browsers used by human
   operators) that are supposed to connect to any endpoint of a Mesos component must be provided
   with valid client certificates.

   **WARNING:** As above, this setting only makes sense if `LIBPROCESS_SSL_ENABLE_DOWNGRADE` is
   set to `false`.


For secure usage, it is recommended to set `LIBPROCESS_SSL_ENABLED=true`,
`LIBPROCESS_SSL_VERIFY_SERVER_CERT=true`, `LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME=openssl`
and `LIBPROCESS_SSL_ENABLE_DOWNGRADE=false`. This provides a good trade-off
between security and usability.

It is not recommended in general to expose Mesos components to the public internet, but in cases
where they are the use of `LIBPROCESS_SSL_REQUIRE_CLIENT_CERT` is strongly suggested.


# Environment Variables
Once you have successfully built and installed your new binaries, here are the environment variables that are applicable to the `Master`, `Agent`, `Framework Scheduler/Executor`, or any `libprocess process`:

**NOTE:** Prior to 1.0, the SSL related environment variables used to be prefixed by `SSL_`. However, we found that they may collide with other programs and lead to unexpected results (e.g., openssl, see [MESOS-5863](https://issues.apache.org/jira/browse/MESOS-5863) for details). To be backward compatible, we accept environment variables prefixed by both `SSL_` or `LIBPROCESS_SSL_`. New users should use the `LIBPROCESS_SSL_` version.

#### LIBPROCESS_SSL_ENABLED=(false|0,true|1) [default=false|0]
Turn on or off SSL. When it is turned off it is the equivalent of default Mesos with libevent as the backing for events. All sockets default to the non-SSL implementation. When it is turned on, the default configuration for sockets is SSL. This means outgoing connections will use SSL, and incoming connections will be expected to speak SSL as well. None of the below flags are relevant if SSL is not enabled.  If SSL is enabled, `LIBPROCESS_SSL_CERT_FILE` and `LIBPROCESS_SSL_KEY_FILE` must be supplied.

#### LIBPROCESS_SSL_SUPPORT_DOWNGRADE=(false|0,true|1) [default=false|0]
Control whether or not non-SSL connections can be established. If this is enabled
__on the accepting side__, then the accepting side will downgrade to a non-SSL socket if the
connecting side is attempting to communicate via non-SSL. (e.g. HTTP).

If this is enabled __on the connecting side__, then the connecting side will retry on a non-SSL
socket if establishing the SSL connection failed.

See [Upgrading Your Cluster](#Upgrading) for more details.

#### LIBPROCESS_SSL_KEY_FILE=(path to key)
The location of the private key used by OpenSSL.

~~~
// For example, to generate a key with OpenSSL:
openssl genrsa -des3 -f4 -passout pass:some_password -out key.pem 4096
~~~

#### LIBPROCESS_SSL_CERT_FILE=(path to certificate)
The location of the certificate that will be presented.

~~~
// For example, to generate a root certificate with OpenSSL:
// (assuming the signing key already exists in `key.pem`)
openssl req -new -x509 -passin pass:some_password -days 365 -keyout key.pem -out cert.pem
~~~

#### LIBPROCESS_SSL_VERIFY_CERT=(false|0,true|1) [default=false|0]
This is a legacy alias for the `LIBPROCESS_SSL_VERIFY_SERVER_CERT` setting.

#### LIBPROCESS_SSL_VERIFY_SERVER_CERT=(false|0,true|1) [default=false|0]
This setting only affects the behaviour of libprocess in TLS client mode.

If this is true, a remote server is required to present a server certificate,
and the presented server certificates will be verified. That means
it will be checked that the certificate is cryptographically valid,
was generated by a trusted CA, and contains the correct hostname.

If this is false, a remote server is still required to present a server certificate (unless
an anonymous cipher is used), but the presented server certificates will not be verified.

**NOTE:** When `LIBPROCESS_SSL_REQUIRE_CERT` is true, `LIBPROCESS_SSL_VERIFY_CERT` is automatically
set to true for backwards compatibility reasons.

#### LIBPROCESS_SSL_REQUIRE_CERT=(false|0,true|1) [default=false|0]
This is a legacy alias for the `LIBPROCESS_SSL_REQUIRE_CLIENT_CERT` setting.

#### LIBPROCESS_SSL_REQUIRE_CLIENT_CERT=(false|0,true|1) [default=false|0]
This setting only affects the behaviour of libprocess in TLS server mode.

If this is true, enforce that certificates must be presented by connecting clients. This means all
connections (including external tooling trying to access HTTP endpoints, like web browsers etc.)
must present valid certificates in order to establish a connection.

**NOTE:** The specifics of what it means for the certificate to "contain the correct hostname"
depend on the selected value of `LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME`.

**NOTE:** If this is set to false, client certificates are not verified even if they are presented
and `LIBPROCESS_SSL_VERIFY_CERT` is set to true.

#### LIBPROCESS_SSL_VERIFY_DEPTH=(N) [default=4]
The maximum depth used to verify certificates. The default is 4. See the OpenSSL documentation or contact your system administrator to learn why you may want to change this.

#### LIBPROCESS_SSL_VERIFY_IPADD=(false|0,true|1) [default=false|0]
Enable IP address verification in the certificate subject alternative name extension. When set
to `true` the peer certificate verification will be able to use the IP address of a peer connection.

The specifics on when a certificate containing an IP address will we accepted depend on the
selected value of the `LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME`.

#### LIBPROCESS_SSL_CA_DIR=(path to CA directory)
The directory used to find the certificate authority / authorities. You can specify `LIBPROCESS_SSL_CA_DIR` or `LIBPROCESS_SSL_CA_FILE` depending on how you want to restrict your certificate authorization.

#### LIBPROCESS_SSL_CA_FILE=(path to CA file)
The file used to find the certificate authority. You can specify `LIBPROCESS_SSL_CA_DIR` or `LIBPROCESS_SSL_CA_FILE` depending on how you want to restrict your certificate authorization.

#### LIBPROCESS_SSL_CIPHERS=(accepted ciphers separated by ':') [default=AES128-SHA:AES256-SHA:RC4-SHA:DHE-RSA-AES128-SHA:DHE-DSS-AES128-SHA:DHE-RSA-AES256-SHA:DHE-DSS-AES256-SHA]
A list of `:`-separated ciphers. Use these if you want to restrict or open up the accepted ciphers for OpenSSL. Read the OpenSSL documentation or contact your system administrators to see whether you want to override the default values.

#### LIBPROCESS_SSL_ENABLE_SSL_V3=(false|0,true|1) [default=false|0]
#### LIBPROCESS_SSL_ENABLE_TLS_V1_0=(false|0,true|1) [default=false|0]
#### LIBPROCESS_SSL_ENABLE_TLS_V1_1=(false|0,true|1) [default=false|0]
#### LIBPROCESS_SSL_ENABLE_TLS_V1_2=(false|0,true|1) [default=true|1]
#### LIBPROCESS_SSL_ENABLE_TLS_V1_3=(false|0,true|1) [default=false|0]
The above switches enable / disable the specified protocols. By default only TLS V1.2 is enabled. SSL V2 is always disabled; there is no switch to enable it. The mentality here is to restrict security by default, and force users to open it up explicitly. Many older version of the protocols have known vulnerabilities, so only enable these if you fully understand the risks.
TLS V1.3 is not supported yet and should not be enabled. [MESOS-9730](https://issues.apache.org/jira/browse/MESOS-9730).
_SSLv2 is disabled completely because modern versions of OpenSSL disable it using multiple compile time configuration options._
#<a name="Dependencies"></a>Dependencies

#### LIBPROCESS_SSL_ECDH_CURVE=(auto|list of curves separated by ':') [default=auto]
List of elliptic curves which should be used for ECDHE-based cipher suites, in preferred order. Available values depend on the OpenSSL version used. Default value `auto` allows OpenSSL to pick the curve automatically.
OpenSSL versions prior to `1.0.2` allow for the use of only one curve; in those cases, `auto` defaults to `prime256v1`.

#### LIBPROCESS_SSL_HOSTNAME_VALIDATION_SCHEME=(legacy|openssl) [default=legacy]
This flag is used to select the scheme by which the hostname validation check works.

Since hostname validation is part of certificate verification, this flag has no
effect unless one of `LIBPROCESS_SSL_VERIFY_SERVER_CERT` or `LIBPROCESS_SSL_REQUIRE_CLIENT_CERT`
is set to true.

Currently, it is possible to choose between two schemes:

  - `openssl`:

    In client mode: Perform the hostname validation checks during the TLS handshake.
    If the client connects via hostname, accept the certificate if it contains
    the hostname as common name (CN) or as a subject alternative name (SAN).
    If the client connects via IP address and `LIBPROCESS_SSL_VERIFY_IPADD` is true,
    accept the certificate if it contains the IP as a subject alternative name.

    **NOTE:** If the client connects via IP address and `LIBPROCESS_SSL_VERIFY_IPADD` is false,
    the connection attempt cannot succeed.

    In server mode: Do not perform any hostname validation checks.

    This setting requires OpenSSL >= 1.0.2 to be used.

  - `legacy`:

    Use a custom hostname validation algorithm that is run after the connection is established,
    and immediately close the connection if it fails.

    In both client and server mode:
    Do a reverse DNS lookup on the peer IP. If `LIBPROCESS_SSL_VERIFY_IPADD` is set to `false`,
    accept the certificate if it contains the first result of that lookup as either the common name
    or as a subject alternative name. If `LIBPROCESS_SSL_VERIFY_IPADD` is set to `true`,
    additionally accept the certificate if it contains the peer IP as a subject alternative name.


It is suggested that operators choose the 'openssl' setting unless they have
applications relying on the legacy behaviour of the 'libprocess' scheme. It is
using standardized APIs (`X509_VERIFY_PARAM_check_{host,ip}`) provided by OpenSSL to
make hostname validation more uniform across applications. It is also more secure,
since attackers that are able to forge a DNS or rDNS result can launch a successful
man-in-the-middle attack on the 'legacy' scheme.

### libevent
If building with `--enable-libevent`, we require the OpenSSL support from
libevent. The suggested version of libevent is
[`2.0.22-stable`](https://github.com/libevent/libevent/releases/tag/release-2.0.22-stable).
As new releases come out we will try to maintain compatibility.

~~~
// For example, on OSX:
brew install libevent
~~~

### OpenSSL
We require [OpenSSL](https://github.com/openssl/openssl).
There are multiple branches of OpenSSL that are being maintained by the
community. Since security requires being vigilant, we recommend reading
the release notes for the current releases of OpenSSL and deciding on a
version within your organization based on your security needs.

When building with libevent, Mesos is not too deeply dependent on specific
OpenSSL versions, so there is room for you to make security decisions as
an organization. When building without libevent, OpenSSL 1.1+ is required,
because Mesos makes use of APIs introduced in later versions of OpenSSL.

Please ensure the `event2` (when building with libevent) and
`openssl` headers are available for building Mesos.

~~~
// For example, on OSX:
brew install openssl
~~~

# <a name="Upgrading"></a>Upgrading Your Cluster
_There is no SSL specific requirement for upgrading different components in a specific order._

The recommended strategy is to restart all your components to enable SSL with downgrades support enabled. Once all components have SSL enabled, then do a second restart of all your components to disable downgrades. This strategy will allow each component to be restarted independently at your own convenience with no time restrictions. It will also allow you to try SSL in a subset of your cluster.

**NOTE:** While different components in your cluster are serving SSL vs non-SSL traffic, any relative links in the WebUI may be broken. Please see the [WebUI](#WebUI) section for details. Here are sample commands for upgrading your cluster:

~~~
// Restart each component with downgrade support (master, agent, framework):
LIBPROCESS_SSL_ENABLED=true LIBPROCESS_SSL_SUPPORT_DOWNGRADE=true LIBPROCESS_SSL_KEY_FILE=<path-to-your-private-key> LIBPROCESS_SSL_CERT_FILE=<path-to-your-certificate> <Any other LIBPROCESS_SSL_* environment variables you may choose> <your-component (e.g. bin/master.sh)> <your-flags>

// Restart each component WITHOUT downgrade support (master, agent, framework):
LIBPROCESS_SSL_ENABLED=true LIBPROCESS_SSL_SUPPORT_DOWNGRADE=false LIBPROCESS_SSL_KEY_FILE=<path-to-your-private-key> LIBPROCESS_SSL_CERT_FILE=<path-to-your-certificate> <Any other LIBPROCESS_SSL_* environment variables you may choose> <your-component (e.g. bin/master.sh)> <your-flags>
~~~
Executors must be able to access the SSL environment variables and the files referred to by those variables. Environment variables can be provided to an executor by specifying `CommandInfo.environment` or by using the agent's `--executor_environment_variables` command line flag. If the agent and the executor are running in separate containers, `ContainerInfo.volumes` can be used to mount SSL files from the host into the executor's container.

The end state is a cluster that is only communicating with SSL.

**NOTE:** Any tools you may use that communicate with your components must be able to speak SSL, or they will be denied. You may choose to maintain `LIBPROCESS_SSL_SUPPORT_DOWNGRADE=true` for some time as you upgrade your internal tooling. The advantage of `LIBPROCESS_SSL_SUPPORT_DOWNGRADE=true` is that all components that speak SSL will do so, while other components may still communicate over insecure channels.

# <a name="WebUI"></a>WebUI
The default Mesos WebUI uses relative links. Some of these links transition between endpoints served by the master and agents. The WebUI currently does not have enough information to change the 'http' vs 'https' links based on whether the target endpoint is currently being served by an SSL-enabled binary. This may cause certain links in the WebUI to be broken when a cluster is in a transition state between SSL and non-SSL. Any tools that hit these endpoints will still be able to access them as long as they hit the endpoint using the right protocol, or the `LIBPROCESS_SSL_SUPPORT_DOWNGRADE` option is set to true.

**NOTE:** Frameworks with their own WebUI will need to add HTTPS support separately.

### Certificates
Most browsers have built in protection that guard transitions between pages served using different certificates. For this reason you may choose to serve both the master and agent endpoints using a common certificate that covers multiple hostnames. If you do not do this, certain links, such as those to agent sandboxes, may seem broken as the browser treats the transition between differing certificates transition as unsafe.
