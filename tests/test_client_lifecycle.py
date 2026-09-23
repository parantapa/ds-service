"""Tests for connecting and disconnecting both clients."""

import asyncio
import socket
import time

import pytest

from ds_service_client import DsServiceClient, DsServiceClientAsync


def test_context_manager_closes_the_client(server):
    with DsServiceClient(server) as client:
        client.map_set("k", b"v")
        assert client.map_get("k") == b"v"

    # A closed client refuses calls rather than quietly reconnecting,
    # so this is how the test observes the close.
    with pytest.raises(RuntimeError, match="closed"):
        client.map_get("k")


def test_exception_in_the_block_still_closes_the_client(server):
    # The test binds the client before the with statement,
    # so it can still reach it after the block raises.
    client = DsServiceClient(server)

    with pytest.raises(ZeroDivisionError):
        with client:
            raise ZeroDivisionError

    with pytest.raises(RuntimeError, match="closed"):
        client.map_get("k")


def test_close_is_still_callable_directly(server):
    client = DsServiceClient(server)
    client.map_set("k", b"v")
    client.close()

    with pytest.raises(RuntimeError, match="closed"):
        client.map_get("k")

    # A second close does nothing.
    client.close()


def test_close_cancels_a_call_in_flight():
    # A listening socket that nothing accepts from never answers a call.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        sock.listen(8)
        address = "127.0.0.1:%d" % sock.getsockname()[1]

        async def main() -> float:
            client = DsServiceClientAsync(address, timeout=60.0)
            call = asyncio.ensure_future(client.map_get("k"))
            # Let the call get in flight,
            # so close() cancels it rather than refusing it at the start.
            await asyncio.sleep(0.3)
            start = time.monotonic()
            await client.close()
            with pytest.raises(RuntimeError, match="closed"):
                await call
            return time.monotonic() - start

        assert asyncio.run(main()) < 5.0


def test_async_context_manager_closes_the_client(server):
    async def main() -> None:
        async with DsServiceClientAsync(server) as client:
            await client.map_set("k", b"v")
            assert await client.map_get("k") == b"v"

        with pytest.raises(RuntimeError, match="closed"):
            await client.map_get("k")

        # A second close does nothing.
        await client.close()

    asyncio.run(main())


def test_async_client_needs_no_running_event_loop(server):
    # The client binds to no event loop,
    # so it can be made outside one and used inside one.
    client = DsServiceClientAsync(server)

    async def main() -> bytes:
        await client.map_set("k", b"v")
        value = await client.map_get("k")
        await client.close()
        return value

    assert asyncio.run(main()) == b"v"


def test_async_calls_run_concurrently_without_blocking_the_loop():
    # Eight calls to a server that never answers, each with a 0.5 s deadline,
    # finish in about 0.5 s together rather than 4 s one after another,
    # and a ticker on the same loop keeps running all the while.
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind(("127.0.0.1", 0))
        sock.listen(8)
        address = "127.0.0.1:%d" % sock.getsockname()[1]

        async def main() -> tuple[float, int]:
            client = DsServiceClientAsync(address, timeout=0.5, max_workers=8)
            ticks = 0
            done = False

            async def ticker() -> None:
                nonlocal ticks
                while not done:
                    ticks += 1
                    await asyncio.sleep(0.01)

            async def one_call() -> None:
                with pytest.raises(TimeoutError):
                    await client.map_get("k")

            tick_task = asyncio.ensure_future(ticker())
            start = time.monotonic()
            await asyncio.gather(*(one_call() for _ in range(8)))
            elapsed = time.monotonic() - start
            done = True
            await tick_task
            await client.close()
            return elapsed, ticks

        elapsed, ticks = asyncio.run(main())
        # Both bounds leave a wide margin.
        # A serial run takes 4 s,
        # and a ticker that never blocks ticks about 50 times in 0.5 s.
        assert elapsed < 2.5
        assert ticks > 20
