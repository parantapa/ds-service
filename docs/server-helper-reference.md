# Server helper reference

`DsServiceServer` runs a private `ds-service` process
for as long as the object lives.
It is importable from `ds_service_client` and implemented in
`python/ds_service_client/server.py`.

```python
from ds_service_client import DsServiceClient, DsServiceServer

with DsServiceServer("lo") as server:
    server.wait_until_ready()          # blocks until the port accepts connections

    with DsServiceClient(server.address) as client:
        client.map_set("greeting", b"hello")
```

## Constructor

The process starts as soon as the object is constructed.

| Argument | Default | Meaning |
| --- | --- | --- |
| `interface` | required | Network interface whose IPv4 address the server binds. |
| `port` | a free ephemeral port | Port the server binds; `0` means the same as leaving it out. |
| `ds_service_bin` | `$DS_SERVICE_BIN`, else `ds-service` | How to start the server. |

An interface that does not exist on this machine,
or that exists with no IPv4 address on it,
raises `ValueError` from the constructor,
before any server process is started.

`ds_service_bin` and `DS_SERVICE_BIN` may hold a **whole command**,
not just a path -- `docker run --rm --network host ds-service`
works as well as `/usr/bin/ds-service`.
`--address <host>:<port>` is appended to whatever is given,
and the result is split with `shlex.split`:
quoting is understood, but shell syntax is not.

## Addressing

The server is never bound to a wildcard address.
The interface decides who can reach it:
`lo` for this machine only,
`eth0` or `ib0` for other machines on that network.

| Attribute | Value |
| --- | --- |
| `server.address` | `<ip of the interface>:<port>`, what every client connects to, local or remote. |
| `server.host` | The resolved address on its own. |

```python
server = DsServiceServer("eth0")
server.address   # -> "172.17.0.2:45999", the address to hand to remote clients
                 # -> the InfiniBand address, had this been "ib0" on a cluster node
```

## `wait_until_ready(timeout=30)`

Polls until the port accepts a TCP connection.
Raises `RuntimeError` if the process exits first,
and `TimeoutError` if the server is not listening within `timeout` seconds.
It only reports a server ready while our own process is still running.

## `close()`

Sends `SIGTERM`, waits for the grace period set by
`TERMINATE_TIMEOUT_S` in `python/ds_service_client/server.py`,
and then sends `SIGKILL`.
Leaving a `with` block calls it.
Call it directly when not using the object as a context manager.
