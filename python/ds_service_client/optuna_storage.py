"""Optuna storage backend backed by ds-service."""

import json
from typing import Any, Iterable, Union

from optuna.storages.journal import BaseJournalBackend, JournalStorage

from .client import Client

# BaseJournalSnapshot lives in a private module
# and is not re-exported from optuna.storages.journal.
# Snapshot support is optional, so degrade gracefully
# if this import ever breaks on a future optuna.
try:
    from optuna.storages.journal._base import BaseJournalSnapshot
except Exception:  # pragma: no cover - depends on optuna internals
    BaseJournalSnapshot = None

# journal_read clamps `end` to the journal size
# (atomically, under the server lock),
# so an out-of-range upper bound reads "everything from `start` onward".
_READ_TO_END = 2**64 - 1


class DsServiceJournalBackend(BaseJournalBackend):
    """An Optuna journal backend that stores its log in a ds-service journal."""

    def __init__(self, client: Client, name: str = "optuna"):
        self._client = client
        self._log_key = f"optuna:{name}:journal"
        self._snapshot_key = f"optuna:{name}:snapshot"

    def append_logs(self, logs: list[dict[str, Any]]) -> None:
        for log in logs:
            entry = json.dumps(log, separators=(",", ":")).encode("utf-8")
            self._client.journal_append(self._log_key, entry)

    def read_logs(self, log_number_from: int) -> Iterable[dict[str, Any]]:
        entries = self._client.journal_read(
            self._log_key, log_number_from, _READ_TO_END
        )
        for entry in entries:
            yield json.loads(entry)

    def save_snapshot(self, snapshot: bytes) -> None:
        self._client.map_set(self._snapshot_key, snapshot)

    def load_snapshot(self) -> Union[bytes, None]:
        try:
            return self._client.map_get(self._snapshot_key)
        except KeyError:
            return None


if BaseJournalSnapshot is not None:
    BaseJournalSnapshot.register(DsServiceJournalBackend)


def create_journal_storage(
    client: Union[Client, str, None] = None,
    name: str = "optuna",
) -> JournalStorage:
    """Build an Optuna compatiable Journal Storage living on ds-service."""
    if not isinstance(client, Client):
        client = Client(client)
    return JournalStorage(DsServiceJournalBackend(client, name=name))
