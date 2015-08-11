---
layout: documentation
---

# Authentication

Mesos 0.15.0 added support for framework authentication, and 0.19.0 added slave authentication.  Authentication permits only trusted entities to interact with the Mesos cluster.

Authentication is used by Mesos in three ways:

1. Require that frameworks must be authenticated in order to register with the master.
2. Require that slaves must be authenticated in order to offer resources to the master.
3. To restrict access to the /teardown endpoint.

## How Does It Work?

Mesos uses the [Cyrus SASL library](http://asg.web.cmu.edu/sasl/) to implement authentication.  SASL is a very flexible authentication framework that allows two endpoints to authenticate with each other and also has support for various authentication mechanisms (ANONYMOUS, PLAIN, CRAM-MD5, GSSAPI etc).  Currently, Mesos provides support for CRAM-MD5 authentication, but users can provide their own authentication modules.  CRAM-MD5 makes use of a **principal** and **secret** pair, with the principal representing the framework's identity.  Note that this is different from the framework *user*, which is the account used by the executor to run tasks, and the *role* which is used to determine what resources frameworks can use.

## Configuration

The [configuration options](http://mesos.apache.org/documentation/latest/configuration/) that are used by the authentication mechanism are as follows:

### Masters

* --[no-]authenticate - If authenticate is 'true' only authenticated frameworks are allowed to register. If 'false' unauthenticated frameworks are also allowed to register.
* --[no-]authenticate_slaves - If 'true' only authenticated slaves are allowed to register. If 'false' unauthenticated slaves are also allowed to register.
* --authenticators - Specifies which authenticator module to use.  The default is crammd5, but additional modules can be added with the --modules option.
* --credentials - The path to the text file which contains a list (either plain text or JSON) of accepted credentials.  This may be optional depending on the authenticator being used.  However, if specified, the credentials will be valid for the /teardown endpoint regardless of which authenticator is used.

### Slaves

* --authenticatee - Analog to the master --authenticators option to specify what module to use.  Defaults to crammd5.
* --credential - Just like the master --credentials option, except only one credential is allowed, since this credential is used to identify the slave to the master.

## CRAM-MD5 Example

1. First, create a credentials file for the masters, the contents of which should look like this:

        principal1 secret1
        principal2 secret2

2. Now, start the master process using your credentials file (assuming the file is ~/credentials):

        ./bin/mesos-master.sh --ip=127.0.0.1 --work_dir=/var/lib/mesos --authenticate --authenticate_slaves --credentials=~/credentials

3. Now create another file with a single credential in it (~/slave_credential):

        principal1 secret1

4.  That file will be used to identify the slave process.  Start the slave:

        ./bin/mesos-slave.sh --master=127.0.0.1:5050 --credential=~/slave_credential

5.  Your new slave should have now successfully authenticated with the master.  With these settings, any framework that you'd like to use must also authenticate against the Mesos master.  The method of configuring framework authentication may vary by framework, but is simple to implement as the scheduler driver will handle authentication when a Credential object is passed to its constructor.  You can test out framework authentication using the test framework provided with Mesos as follows:

        MESOS_AUTHENTICATE=true DEFAULT_PRINCIPAL=principal2 DEFAULT_SECRET=secret2 ./src/test-framework --master=127.0.0.1:5050
