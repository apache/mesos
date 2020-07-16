---
title: Apache Mesos - Attributes and Resources
layout: documentation
---

# Mesos Attributes & Resources

Mesos has two basic methods to describe the agents that comprise a cluster. One of these is managed by the Mesos master, the other is simply passed onwards to the frameworks using the cluster.

## Types

The types of values that are supported by Attributes and Resources in Mesos are scalar, ranges, sets and text.

The following are the definitions of these types:

    scalar : floatValue

    floatValue : ( intValue ( "." intValue )? ) | ...

    intValue : [0-9]+

    range : "[" rangeValue ( "," rangeValue )* "]"

    rangeValue : scalar "-" scalar

    set : "{" text ( "," text )* "}"

    text : [a-zA-Z0-9_/.-]

## Attributes

Attributes are key-value pairs (where value is optional) that Mesos passes along when it sends offers to frameworks. An attribute value supports three different *types*: scalar, range or text.

    attributes : attribute ( ";" attribute )*

    attribute : text ":" ( scalar | range | text )

Note that setting multiple attributes corresponding to the same key is highly
discouraged (and might be disallowed in future), as this complicates attribute-
based filtering of offers, both on schedulers side and on the Mesos side.

## Resources

Mesos can manage three different *types* of resources: scalars, ranges, and sets.  These are used to represent the different resources that a Mesos agent has to offer.  For example, a scalar resource type could be used to represent the amount of memory on an agent. Scalar resources are represented using floating point numbers to allow fractional values to be specified (e.g., "1.5 CPUs"). Mesos only supports three decimal digits of precision for scalar resources (e.g., reserving "1.5123 CPUs" is considered equivalent to reserving "1.512 CPUs"). For GPUs, Mesos only supports whole number values.

Resources can be specified either with a JSON array or a semicolon-delimited string of key-value pairs.  If, after examining the examples below, you have questions about the format of the JSON, inspect the `Resource` protobuf message definition in `include/mesos/mesos.proto`.

As JSON:

    [
      {
        "name": "<resource_name>",
        "type": "SCALAR",
        "scalar": {
          "value": <resource_value>
        }
      },
      {
        "name": "<resource_name>",
        "type": "RANGES",
        "ranges": {
          "range": [
            {
              "begin": <range_beginning>,
              "end": <range_ending>
            },
            ...
          ]
        }
      },
      {
        "name": "<resource_name>",
        "type": "SET",
        "set": {
          "item": [
            "<first_item>",
            ...
          ]
        },
        "role": "<role_name>"
      },
      ...
    ]

As a list of key-value pairs:

    resources : resource ( ";" resource )*

    resource : key ":" ( scalar | range | set )

    key : text ( "(" resourceRole ")" )?

    resourceRole : text | "*"

Note that `resourceRole` must be a valid role name; see the [roles](roles.md) documentation for details.

## Predefined Uses & Conventions

There are several kinds of resources that have predefined behavior:

  - `cpus`
  - `gpus`
  - `disk`
  - `mem`
  - `ports`

Note that `disk` and `mem` resources are specified in megabytes. The master's user interface will convert resource values into a more human-readable format: for example, the value `15000` will be displayed as `14.65GB`.

An agent without `cpus` and `mem` resources will not have its resources advertised to any frameworks.

## Examples

By default, Mesos will try to autodetect the resources available at the local machine when `mesos-agent` starts up. Alternatively, you can explicitly configure which resources an agent should make available.

Here are some examples of how to configure the resources at a Mesos agent:

    --resources='cpus:24;gpus:2;mem:24576;disk:409600;ports:[21000-24000,30000-34000];bugs(debug_role):{a,b,c}'

    --resources='[{"name":"cpus","type":"SCALAR","scalar":{"value":24}},{"name":"gpus","type":"SCALAR","scalar":{"value":2}},{"name":"mem","type":"SCALAR","scalar":{"value":24576}},{"name":"disk","type":"SCALAR","scalar":{"value":409600}},{"name":"ports","type":"RANGES","ranges":{"range":[{"begin":21000,"end":24000},{"begin":30000,"end":34000}]}},{"name":"bugs","type":"SET","set":{"item":["a","b","c"]},"role":"debug_role"}]'

Or given a file `resources.txt` containing the following:

    [
      {
        "name": "cpus",
        "type": "SCALAR",
        "scalar": {
          "value": 24
        }
      },
      {
        "name": "gpus",
        "type": "SCALAR",
        "scalar": {
          "value": 2
        }
      },
      {
        "name": "mem",
        "type": "SCALAR",
        "scalar": {
          "value": 24576
        }
      },
      {
        "name": "disk",
        "type": "SCALAR",
        "scalar": {
          "value": 409600
        }
      },
      {
        "name": "ports",
        "type": "RANGES",
        "ranges": {
          "range": [
            {
              "begin": 21000,
              "end": 24000
            },
            {
              "begin": 30000,
              "end": 34000
            }
          ]
        }
      },
      {
        "name": "bugs",
        "type": "SET",
        "set": {
          "item": [
            "a",
            "b",
            "c"
          ]
        },
        "role": "debug_role"
      }
    ]

You can do:

    $ path/to/mesos-agent --resources=file:///path/to/resources.txt ...

In this case, we have five resources of three different types: scalars, a range, and a set.  There are scalars called `cpus`, `gpus`, `mem` and `disk`, a range called `ports`, and a set called `bugs`. `bugs` is assigned to the role `debug_role`, while the other resources do not specify a role and are thus assigned to the default role.

Note: the "default role" can be set by the `--default_role` flag.

  - scalar called `cpus`, with the value `24`
  - scalar called `gpus`, with the value `2`
  - scalar called `mem`, with the value `24576`
  - scalar called `disk`, with the value `409600`
  - range called `ports`, with values `21000` through `24000` and `30000` through `34000` (inclusive)
  - set called `bugs`, with the values `a`, `b` and `c`, assigned to the role `debug_role`

To configure the attributes of a Mesos agent, you can use the `--attributes` command-line flag of `mesos-agent`:

    --attributes='rack:abc;zone:west;os:centos5;level:10;keys:[1000-1500]'

That will result in configuring the following five attributes:

  - `rack` with text value `abc`
  - `zone` with text value `west`
  - `os` with text value `centos5`
  - `level` with scalar value 10
  - `keys` with range value `1000` through `1500` (inclusive)
