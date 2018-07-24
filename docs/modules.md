---
title: Apache Mesos - Modules
layout: documentation
---

# Mesos Modules

Experimental support for Mesos modules was introduced in Mesos 0.21.0.

### Disclaimer

- Use and development of Mesos modules is at own risk!

- Please direct all questions to the relevant module writer and/or write to
modules@mesos.apache.org. Questions related to modules sent to user and dev list
will be redirected to the modules list.


## What are Mesos modules?
Mesos modules provide a way to easily extend inner workings of Mesos by creating
and using shared libraries that are loaded on demand.  Modules can be used to
customize Mesos without having to recompiling/relinking for each specific use
case.  Modules can isolate external dependencies into separate libraries, thus
resulting into a smaller Mesos core.
Modules also make it easy to experiment with new features.
For example, imagine loadable allocators that contain a VM (Lua, Python, ...)
which makes it possible to try out new allocator algorithms written in
scripting languages without forcing those dependencies into the project.
Finally, modules provide an easy way for third parties to easily extend Mesos
without having to know all the internal details.

## Where can Mesos modules be used?
Modules can be specified for master, agent and tests. Modules can also be used
with schedulers that link against libmesos. Currently, modules cannot be
used with executors.

## <a name="Invoking"></a>Invoking Mesos modules
There are two ways to specify a list of modules to be loaded and be available to
the internal subsystems.

* `--modules=filepath` to specify the list of modules via a manifest file
  containing a JSON-formatted string. 'filepath' can be of the form
  'file:///path/to/file' or '/path/to/file'.

Use `--modules="{...}"` to specify the list of modules inline.

* `--modules_dir=dirpath`to specify a directory name that contains several
  module manifest files. Each file must contain a valid JSON-formatted
  string containing module description. The module manifest files are processed
  in alphabetical order.

NOTE: Only one of `--modules` or `--modules_dir` flag can be used at time.


### Example JSON strings:

1. Load a library `libfoo.so` with two modules `org_apache_mesos_bar` and
   `org_apache_mesos_baz`.

        {
          "libraries": [
            {
              "file": "/path/to/libfoo.so",
              "modules": [
                {
                  "name": "org_apache_mesos_bar",
                },
                {
                  "name": "org_apache_mesos_baz"
                }
              ]
            }
          ]
        }


2. Load the module `org_apache_mesos_bar` from the library `foo` and pass
   the command-line argument `X` with value `Y` (module `org_apache_mesos_baz`
   is loaded without any command-line parameters):

        {
          "libraries": [
            {
              "name": "foo",
              "modules": [
                {
                  "name": "org_apache_mesos_bar",
                  "parameters": [
                    {
                      "key": "X",
                      "value": "Y"
                    }
                  ]
                },
                {
                  "name": "org_apache_mesos_baz"
                }
              ]
            }
          ]
        }

3. Specifying modules inline:

        --modules='{"libraries":[{"file":"/path/to/libfoo.so", "modules":[{"name":"org_apache_mesos_bar"}]}]}'


### Library names

For each library, at least one of the "file" or "name" parameter must be
specified.  The "file" parameter may refer to a filename (e.g., "libfoo.so"), a
relative path (e.g., "myLibs/libfoo.so"), or an absolute path (e.g.,
"/home/mesos/lib/libfoo.so").  The "name" parameter refers to a library name
(e.g., "foo").  If "name" is specified, it is automatically expanded to a proper
library name for the current platform (e.g., "foo" gets expanded to "libfoo.so"
on Linux, and "libfoo.dylib" on OS X).

If a library path is not specified in the "file" parameter, the library is
searched in the standard library paths or directories pointed to by the
`LD_LIBRARY_PATH` (`DYLD_LIBRARY_PATH` on OS X) environment variable.

If both "file" and "name" are specified, "name" is ignored.

## What kinds of modules are supported?

Here are the various module kinds currently available.

| kind                                                | description                                                             |
|-----------------------------------------------------|-------------------------------------------------------------------------|
| [`Allocator`](#allocator)                           | Provides available resources to schedulers.                             |
| [`Anonymous`](#anonymous)                           | Coexists with parent process without providing any callbacks.           |
| [`Authenticatee`](#authentication)                  | Provides the client side of an RPC authentication.                      |
| [`Authenticator`](#authentication)                  | Provides the server side of an RPC authentication.                      |
| [`Authorizer`](#authorization)                      | Allows certain principals to perform specific actions.                  |
| `ContainerLogger`                                   | Container logger modules implement container log management.            |
| [`Hook`](#hook)                                     | Provides means of tying into specific callbacks.                        |
| [`HttpAuthenticatee`](#authentication)              | Provides the client side of an HTTP authentication.                     |
| [`HttpAuthenticator`](#authentication)              | Provides the server side of an HTTP authentication.                     |
| [`Isolator`](#isolator)                             | Enables experimenting with specialized isolation and monitoring.        |
| [`MasterContender`](#master-contender-and-detector) | Contender modules allow implementing custom leader election mechanisms. |
| [`MasterDetector`](#master-contender-and-detector)  | Detector modules allow implementing custom master detection mechanisms. |
| `QoSController`                                     | QoS modules allow providing task performance guarantees.                |
| `ResourceEstimator`                                 | Estimator modules allow predicting the resources total used.            |
| `SecretResolver`                                    | Secret resolver modules allow interfacing with secure data providers.   |
| `TestModule`                                        | Used internally for testing purposes only.                              |


### Allocator

The Mesos master's _allocator_ periodically determines which framework(s) should be offered the cluster's available resources. Allocator modules enable experimenting with specialized resource allocation algorithms. An example of these could be an allocator that provides a feature currently not supported by the built-in Hierarchical Dominant Resource Fairness allocator, like oversubscription with preemption.

To load a custom allocator into Mesos master, you need to:

- Introduce it to the Mesos master by listing it in the `--modules` configuration,

- Select it as the allocator via the `--allocator` flag.

For example, the following command will run the Mesos master with `ExternalAllocatorModule` (see [this section](#Example-JSON-strings) for JSON format):

    ./bin/mesos-master.sh --work_dir=m/work --modules="file://<modules-including-allocator>.json" --allocator=ExternalAllocatorModule


### Anonymous

Anonymous modules do __not__ receive any callbacks but simply
coexists with their parent process.

Unlike other named modules, an anonymous module does not directly
replace or provide essential Mesos functionality (such as an
Isolator module does). Unlike a decorator module it does not
directly add or inject data into Mesos core either.

Anonymous modules do not require any specific selectors (flags) as
they are immediately instantiated when getting loaded via the
`--modules` flag by the Mesos master or agent.


### Authentication

Authenticatee and Authenticator modules allow for third parties to quickly
develop and plug-in new authentication methods. An example for such modules
could be to support PAM (LDAP, MySQL, NIS, UNIX) backed authentication.

Mesos currently supports two variants of authentication; RPC authentication
as used by schedulers and agents to authenticate against the master and HTTP
authentication for securing all kinds of HTTP endpoints. See [Authentication](authentication.md) for more.


### Authorization

The authorization subsystem permits operators to configure the actions that certain
principals are allowed to perform. See [Authorization](authorization.md) for more.


### Hook

Similar to Apache Webserver Modules, hooks allows module writers to tie into
internal components which may not be suitable to be abstracted entirely behind
modules but rather lets them define actions on so-called hooks.

The available hooks API is defined in mesos/hook.hpp and for each hook defines
the insertion point and available context. An example of this context is the
task information which is passed to `masterLaunchTaskHook`.

Some hooks take in an object (e.g. `TaskInfo`) and return all or part of that
object (e.g. task labels), so that the hook can modify or replace the contents
in-flight. These hooks are referred to as _decorators_.

In order to enable decorator modules to remove metadata (environment variables
or labels), the effect of the return value for decorator hooks changed in
Mesos 0.23.0.

The Result<T> return values before and after Mesos 0.23.0 means:

<table class="table table-striped">
<tr>
<th>State</th>
<th>Before (0.22.x)</th>
<th>After (0.23.0+)</th>
</tr>
<tr>
<td>Error</td>
<td>Error is propagated to the call-site</td>
<td>No change</td>
</tr>
<tr>
<td>None</td>
<td>The result of the decorator is not applied</td>
<td>No change</td>
</tr>
<tr>
<td>Some</td>
<td>The result of the decorator is appended</td>
<td>The result of the decorator overwrites the final labels/environment
object</td>
</tr>
</table>

To load a hook into Mesos, you need to:

- introduce it to Mesos by listing it in the `--modules` configuration,

- select it as a hook module via the `--hooks` flag.

For example, the following command will run the Mesos agent with the
`TestTaskHook` hook:

    ./bin/mesos-agent.sh --master=<IP>:<PORT> --modules="file://<path-to-modules-config>.json" --hooks=TestTaskHook


### Isolator

Isolator modules enable experimenting with specialized isolation and monitoring
capabilities. Examples of these could be 3rdparty resource isolation mechanisms
for GPGPU hardware, networking, etc.

If a custom isolator module needs to checkpoint state to disk, it should do this
on its own; Mesos does not provide public helpers to checkpoint module state.
The standard location for such stored state is:

    <runtime_dir>/isolators/<name_of_isolator>/


### Master Contender and Detector

Contender and Detector modules enable developers to implement custom leader
election and master detection mechanisms, other than relying on Zookeeper by
default.

An example for such modules could be to use distributed Key-Value storage such
as [etcd](https://coreos.com/etcd/) and [consul](https://www.consul.io/).

To load custom contender and detector module, you need to:

- Supply `--modules` when running Mesos master,

- Specify selected contender and detector modules with `--master_contender` and
`--master_detector` flags on Mesos Master and `--master_detector` on Mesos Slave.

For example, the following command runs Mesos master with
`org_apache_mesos_TestMasterContender` and `org_apache_mesos_TestMasterDetector`:

`./bin/mesos-master.sh --modules="file://<path-to-modules-config>.json" --master_contender=org_apache_mesos_TestMasterContender --master_detector=org_apache_mesos_TestMasterDetector`

And this one runs Mesos slave with `org_apache_mesos_TestMasterDetector`:

`./bin/mesos-slave.sh --modules="file://<path-to-modules-config>.json" --master_detector=org_apache_mesos_TestMasterDetector`


## Writing Mesos modules

### A HelloWorld module

The following snippet describes the implementation of a module named
"org_apache_mesos_bar" of "TestModule" kind:

~~~{.cpp}
#include <iostream>
#include "test_module.hpp"

class TestModuleImpl : public TestModule
{
public:
  TestModuleImpl()
  {
    std::cout << "HelloWorld!" << std::endl;
  }

  virtual int foo(char a, long b)
  {
    return a + b;
  }

  virtual int bar(float a, double b)
  {
    return a * b;
  }
};

static TestModule* create()
{
    return new TestModule();
}

static bool compatible()
{
  return true;
}

// Declares a module named 'org_apache_mesos_TestModule' of
// 'TestModule' kind.
// Mesos core binds the module instance pointer as needed.
// The compatible() hook is provided by the module for compatibility checks.
// The create() hook returns an object of type 'TestModule'.
mesos::modules::Module<TestModule> org_apache_mesos_TestModule(
    MESOS_MODULE_API_VERSION,
    MESOS_VERSION,
    "Apache Mesos",
    "modules@mesos.apache.org",
    "This is a test module.",
    compatible,
    create);
~~~

### Building a module

  The following assumes that Mesos is installed in the standard location, i.e.
  the Mesos dynamic library and header files are available.

    g++ -lmesos -fpic -o test_module.o test_module.cpp
    $ gcc -shared -o libtest_module.so test_module.o

### Testing a module

Apart from testing the module by hand with explicit use of --modules flag, one
can run the entire Mesos test suite with the given module. For example, the
following command will run the Mesos test suite with the
`org_apache_mesos_TestCpuIsolator` module selected for isolation:

    ./bin/mesos-tests.sh --modules="/home/kapil/mesos/isolator-module/modules.json" --isolation="org_apache_mesos_TestCpuIsolator"


### Module naming convention
Each module name should be unique.  Having duplicate module names in the Json
string will cause the process to abort.

Therefore, we encourage module writers to name their modules according to Java
package naming scheme
(http://docs.oracle.com/javase/tutorial/java/package/namingpkgs.html).


For example:

<table class="table table-striped">
<tr>
<th> Module Name </th> <th> Module Domain name </th> <th> Module Symbol Name </th>
</tr>

<tr>
<td> foobar      </td> <td> mesos.apache.org   </td> <td> org_apache_mesos_foobar </td>
</tr>

<tr>
<td> barBaz      </td> <td> example.com        </td> <td> com_example_barBaz       </td>
</table>

In short:
- Keep case of module name.
- Lower case and reverse domain.
- Separate with underscore.
- Do not simply use the kind name as module name.
- Different modules from the same organization still need different names.


## Module Versioning and backwards compatibility

Before loading the above module, a dynamic library that contains the module
needs to be loaded into Mesos. This happens early in Mesos startup code. The
Mesos developer does not need to touch that code when introducing new module
kinds.  However, the developer is responsible for registering what versions of
any given module are expected to remain compatible with Mesos as it progresses.
This information is maintained in a table in `src/module/manager.cpp`. It
contains an entry for every possible module kind that Mesos recognizes, each
with a corresponding Mesos release version number. This number needs to be
adjusted by the Mesos developer to reflect the current Mesos version whenever
compatibility between Mesos and modules that get compiled against it gets
broken. Given that module implementation for older Mesos versions can still be
written in the future, this may be impossible to tell and so in doubt it is best
to just bump the required module version to the current Mesos version. But if
one can be reasonably sure, assuming cooperative module developers, that a
certain kind of module will continue to function across several Mesos versions,
the table provides an easy way to specify this.

For successfully loading the module, the following relationship
must exist between the various versions:

`  kind version <= Library version <= Mesos version`

<table class="table table-striped">
<tr>
<td>Mesos </td> <td>kind version </td> <td> Library </td> <td>Is module loadable </td> <td>Reason </td>
</tr>

<tr>
<td>0.18.0 </td> <td> 0.18.0 </td> <td> 0.18.0  </td> <td> yes </td> <td> </td>
</tr>

<tr>
<td>1.0.0 </td> <td> 0.18.0 </td> <td> 0.18.0  </td> <td> yes </td> <td> </td>
</tr>

<tr>
<td>1.0.0 </td> <td> 0.18.0 </td> <td> 0.21.0  </td> <td> yes </td> <td> </td>
</tr>

<tr>
<td>0.18.0 </td> <td> 0.18.0 </td> <td> 1.0.0  </td> <td> NO </td> <td> Library compiled against a newer Mesos release.                </td>
</tr>

<tr>
<td>1.0.0 </td> <td> 0.21.0 </td> <td> 0.18.0  </td> <td> NO </td> <td> Module/Library older than the kind version supported by Mesos. </td>
</tr>

<tr>
<td>1.0.0 </td> <td> 1.0.0 </td> <td> 0.18.0  </td> <td> NO </td> <td> Module/Library older than the kind version supported by Mesos. </td>
</tr>
</table>

## Mesos module API changes

Record of incompatible changes to the modules API.

### Version 2
- Added support for module-specific command-line parameters.
- Changed function signature for create().

### Version 1
Initial version of the modules API.


## Appendix:
### JSON Schema:

    {
      "type":"object",
      "required":false,
      "properties":{
        "libraries":{
          "type":"array",
          "required":false,
          "items":{
            "type":"object",
            "required":false,
            "properties":{
              "file":{
                "type":"string",
                "required":false
              },
              "name":{
                "type":"string",
                "required":false
              },
              "modules":{
                "type":"array",
                "required":false,
                "items":{
                  "type":"object",
                  "required":false,
                  "properties":{
                    "name":{
                      "type":"string",
                      "required":true
                    },
                    "parameters":{
                      "type":"array",
                      "required":false,
                      "items":{
                        "type":"object",
                        "required":false,
                        "properties":{
                          "key":{
                            "type":"string",
                            "required":true
                          },
                          "value":{
                            "type":"string",
                            "required":true
                          }
                        }
                      }
                    }
                  }
                }
              }
            }
          }
        }
      }
    }
