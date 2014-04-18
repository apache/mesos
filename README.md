# About Apache Mesos

Apache Mesos is a cluster manager that provides efficient resource isolation 
and sharing across distributed applications, or frameworks. It can run Hadoop, 
MPI, Hypertable, Spark, and other frameworks on a dynamically shared pool of 
nodes.

Visit us at [mesos.apache.org](http://mesos.apache.org).

# Mailing Lists

 * [User Mailing List](mailto:user-subscribe@mesos.apache.org) ([Archive](https://mail-archives.apache.org/mod_mbox/mesos-user/))
 * [Development Mailing List](mailto:dev-subscribe@mesos.apache.org) ([Archive](https://mail-archives.apache.org/mod_mbox/mesos-dev/))

# Documentation

Documentation is available in the docs/ directory. Additionally, a rendered HTML 
version can be found on the Mesos website's [Documentation](http://mesos.apache.org/documentation/) page.

# Profiling

    ./bootstrap

Enable gprof (and gcov)
    ./configure --enable_gprof=yes
    
    make
    
    make check

run the test

    ./src/mesos-tests

use gcov from src

    cd src

collect output with xargs and send to gcov

    find -name \*.gcno | xargs gcov

You will see currently that only 78% is executed:

    Lines executed:77.62% of 28906

Running gprof will show you the stats, but do that to the libtool binary
    gprof ./.libs/mesos-tests

It will show you the top functions where time is spent, for example :
```
  %   cumulative   self              self     total  
 time   seconds   seconds    calls  ms/call  ms/call  name    
 14.10      0.11     0.11   260182     0.00     0.00  boost::uuids::detail::sha1::process_block()
  7.69      0.17     0.06 14066728     0.00     0.00  boost::uuids::detail::sha1::process_byte(unsigned char)
  7.69      0.23     0.06   171753     0.00     0.00  std::string stringify<int>(int)
  5.13      0.27     0.04   130762     0.00     0.00  boost::uuids::detail::seed_rng::sha1_random_digest_()
  4.49      0.31     0.04    44383     0.00     0.00  process::internal::release(int*)
  3.85      0.34     0.03    84876     0.00     0.00  os::process(int)
  2.56      0.36     0.02   170519     0.00     0.00  Try<Duration>::get() const
  2.56      0.38     0.02    85252     0.00     0.00  proc::status(int)
```

# Installation

Instructions are included on the [Getting Started](http://mesos.apache.org/gettingstarted/) page.

# License

Apache Mesos is licensed under the [Apache License, Version 2.0](http://www.apache.org/licenses/LICENSE-2.0).

For additional information, see the LICENSE and NOTICE files.
