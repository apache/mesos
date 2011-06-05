To build the Java, Python and Ruby libraries for Mesos, you need to pass the following arguments to configure, respectively:
  --with-java-home=<path to Java Development Kit install directory>
  --with-python-headers=<path to Python headers>
  --with-ruby-headers=<path to Ruby headers>

For example, on Ubuntu 9.04, the following directories can be used:
  --with-java-home=/usr/lib/jvm/java-6-sun
  --with-python-headers=/usr/include/python2.6
  --with-ruby-headers=/usr/lib/ruby/1.8/x86_64-linux

To get these headers in the first place, you will need to install development
packages for each language for your system. For example, in Ubuntu 9.04,
the Python and Ruby packages are python-dev and ruby-dev, and the Java
Development Kit package is sun-java6-jdk.

Finally, to run executors written in one of these languages, you will need
to properly set up the include paths for each one. The easiest way right
now is to create a shell script as your executor that you place in src/mesos,
and have this script set language-specific paths and run your code.
For example, for a Python executor exec.py, your script might look like:

#!/bin/sh
PYTHONPATH=<mesos_home>/src/swig/python
exec python <absolute_path>/exec.py

In Java, you would need to set the CLASSPATH and the Java library path (with LD_LIBRARY_PATH or -Djava.library.path). In Ruby, use the -I option.
