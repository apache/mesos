---
title: Apache Mesos - Downgrade Compatibility
layout: documentation
---

# Downgrade Mesos

This document serves as a guide for users who wish to downgrade from an
existing Mesos cluster to a previous version. This usually happens when
rolling back from problematic upgrades. Mesos provides compatibility
between any 1.x and 1.y versions of masters/agents as long as new features
are not used. Since Mesos 1.8, we introduced a check for minimum capabilities
on the master. If a backwards incompatible feature is used, a corresponding
minimum capability entry will be persisted to the registry. If an old master
(that does not possess the capability) tries to recover from the registry
(e.g. when rolling back), an error message will be printed containing the
missing capabilities. This document lists the detailed information regarding
these minimum capabilities and remediation for downgrade errors.


## List of Master Minimum Capabilities

Currently, no minimum capabilities will block downgrades.
