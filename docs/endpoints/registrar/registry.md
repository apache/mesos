<!--- This is an automatically generated file. DO NOT EDIT! --->

### USAGE ###
>        /registrar(1)/registry

### TL;DR; ###
Returns the current contents of the Registry in JSON.

### DESCRIPTION ###
Example:

```
{
  "master":
  {
    "info":
    {
      "hostname": "localhost",
      "id": "20140325-235542-1740121354-5050-33357",
      "ip": 2130706433,
      "pid": "master@127.0.0.1:5050",
      "port": 5050
    }
  },

  "slaves":
  {
    "slaves":
    [
      {
        "info":
        {
          "checkpoint": true,
          "hostname": "localhost",
          "id":
          {
            "value": "20140325-234618-1740121354-5050-29065-0"
          },
          "port": 5051,
          "resources":
          [
            {
              "name": "cpus",
              "role": "*",
              "scalar": { "value": 24 },
              "type": "SCALAR"
            }
          ],
        }
      }
    ]
  }
}
```