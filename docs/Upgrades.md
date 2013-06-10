Upgrading Mesos
===============
This document serves as a guide for users who wish to upgrade an existing mesos cluster. Some versions require particular upgrade techniques when upgrading a running cluster. Some upgrades will have incompatible changes.

Upgrading from 0.11.0 to 0.12.0.
--------------------------------
In order to upgrade a running cluster:
  - First upgrade all of the slaves.
  - Then, once all slaves are running 0.12.0, upgrade the masters.

If you are a framework developer, you will want to examine the new 'source' field in the ExecutorInfo protobuf. This will allow you to take further advantage of the resource monitoring.
