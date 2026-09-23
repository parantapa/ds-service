# About the architecture

How `ds-service` is put together,
and why it has that shape.

For what each operation does, see
the [data structure reference](../reference/data-structure.md).

## The pieces

- **Common types** (`cpp/common/`): plain C++ structs,
    one per message the server and its clients exchange,
    with `TaskState` and the error codes.
    Both sides use them, and neither depends on a transport.
- **Server** (`cpp/server/`): a C++23 server.
    All state lives in memory.
    Each top-level data structure is a struct
    that carries the lock that guards it.
    `cpp/server/core/data-structures.hpp` declares each struct,
    and a source file of its own implements it.
    The server core takes plain requests and returns plain responses,
    and knows nothing of any transport.
- **Transports**: the code that carries requests between the two sides.
    Each transport lives in a directory of its own, `cpp/grpc/` for gRPC.
    A transport's server half decodes each request, hands it to the core,
    and encodes the answer.
    Its client half does the reverse.
- **C++ client** (`cpp/client/`): `ds::Client`,
    one method per operation, over whatever transport `ds::connect()` picks.
- **Python client** (`python/ds_service_client/`): a Python 3.12+ library
    over the C++ client, which nanobind binds as `ds_service_client._ext`.
    It translates failed calls into Python exceptions.
    It ships a blocking client, `DsServiceClient`,
    and an asyncio one, `DsServiceClientAsync`.
    Both offer the same methods and raise the same exceptions.
    The package also ships `DsServiceServer`,
    which starts a private server process
    and stops it on `close()` or at the end of a `with` block.

The plain types in `cpp/common/` and the behavior of the server core
define the system.
gRPC is the transport in use today,
and more transports may be added later.
`cpp/grpc/ds-service.proto` is only the gRPC transport's encoding
of the plain types,
and a codec under `cpp/grpc/` converts between the two.
So a new transport needs its own encoding of the same plain types,
and neither the plain types, the server core nor `ds::Client` changes.

The Python client carries its own copy of the C++ client and its transport.
It needs neither `grpcio` nor `protobuf`,
and its behavior matches the C++ client call for call.

## The server does not persist state

When the server stops, all data is lost.
There is no snapshot, no log, and no recovery.

This is a deliberate trade.
`ds-service` coordinates distributed clients and workers
that are themselves transient:

- A batch of workers on a cluster.
- A training run reporting metrics.
- A set of processes handing blobs to one another.

For that, the state is only interesting while the run is.
Durability buys correctness guarantees nobody asked for,
at the cost of a write path on every operation.

The consequence is that the server is a coordination point,
not a database.
Anything that must survive the run belongs somewhere else.

## One lock per structure

Each top-level data structure has its own lock,
and an operation takes at most one of them.
Each operation touches a single structure,
so no request ever holds more than one lock.
Two properties follow.

First, operations on different structures do not contend:
a slow `map_search_key` does not delay a `counter_get_next_value`.
Second, the lock fully serializes operations on the same structure,
which is what makes the counters gap-free
and the mutexes meaningful.

One lock per structure also means
that a single expensive call blocks its whole structure.
The search_key operations walk every key under the structure's lock.
So a search over a large key space blocks every other operation
on that structure until it finishes.

## Two Python clients, one API

`DsServiceClient` and `DsServiceClientAsync` are separate classes
rather than one class with two modes.
A caller wants `map_get` to return either `bytes` or an awaitable,
never one dressed as the other.

Both call the same blocking C++ client.
The async client runs each call on a thread of its own pool,
and the event loop goes on with other work while the call waits.
The C++ client releases the GIL while the call runs,
and takes it back every 100 ms only to check for a signal such as Ctrl-C,
so the threads do not hold each other up.
So there is one call path per transport,
and the asyncio support costs no second implementation of any transport.
The price is a cap on concurrent calls,
which is the size of the pool.

The cost of two classes is that every method appears twice.
The project pays that cost deliberately,
and a test defends it, rather than discipline.
See the [developer notes](../developer-notes.md#two-clients-one-api)
for how the two copies are kept in step.
