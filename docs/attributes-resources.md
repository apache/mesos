---
layout: documentation
---

# Mesos Attributes & Resources

The Mesos system has two basic methods to describe the slaves that comprise a cluster.  One of these is managed by the Mesos master, the other is simply passed onwards to the frameworks using the cluster.

## Attributes

The attributes are simply key value string pairs that Mesos passes along when it sends offers to frameworks.

    attributes : attribute ( ";" attribute )*

    attribute : labelString ":" ( labelString | "," )+

## Resources

The Mesos system can manage 3 different *types* of resources: scalars, ranges, and sets.  These are used to represent the different resources that a Mesos slave has to offer.  For example, a scalar resource type could be used to represent the amount of memory on a slave.  Each resource is identified by a key string.

    resources : resource ( ";" resource )*

    resource : key ":" ( scalar | range | set )

    key : labelString ( "(" resourceRole ")" )?

    scalar : floatValue

    range : "[" rangeValue ( "," rangeValue )* "]"

    rangeValue : scalar "-" scalar

    set : "{" labelString ( "," labelString )* "}"

    resourceRole : labelString | "*"

    labelString : [a-zA-Z0-9_/.-]

    floatValue : ( intValue ( "." intValue )? ) | ...

    intValue : [0-9]+

## Predefined Uses & Conventions

The Mesos master has a few resources that it pre-defines in how it handles them.  At the current time, this list consist of:

  - `cpus`
  - `mem`
  - `disk`
  - `ports`

In particular, a slave without `cpus` and `mem` resources will never have its resources advertised to any frameworks.  Also, the Master's user interface interprets the scalars in `mem` and `disk` in terms of *`MB`*.  IE: the value `15000` is displayed as `14.65GB`.

## Examples

Here are some examples for configuring the Mesos slaves.

    --resources='cpus:24;mem:24576;disk:409600;ports:[21000-24000];bugs:{a,b,c}'
    --attributes='rack:abc;zone:west;os:centos5,full'

In this case, we have three different types of resources, scalars, a range, and a set.  They are called `cpus`, `mem`, `disk`, and the range type is `ports`.

  - scalar called `cpus`, with the value `24`
  - scalar called `mem`, with the value `24576`
  - scalar called `disk`, with the value `409600`
  - range called `ports`, with values `21000` through `24000` (inclusive)
  - set called `bugs`, with the values `a`, `b` and `c`

In the case of attributes, we end up with three attributes:

  - `rack` with value `abc`
  - `zone` with value `west`
  - `os` with value `centos5,full`
