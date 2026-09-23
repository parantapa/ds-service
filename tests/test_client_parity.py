"""Tests that the async client mirrors the synchronous one.

See "Two clients, one API" in docs/developer-notes.md
for why the two classes are separate.
"""

import inspect
from typing import Any, Callable

import pytest

from ds_service_client import DsServiceClient, DsServiceClientAsync


def public_methods(cls: type) -> dict[str, Callable[..., Any]]:
    """The methods a caller uses, keyed by name.

    This leaves out every name that starts with an underscore,
    the dunder methods included.
    """
    # Each side spells the context manager protocol differently,
    # so test_client_lifecycle.py
    # and test_async_client_supports_the_async_context_manager_protocol
    # check it on their own.
    return {
        name: member
        for name, member in inspect.getmembers(cls, inspect.isfunction)
        if not name.startswith("_")
    }


SYNC_METHODS = public_methods(DsServiceClient)
ASYNC_METHODS = public_methods(DsServiceClientAsync)

# Only the methods both classes have.
# test_the_two_clients_offer_the_same_methods reports a method
# missing from one of them,
# rather than a KeyError from every other test here.
SHARED_METHOD_NAMES = sorted(SYNC_METHODS.keys() & ASYNC_METHODS.keys())


def parameters(method: Callable[..., Any]) -> list[inspect.Parameter]:
    """The parameters of a method, without `self`."""
    return list(inspect.signature(method).parameters.values())[1:]


def test_the_two_clients_offer_the_same_methods():
    assert sorted(SYNC_METHODS) == sorted(ASYNC_METHODS)


@pytest.mark.parametrize("name", SHARED_METHOD_NAMES)
def test_shared_methods_take_the_same_arguments(name: str):
    # One comparison of Parameter lists covers
    # names, order, kinds, defaults and annotations.
    assert parameters(SYNC_METHODS[name]) == parameters(ASYNC_METHODS[name])


@pytest.mark.parametrize("name", SHARED_METHOD_NAMES)
def test_shared_methods_return_the_same_type(name: str):
    # An async method annotates the type that an await returns,
    # so the two annotations must read the same.
    sync_return = inspect.signature(SYNC_METHODS[name]).return_annotation
    async_return = inspect.signature(ASYNC_METHODS[name]).return_annotation
    # An un-annotated method is not a mismatch to fix here.
    if inspect.Signature.empty in (sync_return, async_return):
        pytest.skip(f"{name} is not annotated on both clients.")

    assert sync_return == async_return


@pytest.mark.parametrize("name", SHARED_METHOD_NAMES)
def test_only_the_async_client_has_coroutine_methods(name: str):
    assert inspect.iscoroutinefunction(ASYNC_METHODS[name])
    assert not inspect.iscoroutinefunction(SYNC_METHODS[name])


def test_async_client_supports_the_async_context_manager_protocol():
    # test_client_lifecycle.py uses both protocols against a server.
    assert inspect.iscoroutinefunction(DsServiceClientAsync.__aenter__)
    assert inspect.iscoroutinefunction(DsServiceClientAsync.__aexit__)
