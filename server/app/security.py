import hashlib
import os
import secrets
import datetime

import bcrypt
from jose import jwt, JWTError

# bcrypt truncates its input at 72 bytes silently having nothing beyond
# that contribute to the hash - encoding as UTF-8 first and slicing to 72
# bytes (not 72 characters, since multi-byte UTF-8 characters would
# otherwise let the slice land mid-character) makes that limit explicit
# rather than relying on the library to enforce it consistently across
# versions (this exact inconsistency is why passlib's own bcrypt wrapper
# was dropped in favour of calling bcrypt directly - see git history).
def _prepare(password: str) -> bytes:
    return password.encode("utf-8")[:72]

# Must be set in the environment in production (see .env.example) - the
# fallback only exists so the app doesn't crash on a first `python -c
# "import app.main"` sanity check. A session signed with the fallback
# secret would let anyone forge a login, so the app refuses to start
# outside DEBUG mode without an explicit SESSION_SECRET (see main.py).
SESSION_SECRET = os.environ.get("SESSION_SECRET", "insecure-dev-secret-change-me")
SESSION_ALGO = "HS256"
SESSION_TTL_HOURS = 24 * 14


def hash_password(password: str) -> str:
    return bcrypt.hashpw(_prepare(password), bcrypt.gensalt()).decode("ascii")


def verify_password(password: str, password_hash: str) -> bool:
    try:
        return bcrypt.checkpw(_prepare(password), password_hash.encode("ascii"))
    except ValueError:
        return False


def create_session_token(subject: str, scope: str) -> str:
    """scope is "admin" or "account" - kept inside the signed token itself
    so a customer session can never be replayed against admin routes even
    if the two happened to share a secret or a route forgot to check."""
    expires = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(hours=SESSION_TTL_HOURS)
    payload = {"sub": subject, "scope": scope, "exp": expires}
    return jwt.encode(payload, SESSION_SECRET, algorithm=SESSION_ALGO)


def read_session_token(token: str, expected_scope: str) -> str | None:
    try:
        payload = jwt.decode(token, SESSION_SECRET, algorithms=[SESSION_ALGO])
    except JWTError:
        return None
    if payload.get("scope") != expected_scope:
        return None
    return payload.get("sub")


# API keys: "adsb_" + 32 random URL-safe characters. Only the SHA-256 hash
# is ever persisted (see ApiKey model) - the prefix is stored separately,
# unhashed, purely so the admin panel can show "adsb_x7Hf..." to help a
# customer recognise which key is which without ever storing anything that
# lets the full key be reconstructed.
API_KEY_PREFIX = "adsb_"


def generate_api_key() -> tuple[str, str, str]:
    """Returns (plaintext_key, prefix, sha256_hash) - plaintext is shown to
    the user exactly once by the caller and never stored."""
    plaintext = API_KEY_PREFIX + secrets.token_urlsafe(24)
    prefix = plaintext[: len(API_KEY_PREFIX) + 6]
    digest = hash_api_key(plaintext)
    return plaintext, prefix, digest


def hash_api_key(plaintext: str) -> str:
    return hashlib.sha256(plaintext.encode("utf-8")).hexdigest()


def generate_claim_code() -> str:
    """Short, human-typeable code a customer enters on their device (or the
    device shows one it generated, matched here) to link it to their
    account. Not a secret on its own - it only ever grants "attach this
    unclaimed device to my account", never data access, so a short
    alphanumeric code is fine."""
    alphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"  # no 0/O/1/I
    return "".join(secrets.choice(alphabet) for _ in range(8))
