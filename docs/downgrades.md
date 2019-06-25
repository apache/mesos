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

<table class="table table-striped">
<thead>
<tr><th>Capability</th><th>Description</th>
</thead>

<tr>
  <td>
    <code>AGENT_DRAINING</code>
  </td>
  <td>
    This capability is required when any agent is marked for draining
    or deactivated.  These states were added in Mesos 1.9 and are
    triggered by using the <code>DRAIN_AGENT</code> or
    <code>DEACTIVATE_AGENT</code> operator APIs.
    <br/>
    To remove this minimum capability requirement:
    <ol>
      <li>
        Stop the master downgrade and return to the more recent version.
      </li>
      <li>
        Find all agents that are marked for draining or deactivated.
        This can be done by using the <code>GET_AGENTS</code> operator
        API and checking the <code>deactivated</code> boolean field of
        each agent.  All draining agents will also be deactivated.
      </li>
      <li>
        Use the <code>REACTIVATE_AGENT</code> operator API for each
        deactivated agent.
      </li>
    </ol>
  </td>
</tr>

<tr>
  <td>
    <code>QUOTA_V2</code>
  </td>
  <td>
    This capability is required when quota is configured in Mesos 1.9 or
    higher. When that happens, the newly configured quota will be persisted
    in the <code>quota_configs</code> field in the registry which requires this
    capability to decode.
    <br/>
    To remove this minimum capability requirement:
    <ol>
      <li>
        Stop the master downgrade and return to the more recent version.
      </li>
      <li>
        Use the <code>/registrar(id)/registry</code> endpoint to read the
        registry content and identify roles listed under the
        <code>quota_configs</code> field.
      </li>
      <li>
        Reset those roles' quota back to default (no guarantees and no limits).
        This will remove the roles from the <code>quota_configs</code> field.
        Once <code>quota_configs</code> becomes empty, the capability
        requirement will be removed.
      </li>
    </ol>
  </td>
</tr>
</table>
