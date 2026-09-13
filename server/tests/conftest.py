"""Test bootstrap.

The app builds its SQLAlchemy engine at import time from DATABASE_URL, so
that variable has to be set before anything under app/ is imported - hence
setting it here, at collection, rather than in a fixture. Each test then gets
an empty schema on the same throwaway file.
"""

import asyncio
import inspect
import os
import tempfile

_handle, _DB_PATH = tempfile.mkstemp(suffix=".db")
os.close(_handle)

os.environ["DATABASE_URL"] = f"sqlite:///{_DB_PATH}"
os.environ["SESSION_SECRET"] = "test-secret-not-a-real-one"
# Startup refuses to run without these, and a real bootstrap admin is
# irrelevant to what these tests cover.
os.environ.setdefault("ADMIN_BOOTSTRAP_EMAIL", "admin@example.com")
os.environ.setdefault("ADMIN_BOOTSTRAP_PASSWORD", "test-admin-password")

import pytest  # noqa: E402
from fastapi.testclient import TestClient  # noqa: E402

from app.database import Base, engine  # noqa: E402
from app.main import app  # noqa: E402


@pytest.fixture(autouse=True)
def _current_event_loop(request):
    """Guarantees a current event loop for the synchronous tests.

    Several of them drive the cache with
    asyncio.get_event_loop().run_until_complete(...). That only worked
    because some earlier import had left a loop installed - pytest-asyncio
    closes and unsets the loop it creates for an async test, so as soon as
    an async test file sorted before one of those, the sync test failed
    with "There is no current event loop". Installing a fresh loop per
    synchronous test makes that independent of test order.
    """
    if inspect.iscoroutinefunction(request.function):
        yield  # pytest-asyncio owns the loop for these
        return
    loop = asyncio.new_event_loop()
    asyncio.set_event_loop(loop)
    try:
        yield
    finally:
        asyncio.set_event_loop(None)
        loop.close()


@pytest.fixture()
def client():
    # Fresh schema per test: these exercise redirect-driven flows that leave
    # rows behind, and a leaked account would make the next signup collide.
    Base.metadata.drop_all(bind=engine)
    Base.metadata.create_all(bind=engine)
    # https, not http: the session cookie is set with secure=True, so over
    # plain http the client would silently never send it back and every
    # authenticated request would redirect to the login page.
    with TestClient(app, base_url="https://testserver") as test_client:
        # Startup launches the real polling loop, which fetches live aircraft
        # from adsb.fi and friends and merges them into the same cache these
        # tests seed. Stop it: a test suite must not depend on the sky, on a
        # third party being up, or on the network existing at all.
        task = getattr(app.state.aggregator, "_task", None)
        if task is not None:
            task.cancel()
            app.state.aggregator._task = None
        # The cache lives on app.state and the app is imported once, so
        # without this a test sees aircraft seeded by an earlier one.
        app.state.aggregator.cache._by_hex.clear()
        yield test_client


def pytest_sessionfinish(session, exitstatus):
    try:
        os.unlink(_DB_PATH)
    except OSError:
        pass
