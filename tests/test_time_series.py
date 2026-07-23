"""Tests for the time series store."""

import pytest


def _points(client, key, **kwargs):
    """Return (value, step) pairs for a time_series_get, for terse assertions."""
    return [(p.value, p.step) for p in client.time_series_get(key, **kwargs)]


def test_append_then_get_returns_in_append_order(client):
    client.time_series_append("m", 1.0, "2024-01-01T00:00:03Z")
    client.time_series_append("m", 2.0, "2024-01-01T00:00:01Z")
    client.time_series_append("m", 3.0, "2024-01-01T00:00:02Z")

    # Append order is preserved, not sorted by time.
    assert [p.value for p in client.time_series_get("m")] == [1.0, 2.0, 3.0]


def test_get_missing_key_returns_empty(client):
    assert client.time_series_get("does-not-exist") == []


def test_step_defaults_to_zero(client):
    client.time_series_append("m", 1.5, "2024-01-01T00:00:00Z")
    (point,) = client.time_series_get("m")
    assert point.step == 0
    assert point.value == 1.5


def test_step_is_stored(client):
    client.time_series_append("m", 1.0, "2024-01-01T00:00:00Z", step=7)
    (point,) = client.time_series_get("m")
    assert point.step == 7


def test_datetime_round_trips(client):
    client.time_series_append("m", 1.0, "2024-01-02T03:04:05Z")
    (point,) = client.time_series_get("m")
    assert point.datetime == "2024-01-02T03:04:05Z"


def test_datetime_python_isoformat_offset(client):
    # datetime.now(timezone.utc).isoformat() emits "+00:00", not "Z".
    client.time_series_append("m", 1.0, "2024-01-02T03:04:05.123456+00:00")
    (point,) = client.time_series_get("m")
    assert point.datetime == "2024-01-02T03:04:05.123456Z"


def test_non_utc_offset_is_normalized(client):
    client.time_series_append("m", 1.0, "2024-01-02T09:00:00+05:30")
    (point,) = client.time_series_get("m")
    assert point.datetime == "2024-01-02T03:30:00Z"


def test_invalid_datetime_on_append_raises(client):
    with pytest.raises(ValueError):
        client.time_series_append("m", 1.0, "not-a-datetime")


def test_invalid_datetime_leaves_series_usable(client):
    client.time_series_append("m", 1.0, "2024-01-01T00:00:00Z")
    with pytest.raises(ValueError):
        client.time_series_append("m", 2.0, "garbage")
    assert [p.value for p in client.time_series_get("m")] == [1.0]


@pytest.fixture
def stepped_series(client):
    # value == step, times one second apart, for readable filter assertions.
    for i in range(5):
        client.time_series_append("m", float(i), f"2024-01-01T00:00:0{i}Z", step=i)
    return client


def test_start_time_is_inclusive(stepped_series):
    got = _points(stepped_series, "m", start_time="2024-01-01T00:00:02Z")
    assert got == [(2.0, 2), (3.0, 3), (4.0, 4)]


def test_end_time_is_exclusive(stepped_series):
    got = _points(stepped_series, "m", end_time="2024-01-01T00:00:02Z")
    assert got == [(0.0, 0), (1.0, 1)]


def test_time_window_start_inclusive_end_exclusive(stepped_series):
    got = _points(
        stepped_series,
        "m",
        start_time="2024-01-01T00:00:01Z",
        end_time="2024-01-01T00:00:03Z",
    )
    assert got == [(1.0, 1), (2.0, 2)]


def test_start_step_is_inclusive(stepped_series):
    got = _points(stepped_series, "m", start_step=3)
    assert got == [(3.0, 3), (4.0, 4)]


def test_end_step_is_exclusive(stepped_series):
    got = _points(stepped_series, "m", end_step=2)
    assert got == [(0.0, 0), (1.0, 1)]


def test_step_window(stepped_series):
    got = _points(stepped_series, "m", start_step=1, end_step=4)
    assert got == [(1.0, 1), (2.0, 2), (3.0, 3)]


def test_time_and_step_filters_combine(stepped_series):
    # start_time keeps steps 2..4; end_step keeps 0..2; intersection is step 2.
    got = _points(
        stepped_series,
        "m",
        start_time="2024-01-01T00:00:02Z",
        end_step=3,
    )
    assert got == [(2.0, 2)]


def test_start_step_zero_is_a_real_bound(stepped_series):
    # step=0 must be treated as "provided", not "absent" — here it admits everything.
    got = _points(stepped_series, "m", start_step=0)
    assert [s for _, s in got] == [0, 1, 2, 3, 4]


def test_empty_window_returns_empty(stepped_series):
    got = _points(stepped_series, "m", start_step=10)
    assert got == []


def test_invalid_start_time_on_get_raises(stepped_series):
    with pytest.raises(ValueError):
        stepped_series.time_series_get("m", start_time="nonsense")
