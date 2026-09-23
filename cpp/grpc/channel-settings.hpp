#pragma once

// The gRPC channel settings.
//
// Some of these are only correct as a matched pair between the server and its clients.
// See "The channel settings are one setting in two languages"
// in docs/developer-notes.md.
// python/ds_service_client/client.py still holds its own copy of the client side,
// and tests/test_grpc_options.py compares the two.

namespace ds::grpc_settings {

// Largest single request or response accepted.
// gRPC's default is 4 MiB.
// client.py holds the same value.
constexpr int MAX_MESSAGE_SIZE_BYTES = 64 * 1024 * 1024;

// How often the server pings an idle client, and how long it waits for the answer.
constexpr int SERVER_KEEPALIVE_TIME_MS = 10 * 60 * 1000;
constexpr int SERVER_KEEPALIVE_TIMEOUT_MS = 20 * 1000;

// A floor on how often a client can ping, which is different in kind from the two above.
// The client's keepalive interval must stay above it.
// Below it, the server answers pings with GOAWAY and drops the connection,
// which the caller sees as an unavailable server with no mention of pings.
constexpr int SERVER_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS = 10 * 1000;

// How often a client pings an idle server, and how long it waits for the answer.
// client.py holds the same values.
constexpr int CLIENT_KEEPALIVE_TIME_MS = 120 * 1000;
constexpr int CLIENT_KEEPALIVE_TIMEOUT_MS = 30 * 1000;

static_assert(CLIENT_KEEPALIVE_TIME_MS > SERVER_MIN_RECV_PING_INTERVAL_WITHOUT_DATA_MS,
              "the server would drop a client that pings this often");

} // namespace ds::grpc_settings
