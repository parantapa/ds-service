"""Tests for the Optuna storage backend built on ds-service journals."""

import pytest

optuna = pytest.importorskip("optuna")

from ds_service_client import Client
from ds_service_client.optuna_storage import (
    DsServiceJournalBackend,
    create_journal_storage,
)


def _objective(trial):
    x = trial.suggest_float("x", -10.0, 10.0)
    return (x - 3.0) ** 2


def test_optimize_records_all_trials(server):
    optuna.logging.set_verbosity(optuna.logging.WARNING)
    storage = create_journal_storage(server, name="study-a")

    study = optuna.create_study(storage=storage, direction="minimize")
    study.optimize(_objective, n_trials=15)

    assert len(study.trials) == 15
    assert all(t.state == optuna.trial.TrialState.COMPLETE for t in study.trials)
    # The minimum of (x - 3)^2 is 0; a short search should get reasonably close.
    assert study.best_value < 5.0


def test_study_state_is_replayed_by_a_fresh_backend(server):
    optuna.logging.set_verbosity(optuna.logging.WARNING)

    # Writer populates the journal.
    writer_storage = create_journal_storage(server, name="shared")
    study = optuna.create_study(
        study_name="shared", storage=writer_storage, direction="minimize"
    )
    study.optimize(_objective, n_trials=10)

    # A brand-new backend/client reconstructs the same study purely by replaying
    # the journal (read_logs) — nothing is kept in process memory.
    reader_storage = create_journal_storage(server, name="shared")
    loaded = optuna.load_study(study_name="shared", storage=reader_storage)

    assert len(loaded.trials) == 10
    assert loaded.best_value == study.best_value
    assert loaded.best_params == study.best_params


def test_backend_uses_snapshot_when_available(server):
    # Our backend advertises snapshot support; exercise save/load directly.
    client = Client(server)
    backend = DsServiceJournalBackend(client, name="snap")

    assert backend.load_snapshot() is None
    backend.save_snapshot(b"snapshot-bytes")
    assert backend.load_snapshot() == b"snapshot-bytes"
    client.close()


def test_distinct_names_are_isolated(server):
    optuna.logging.set_verbosity(optuna.logging.WARNING)

    study_a = optuna.create_study(
        storage=create_journal_storage(server, name="a"), study_name="a"
    )
    study_a.optimize(_objective, n_trials=3)

    # A different name is a different journal, so this study starts empty.
    study_b = optuna.create_study(
        storage=create_journal_storage(server, name="b"), study_name="b"
    )
    assert len(study_b.trials) == 0
