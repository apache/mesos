---
layout: documentation
---

# Configuration
There is currently only one implementation of the [libprocess socket interface](https://github.com/apache/mesos/blob/master/3rdparty/libprocess/include/process/socket.hpp) that supports SSL. This implementation uses [libevent](https://github.com/libevent/libevent). Specifically it relies on the `libevent-openssl` library that wraps `openssl`.

After building Mesos 0.23.0 from source, assuming you have installed the required [Dependencies](#Dependencies), you can modify your configure line to enable SSL as follows:

~~~
../configure --enable-libevent --enable-ssl
~~~

# Running
Once you have successfully built and installed your new binaries, here are the environment variables that are applicable to the `Master`, `Slave`, `Framework Scheduler/Executor`, or any `libprocess process`:

#### SSL_ENABLED=(false|0,true|1) [default=false|0]
Turn on or off SSL. When it is turned off it is the equivalent of default mesos with libevent as the backing for events. All sockets default to the non-SSL implementation. When it is turned on, the default configuration for sockets is SSL. This means outgoing connections will use SSL, and incoming connections will be expected to speak SSL as well. None of the below flags are relevant if SSL is not enabled.

#### SSL_SUPPORT_DOWNGRADE=(false|0,true|1) [default=false|0]
Control whether or not non-SSL connections can be established. If this is enabled __on the accepting side__, then the accepting side will downgrade to a non-SSL socket if the connecting side is attempting to communicate via non-SSL. (e.g. HTTP). See [Upgrading Your Cluster](#Upgrading) for more details.

#### SSL_CERT_FILE=(path to certificate)
The location of the certificate that will be presented.

#### SSL_KEY_FILE=(path to key)
The location of the private key used by OpenSSL.

#### SSL_VERIFY_CERT=(false|0,true|1) [default=false|0]
Control whether certificates are verified when presented. If this is false, even when a certificate is presented, it will not be verified. When `SSL_REQUIRE_CERT` is true, `SSL_VERIFY_CERT` is overridden and all certificates will be verified _and_ required.

#### SSL_REQUIRE_CERT=(false|0,true|1) [default=false|0]
Enforce that certificates must be presented by connecting clients. This means all connections (including tools hitting endpoints) must present valid certificates in order to establish a connection.

#### SSL_VERIFY_DEPTH=(N) [default=4]
The maximum depth used to verify certificates. The default is 4. See the OpenSSL documentation or contact your system administrator to learn why you may want to change this.

#### SSL_CA_DIR=(path to CA directory)
The directory used to find the certificate authority / authorities. You can specify `SSL_CA_DIR` or `SSL_CA_FILE` depending on how you want to restrict your certificate authorization.

#### SSL_CA_FILE=(path to CA file)
The file used to find the certificate authority. You can specify `SSL_CA_DIR` or `SSL_CA_FILE` depending on how you want to restrict your certificate authorization.

#### SSL_CIPHERS=(accepted ciphers separated by ':') [default=AES128-SHA:AES256-SHA:RC4-SHA:DHE-RSA-AES128-SHA:DHE-DSS-AES128-SHA:DHE-RSA-AES256-SHA:DHE-DSS-AES256-SHA]
A list of `:`-separated ciphers. Use these if you want to restrict or open up the accepted ciphers for OpenSSL. Read the OpenSSL documentation or contact your system administrators to see whether you want to override the default values.

#### SSL_ENABLE_SSL_V3=(false|0,true|1) [default=false|0]
#### SSL_ENABLE_TLS_V1_0=(false|0,true|1) [default=false|0]
#### SSL_ENABLE_TLS_V1_1=(false|0,true|1) [default=false|0]
#### SSL_ENABLE_TLS_V1_2=(false|0,true|1) [default=true|1]
The above switches enable / disable the specified protocols. By default only TLS V1.2 is enabled. SSL V2 is always disabled; there is no switch to enable it. The mentality here is to restrict security by default, and force users to open it up explicitly. Many older version of the protocols have known vulnerabilities, so only enable these if you fully understand the risks.
_SSLv2 is disabled completely because modern versions of OpenSSL disable it using multiple compile time configuration options._
#<a name="Dependencies"></a>Dependencies

### libevent
We require the OpenSSL support from libevent. The suggested version of libevent is [`2.0.22-stable`](https://github.com/libevent/libevent/releases/tag/release-2.0.22-stable). As new releases come out we will try to maintain compatibility.

~~~
// For example, on OSX:
brew install libevent
~~~

### OpenSSL
We require [OpenSSL](https://github.com/openssl/openssl). There are multiple branches of OpenSSL that are being maintained by the community. Since security requires being vigilant, we recommend reading the release notes for the current releases of OpenSSL and deciding on a version within your organization based on your security needs. Mesos is not too deeply dependent on specific OpenSSL versions, so there is room for you to make security decisions as an organization.
Please ensure the `event2` and `openssl` headers are available for building mesos.

~~~
// For example, on OSX:
brew install openssl
~~~

# <a name="Upgrading"></a>Upgrading Your Cluster
_There is no SSL specific requirement for upgrading different components in a specific order._

The recommended strategy is to restart all your components to enable SSL with downgrades support enabled. Once all components have SSL enabled, then do a second restart of all your components to disable downgrades. This strategy will allow each component to be restarted independently at your own convenience with no time restrictions. It will also allow you to try SSL in a subset of your cluster. __NOTE:__ While different components in your cluster are serving SSL vs non-SSL traffic, any relative links in the WebUI may be broken. Please see the [WebUI](#WebUI) section for details. Here are sample commands for upgrading your cluster:

~~~
// Restart each component with downgrade support (master, slave, framework):
SSL_ENABLED=true SSL_SUPPORT_DOWNGRADE=true SSL_KEY_FILE=<path-to-your-private-key> SSL_CERT_FILE=<path-to-your-certificate> <Any other SSL_* environment variables you may choose> <your-component (e.g. bin/master.sh)> <your-flags>

// Restart each component WITHOUT downgrade support (master, slave, framework):
SSL_ENABLED=true SSL_SUPPORT_DOWNGRADE=false SSL_KEY_FILE=<path-to-your-private-key> SSL_CERT_FILE=<path-to-your-certificate> <Any other SSL_* environment variables you may choose> <your-component (e.g. bin/master.sh)> <your-flags>
~~~
The end state is a cluster that is only communicating with SSL.

__NOTE:__ Any tools you may use that communicate with your components must be able to speak SSL, or they will be denied. You may choose to maintain `SSL_SUPPORT_DOWNGRADE=true` for some time as you upgrade your internal tooling. The advantage of `SSL_SUPPORT_DOWNGRADE=true` is that all components that speak SSL will do so, while other components may still communicate over insecure channels.

# <a name="WebUI"></a>WebUI
The default Mesos WebUI uses relative links. Some of these links transition between endpoints served by the master and slaves. The WebUI currently does not have enough information to change the 'http' vs 'https' links based on whether the target endpoint is currently being served by an SSL-enabled binary. This may cause certain links in the WebUI to be broken when a cluster is in a transition state between SSL and non-SSL. Any tools that hit these endpoints will still be able to access them as long as they hit the endpoint using the right protocol, or the `SSL_SUPPORT_DOWNGRADE` option is set to true.

### Certificates
Most browsers have built in protection that guard transitions between pages served using different certificates. For this reason you may choose to serve both the master and slave endpoints using a common certificate that covers multiple hostnames. If you do not do this, certain links, such as those to slave sandboxes, may seem broken as the browser treats the transition between differing certificates transition as unsafe.
