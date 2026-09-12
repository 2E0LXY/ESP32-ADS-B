"""A source that cannot succeed should not be presented as a live one.

airplanes.live began answering 403 to every request, from this deployment's
address and from unrelated networks alike. Left in the list it sat
permanently red in the admin panel - implying an outage somebody could fix -
and kept spending requests at someone else's server to re-learn the same
answer every backoff interval.
"""

import httpx
import pytest

from app import aggregator as aggregator_module
from app.aggregator import Aggregator
from app.database import SessionLocal


class _RecordingClient:
    """Stands in for httpx.AsyncClient, remembering what was asked for."""

    def __init__(self):
        self.urls: list[str] = []

    async def get(self, url):
        self.urls.append(url)
        request = httpx.Request("GET", url)
        return httpx.Response(200, json={"ac": []}, request=request)


@pytest.fixture()
def only_adsbfi(monkeypatch):
    monkeypatch.setattr(aggregator_module, "DISABLED_SOURCES", {"airplaneslive", "adsblol"})
    return Aggregator(53.73, -1.57, 50, SessionLocal)


async def test_a_disabled_source_is_never_asked(only_adsbfi, client):
    recording = _RecordingClient()
    await only_adsbfi._poll_all(recording)

    hosts = {url.split("/")[2] for url in recording.urls}
    assert hosts == {"opendata.adsb.fi"}, f"asked a disabled source: {hosts}"


async def test_a_disabled_source_is_not_reported_as_unhealthy(only_adsbfi):
    # Absent from the panel entirely, rather than present and red: the
    # admin template renders whatever health() returns.
    assert set(only_adsbfi.health()) == {"adsbfi"}


async def test_recording_a_disabled_source_is_harmless(only_adsbfi):
    """Nothing should call _record for a disabled source, but a future
    caller that does must not raise a KeyError deep in the poll loop."""

    async def never_awaited():
        raise AssertionError("a disabled source was fetched")

    await only_adsbfi._record("airplaneslive", never_awaited())
    assert "airplaneslive" not in only_adsbfi.health()


def test_the_default_disables_airplaneslive():
    """The deployed default, not just whatever a test monkeypatched."""
    assert "airplaneslive" in aggregator_module.DISABLED_SOURCES
    assert "adsbfi" not in aggregator_module.DISABLED_SOURCES
    assert "adsblol" not in aggregator_module.DISABLED_SOURCES
