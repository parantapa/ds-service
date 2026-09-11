# Server helper reference

`DsServiceServer` runs a private `ds-service` process
for as long as the object lives.
Import `DsServiceServer` from `ds_service_client`.
`python/ds_service_client/server.py` holds the implementation.

```python
from ds_service_client import DsServiceClient, DsServiceServer

with DsServiceServer("lo") as server:
    server.wait_until_ready()          # blocks until the port accepts connections

    with DsServiceClient(server.address) as client:
        client.map_set("greeting", b"hello")
```

## Constructor

The process starts as soon as you construct the object.

| Argument | Default | Meaning |
| --- | --- | --- |
| `interface` | required | Network interface whose IPv4 address the server binds. |
| `port` | a free ephemeral port | Port the server binds. `0` means the same as leaving it out. |
| `ds_service_bin` | `$DS_SERVICE_BIN`, else `ds-service` | How to start the server. |

The constructor raises `ValueError` for an interface
that does not exist on this machine,
or that exists with no IPv4 address on it.
The constructor raises it before it starts any server process.

The constructor raises `OSError` for an explicitly given port
that is already in use.

`ds_service_bin` and `DS_SERVICE_BIN` can hold a whole command,
not only a path.
`docker run --rm --network host ds-service` works as well as `/usr/bin/ds-service`.
`DsServiceServer` appends `--address <host>:<port>` to that command.
Then it splits the result with `shlex.split`.
`shlex.split` understands quoting, but it does not understand shell syntax.

## Addressing

The server never binds a wildcard address.
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
`wait_until_ready` reports the server ready
only while the process it started still runs.

## `close()`

Sends `SIGTERM`, waits for the grace period set by
`TERMINATE_TIMEOUT_S` in `python/ds_service_client/server.py`,
and then sends `SIGKILL`.
The exit of a `with` block calls `close()`.
Call `close()` directly where the object is not a context manager.
