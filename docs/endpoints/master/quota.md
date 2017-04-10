---
title: Apache Mesos - HTTP Endpoints - /quota
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /quota
>        /master/quota

### TL;DR; ###
Gets or updates quota for roles.

### DESCRIPTION ###
Returns 200 OK when the quota was queried or updated successfully.

Returns 307 TEMPORARY_REDIRECT redirect to the leading master when
current master is not the leader.

Returns 503 SERVICE_UNAVAILABLE if the leading master cannot be
found.

GET: Returns the currently set quotas as JSON.

POST: Validates the request body as JSON
 and sets quota for a role.

DELETE: Validates the request body as JSON
 and removes quota for a role.


### AUTHENTICATION ###
This endpoint requires authentication iff HTTP authentication is
enabled.

### AUTHORIZATION ###
Using this endpoint to set a quota for a certain role requires that
the current principal is authorized to set quota for the target role.
Similarly, removing quota requires that the principal is authorized
to remove quota created by the quota_principal.
Getting quota information for a certain role requires that the
current principal is authorized to get quota for the target role,
otherwise the entry for the target role could be silently filtered.
See the authorization documentation for details.