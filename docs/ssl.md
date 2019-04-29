---
title: Apache Mesos - SSL in Mesos
layout: documentation
---

# SSL in Mesos

By default, all the messages that flow through the Mesos cluster are unencrypted, making it possible for anyone with access to the cluster to intercept and potentially control arbitrary tasks.

SSL/TLS support was added to libprocess in Mesos 0.23.0, which encypts the low-level communication that Mesos uses for network communication between Mesos components.  Additionally, HTTPS support was added to the Mesos WebUI.

# Configuration
There is currently only one implementation of the [libprocess socket interface](https://github.com/apache/mesos/blob/master/3rdparty/libprocess/include/process/socket.hpp) that supports SSL. This implementation uses [libevent](https://github.com/libevent/libevent). Specifically it relies on the `libevent-openssl` library that wraps `openssl`.

Before building Mesos 0.23.0 from source, assuming you have installed the required [Dependencies](#Dependencies), you can modify your configure line to enable SSL as follows:

~~~
../configure --enable-libevent --enable-ssl
~~~

# Running
Once you have successfully built and installed your new binaries, here are the environment variables that are applicable to the `Master`, `Agent`, `Framework Scheduler/Executor`, or any `libprocess process`:

**NOTE:** Prior to 1.0, the SSL related environment variables used to be prefixed by `SSL_`. However, we found that they may collide with other programs and lead to unexpected results (e.g., openssl, see [MESOS-5863](https://issues.apache.org/jira/browse/MESOS-5863) for details). To be backward compatible, we accept environment variables prefixed by both `SSL_` or `LIBPROCESS_SSL_`. New users should use the `LIBPROCESS_SSL_` version.

#### LIBPROCESS_SSL_ENABLED=(false|0,true|1) [default=false|0]
Turn on or off SSL. When it is turned off it is the equivalent of default Mesos with libevent as the backing for events. All sockets default to the non-SSL implementation. When it is turned on, the default configuration for sockets is SSL. This means outgoing connections will use SSL, and incoming connections will be expected to speak SSL as well. None of the below flags are relevant if SSL is not enabled.  If SSL is enabled, `LIBPROCESS_SSL_CERT_FILE` and `LIBPROCESS_SSL_KEY_FILE` must be supplied.

#### LIBPROCESS_SSL_SUPPORT_DOWNGRADE=(false|0,true|1) [default=false|0]
Control whether or not non-SSL connections can be established. If this is enabled __on the accepting side__, then the accepting side will downgrade to a non-SSL socket if the connecting side is attempting to communicate via non-SSL. (e.g. HTTP). See [Upgrading Your Cluster](#Upgrading) for more details.

#### LIBPROCESS_SSL_KEY_FILE=(path to key)
The location of the private key used by OpenSSL.

~~~
// For example, to generate a key with OpenSSL:
openssl genrsa -des3 -f4 -passout pass:some_password -out key.pem 4096
~~~

#### LIBPROCESS_SSL_CERT_FILE=(path to certificate)
The location of the certificate that will be presented.

~~~
// For example, to generate a certificate with OpenSSL:
openssl req -new -x509 -passin pass:some_password -days 365 -key key.pem -out cert.pem
~~~

#### LIBPROCESS_SSL_VERIFY_CERT=(false|0,true|1) [default=false|0]
Control whether certificates are verified when presented. If this is false, even when a certificate is presented, it will not be verified. When `LIBPROCESS_SSL_REQUIRE_CERT` is true, `LIBPROCESS_SSL_VERIFY_CERT` is overridden and all certificates will be verified _and_ required.

#### LIBPROCESS_SSL_REQUIRE_CERT=(false|0,true|1) [default=false|0]
Enforce that certificates must be presented by connecting clients. This means all connections (including tools hitting endpoints) must present valid certificates in order to establish a connection.

#### LIBPROCESS_SSL_VERIFY_DEPTH=(N) [default=4]
The maximum depth used to verify certificates. The default is 4. See the OpenSSL documentation or contact your system administrator to learn why you may want to change this.

#### LIBPROCESS_SSL_VERIFY_IPADD=(false|0,true|1) [default=false|0]
Enable IP address verification in the certificate subject alternative name extension. When set to `true` the peer certificate verification will additionally use the IP address of a peer connection. When a hostname of the peer as well as its IP address are available, the validation will succeed when either the hostname or the IP match.

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

### libevent
We require the OpenSSL support from libevent. The suggested version of libevent is [`2.0.22-stable`](https://github.com/libevent/libevent/releases/tag/release-2.0.22-stable). As new releases come out we will try to maintain compatibility.

~~~
// For example, on OSX:
brew install libevent
~~~

### OpenSSL
We require [OpenSSL](https://github.com/openssl/openssl). There are multiple branches of OpenSSL that are being maintained by the community. Since security requires being vigilant, we recommend reading the release notes for the current releases of OpenSSL and deciding on a version within your organization based on your security needs. Mesos is not too deeply dependent on specific OpenSSL versions, so there is room for you to make security decisions as an organization.
Please ensure the `event2` and `openssl` headers are available for building Mesos.

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
