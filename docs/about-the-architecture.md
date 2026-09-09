# About the architecture

How `ds-service` is put together,
and why it is shaped the way it is.

For what each RPC does, see the
[data structure reference](data-structure-reference.md).

## Three pieces

- **Server** (`cpp/ds-service.cpp`) -- a C++23 gRPC service.
    All state lives in memory,
    with a separate lock guarding each top-level data structure.
    Operations on one structure are serialized,
    while operations on different structures may run concurrently.
    Each RPC touches a single structure,
    so no request ever holds more than one lock.
- **Client** (`python/ds_service_client/`) -- a Python 3.12+ client library
    that wraps the generated gRPC stubs
    and translates gRPC status codes into Python exceptions.
    It ships a blocking client, `DsServiceClient`,
    and an asyncio one, `DsServiceClientAsync`,
    offering the same methods and raising the same exceptions.
- **Interface** (`misc/ds-service.proto`) -- the protobuf/gRPC contract
    shared by both sides.

The proto file is the authoritative definition of the wire format.
Both sides are generated from it,
which is why a change there is felt in both languages at once.

## State is not persisted

When the server stops, all data is lost.
There is no snapshot, no log, and no recovery.

This is a deliberate trade.
The intended use is coordination between distributed clients and workers
that are themselves transient:
a batch of workers on a cluster,
a training run reporting metrics,
a set of processes handing blobs to one another.
For that, the state is only interesting while the run is,
and durability would buy correctness guarantees nobody was asking for
at the cost of a write path on every operation.

The consequence is that the server is a coordination point,
not a database.
Anything that must survive the run has to be written somewhere else.

## One lock per structure

Each top-level data structure has its own lock,
and an RPC takes exactly one of them.
Two properties follow.

Operations on different structures do not contend:
a slow `MapSearchKey` does not delay a `CounterGetNextValue`.
Operations on the same structure are fully serialized,
which is what makes the counters gap-free
and the mutexes meaningful.

It also means a single expensive call blocks its whole structure.
The `SearchKey` family walks every key under the structure's lock,
so a search over a large key space
holds off every other operation on that structure until it finishes.

## Two Python clients, one API

`DsServiceClient` and `DsServiceClientAsync` are separate classes
rather than one class with two modes,
because grpc's blocking and asyncio channels are different objects,
and a caller wants `map_get` to return either `bytes` or an awaitable,
never one dressed as the other.

The cost is that every RPC is written out twice.
That cost is paid deliberately, and it is defended by a test rather than
by discipline; see the developer notes for how the two copies are kept in step.
