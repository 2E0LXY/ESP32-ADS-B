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
from app.runtime_settings import SettingsStore


class _RecordingClient:
    """Stands in for httpx.AsyncClient, remembering what was asked for."""

    def __init__(self):
        self.urls: list[str] = []

    async def get(self, url):
        self.urls.append(url)
        request = httpx.Request("GET", url)
        return httpx.Response(200, json={"ac": []}, request=request)


@pytest.fixture()
def only_adsbfi():
    """Which sources are polled is an admin-panel setting now, not an
    environment variable read at import - so this sets it the way the
    panel does."""
    settings = SettingsStore()
    settings._values["source_airplaneslive"] = False
    settings._values["source_adsblol"] = False
    return Aggregator(53.73, -1.57, 50, SessionLocal, settings=settings)


async def test_a_disabled_source_is_never_asked(only_adsbfi, client):
    recording = _RecordingClient()
    await only_adsbfi._poll_all(recording)

    hosts = {url.split("/")[2] for url in recording.urls}
    assert hosts == {"opendata.adsb.fi"}, f"asked a disabled source: {hosts}"


async def test_a_disabled_source_is_listed_but_not_treated_as_failing(only_adsbfi):
    """Listed, and shown as off.

    It used to be dropped from health() entirely, which is what stopped it
    sitting permanently red. Now that sources are switched from the admin
    panel, omitting them is wrong the other way: an operator cannot turn
    something back on if the panel does not admit it exists, and a source
    switched back on needs somewhere to record its first attempt.
    """
    assert set(only_adsbfi.health()) == {"adsbfi", "airplaneslive", "adsblol"}
    assert only_adsbfi.source_enabled("adsbfi") is True
    assert only_adsbfi.source_enabled("airplaneslive") is False
    # And being off is not an error - nothing has failed.
    assert only_adsbfi.health()["airplaneslive"].consecutive_errors == 0
    assert only_adsbfi.health()["airplaneslive"].last_error is None


async def test_recording_a_disabled_source_is_harmless(only_adsbfi):
    """Nothing should call _record for a disabled source, but a future
    caller that does must not fetch it or mark it failed."""

    async def never_awaited():
        raise AssertionError("a disabled source was fetched")

    await only_adsbfi._record("airplaneslive", never_awaited())
    assert only_adsbfi.health()["airplaneslive"].last_attempt is None


async def test_turning_a_source_back_on_takes_effect_without_a_restart(only_adsbfi):
    """The point of moving this out of the environment: an operator can
    check whether airplanes.live has started answering again without
    editing .env over SSH and restarting the container, which drops every
    feeder connection."""
    recording = _RecordingClient()
    await only_adsbfi._poll_all(recording)
    assert {url.split("/")[2] for url in recording.urls} == {"opendata.adsb.fi"}

    only_adsbfi.settings._values["source_airplaneslive"] = True

    recording = _RecordingClient()
    await only_adsbfi._poll_all(recording)
    assert {url.split("/")[2] for url in recording.urls} == {
        "opendata.adsb.fi", "api.airplanes.live"}


def test_the_default_disables_airplaneslive():
    """The deployed default on a fresh database, not just whatever a test
    set - the environment still decides where a new deployment starts."""
    assert "airplaneslive" in aggregator_module.DISABLED_SOURCES
    assert "adsbfi" not in aggregator_module.DISABLED_SOURCES
    assert "adsblol" not in aggregator_module.DISABLED_SOURCES

    fresh = SettingsStore()
    assert fresh.source_enabled("airplaneslive") is False
    assert fresh.source_enabled("adsbfi") is True
    assert fresh.source_enabled("adsblol") is True
