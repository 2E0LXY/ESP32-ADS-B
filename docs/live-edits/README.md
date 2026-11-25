# Uncommitted edits found on the adsb host (2026-11-25)

Source: /opt/adsb-repo on host `adsb`, branch `claude/project-overview-status-jqcyea` at 1c5d122
(5 commits behind main). Saved here so they are not lost; the same patch is backed up on the host at
/root/backups/adsb-repo-uncommitted-*.patch. Nothing was applied or deployed.

| Hunk | Effect | Verdict (to review) |
|---|---|---|
| `admin.py`, `public.py`: clear the other role's session cookie on login/signup | Prevents an account session and admin session coexisting | Keep: worth porting to main |
| `admin.py`, `public.py`: refuse account creation/signup with an email that belongs to an admin | Prevents an account shadowing an admin identity | Keep: worth porting to main |
| `public.py` `my_feed_aircraft`: `total` becomes `len(aircraft)` and the explanatory comment is removed | Reverts the in-range total semantics | Likely a regression: main may already differ. Do NOT port without checking `total_in_range` |

Apply with `git apply --check` against main first; the file context may have moved.
