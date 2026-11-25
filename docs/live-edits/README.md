# Uncommitted edits found on the adsb host (2026-11-25)

Source: /opt/adsb-repo on host `adsb`, branch `claude/project-overview-status-jqcyea` at 1c5d122
(5 commits behind main). Nothing was applied to the live host or deployed. The same full patch is backed up on the
host at /root/backups/adsb-repo-uncommitted-*.patch.

| Hunk | Effect | Verdict |
|---|---|---|
| `admin.py`, `public.py`: clear the other role's session cookie on login/signup | Prevents an account session and an admin session coexisting | Keep |
| `admin.py`, `public.py`: refuse account creation/signup with an email that belongs to an admin | Prevents an account shadowing an admin identity | Keep |
| `public.py` `my_feed_aircraft`: `total` becomes `len(aircraft)` | Reverts the in-range total semantics; main already has `total_in_range` | Drop (regression) |

## Files

- `2026-11-25-adsb-host-uncommitted.patch`: the full set of host edits, for the record (do not apply).
- `2026-11-25-adsb-keepers-vs-main.patch`: only the two keeper hunks (5 diff hunks). Checked on 2026-11-25
  against `origin/main` in a temporary worktree: `git apply --check` passes, the patch applies, and
  `python3 -m py_compile` succeeds on both files. The test suite was NOT run (dependencies not installed on the host).

## To port

    git checkout -b security/role-cookie-and-admin-email origin/main
    git apply docs/live-edits/2026-11-25-adsb-keepers-vs-main.patch
    # run server/tests, then commit

Add tests for: login as admin clears `account_session`; login/signup as account clears `admin_session`;
signup and admin-create-account with an admin's email are refused.
