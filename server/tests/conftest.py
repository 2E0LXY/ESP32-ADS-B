"""Test bootstrap.

The app builds its SQLAlchemy engine at import time from DATABASE_URL, so
that variable has to be set before anything under app/ is imported - hence
setting it here, at collection, rather than in a fixture. Each test then gets
an empty schema on the same throwaway file.
"""

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
        yield test_client


def pytest_sessionfinish(session, exitstatus):
    try:
        os.unlink(_DB_PATH)
    except OSError:
        pass
