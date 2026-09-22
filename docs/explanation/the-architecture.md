# About the architecture

How `ds-service` is put together,
and why it has that shape.

For what each RPC does, see
the [data structure reference](../reference/data-structure.md).

## Three pieces

- **Server** (`cpp/`): a C++23 gRPC service.
    All state lives in memory.
    Each top-level data structure is a struct
    that carries the lock that guards it.
    `cpp/ds-service.hpp` declares each struct,
    and a source file of its own implements it.
- **Client** (`python/ds_service_client/`): a Python 3.12+ client library
    that wraps the generated gRPC stubs
    and translates gRPC status codes into Python exceptions.
    It ships a blocking client, `DsServiceClient`,
    and an asyncio one, `DsServiceClientAsync`.
    Both offer the same methods and raise the same exceptions.
    The package also ships `DsServiceServer`,
    which starts a private server process
    and stops it on `close()` or at the end of a `with` block.
- **Interface** (`misc/ds-service.proto`): the protobuf/gRPC contract
    shared by both sides.

The proto file is the authoritative definition of the wire format.
The gRPC code of both sides comes from it.
A change there therefore changes both languages at once.

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
and an RPC takes at most one of them.
Each RPC touches a single structure,
so no request ever holds more than one lock.
Two properties follow.

First, operations on different structures do not contend:
a slow `MapSearchKey` does not delay a `CounterGetNextValue`.
Second, the lock fully serializes operations on the same structure,
which is what makes the counters gap-free
and the mutexes meaningful.

One lock per structure also means
that a single expensive call blocks its whole structure.
The `SearchKey` family walks every key under the structure's lock.
So a search over a large key space blocks every other operation
on that structure until it finishes.

## Two Python clients, one API

`DsServiceClient` and `DsServiceClientAsync` are separate classes
rather than one class with two modes.
The reason is that gRPC's blocking and asyncio channels are different objects.
A caller also wants `map_get` to return either `bytes` or an awaitable,
never one dressed as the other.

The cost is that every RPC appears twice.
The project pays that cost deliberately,
and a test defends it, rather than discipline.
See the [developer notes](../developer-notes.md#two-clients-one-api)
for how the two copies are kept in step.
