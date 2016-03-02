---
title: Apache Mesos - HTTP Endpoints - /master/machine/down
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /master/machine/down

### TL;DR; ###
Brings a set of machines down.

### DESCRIPTION ###
POST: Validates the request body as JSON and transitions
  the list of machines into DOWN mode.  Currently, only
  machines in DRAINING mode are allowed to be brought down.