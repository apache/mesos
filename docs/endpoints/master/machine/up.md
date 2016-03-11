---
title: Apache Mesos - HTTP Endpoints - /machine/up
layout: documentation
---
<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /machine/up
>        /master/machine/up

### TL;DR; ###
Brings a set of machines back up.

### DESCRIPTION ###
POST: Validates the request body as JSON and transitions
  the list of machines into UP mode.  This also removes
  the list of machines from the maintenance schedule.