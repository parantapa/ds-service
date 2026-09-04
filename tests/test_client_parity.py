"""Tests that the async client mirrors the synchronous one.

The two clients are separate classes on purpose --
grpc's blocking and asyncio channels are different objects,
and a caller wants `map_get` to return `bytes` or an awaitable,
never one pretending to be the other --
but that leaves each RPC written out twice.
Nothing except these tests stops one class
from gaining a method, or changing an argument,
that the other never hears about.
"""

import inspect
from typing import Any, Callable

import pytest

from ds_service_client import DsServiceClient, DsServiceClientAsync


def public_methods(cls: type) -> dict[str, Callable[..., Any]]:
    """The methods a caller uses, keyed by name.

    Dunders are left out:
    the context manager protocol is spelled differently
    on each side, and is checked on its own below.
    """
    return {
        name: member
        for name, member in inspect.getmembers(cls, inspect.isfunction)
        if not name.startswith("_")
    }


SYNC_METHODS = public_methods(DsServiceClient)
ASYNC_METHODS = public_methods(DsServiceClientAsync)

# Only the methods both classes have.
# A method missing from one of them
# is reported by test_the_two_clients_offer_the_same_methods,
# rather than as a KeyError from every other test here.
SHARED_METHOD_NAMES = sorted(SYNC_METHODS.keys() & ASYNC_METHODS.keys())


def parameters(method: Callable[..., Any]) -> list[inspect.Parameter]:
    """The parameters of a method, without `self`."""
    return list(inspect.signature(method).parameters.values())[1:]


def test_the_two_clients_offer_the_same_methods():
    assert sorted(SYNC_METHODS) == sorted(ASYNC_METHODS)


@pytest.mark.parametrize("name", SHARED_METHOD_NAMES)
def test_shared_methods_take_the_same_arguments(name: str):
    """Parameter names, order, kinds, defaults and annotations must match.

    Comparing Parameter objects covers all five at once.
    """
    assert parameters(SYNC_METHODS[name]) == parameters(ASYNC_METHODS[name])


@pytest.mark.parametrize("name", SHARED_METHOD_NAMES)
def test_shared_methods_return_the_same_type(name: str):
    """An async method is annotated with what awaiting it yields,
    so the two annotations should read the same.

    Only checked where both are annotated,
    because an un-annotated method is not a mismatch to fix here.
    """
    sync_return = inspect.signature(SYNC_METHODS[name]).return_annotation
    async_return = inspect.signature(ASYNC_METHODS[name]).return_annotation
    if inspect.Signature.empty in (sync_return, async_return):
        pytest.skip(f"{name} is not annotated on both clients.")

    assert sync_return == async_return


@pytest.mark.parametrize("name", SHARED_METHOD_NAMES)
def test_only_the_async_client_has_coroutine_methods(name: str):
    assert inspect.iscoroutinefunction(ASYNC_METHODS[name])
    assert not inspect.iscoroutinefunction(SYNC_METHODS[name])


def test_each_client_supports_its_own_context_manager_protocol():
    assert callable(getattr(DsServiceClient, "__enter__"))
    assert callable(getattr(DsServiceClient, "__exit__"))

    assert inspect.iscoroutinefunction(DsServiceClientAsync.__aenter__)
    assert inspect.iscoroutinefunction(DsServiceClientAsync.__aexit__)
