"""Accepts live SBS/BaseStation feeds pushed by customers' own receivers.

One TCP listener per feeder-enabled device (see the Device.feeder_port
comment in models.py for why a dedicated port per feeder, not a shared one
with a handshake). Each connection gets its own sbs.StreamDecoder so one
feeder's aircraft never get attributed to another's, then periodically
merges its current state into the shared Aggregator cache tagged
"feeder:<device_id>" - from that point on those aircraft are just part of
the same cache /v1/aircraft already serves from, automatically included in
every device's results the same way an adsb.fi/adsb.lol aircraft would be.
"""

import asyncio
import datetime
import logging
import os

from sqlalchemy.orm import Session

from . import models
from .aggregator import Aggregator
from .feed_guard import FeedGuard
from .sbs import StreamDecoder
from .site_estimate import SiteSampler

logger = logging.getLogger("feed_ingest")

PORT_RANGE_START = int(os.environ.get("FEEDER_PORT_RANGE_START", "30100"))
PORT_RANGE_END = int(os.environ.get("FEEDER_PORT_RANGE_END", "30999"))
MERGE_INTERVAL_SECONDS = 2
DB_TOUCH_INTERVAL_SECONDS = 15  # how often a live connection updates feeder_last_message_at
SITE_ESTIMATE_INTERVAL_SECONDS = 300  # how often the receiver-position estimate is rewritten


def allocate_port(db: Session) -> int | None:
    used = {p for (p,) in db.query(models.Device.feeder_port).filter(models.Device.feeder_port.isnot(None))}
    for port in range(PORT_RANGE_START, PORT_RANGE_END + 1):
        if port not in used:
            return port
    return None  # range exhausted - caller must tell the customer to contact support


class FeedIngestManager:
    def __init__(self, aggregator: Aggregator, session_factory):
        self.aggregator = aggregator
        self._session_factory = session_factory
        self._servers: dict[int, asyncio.base_events.Server] = {}
        # Receiver-position samples, per device, surviving reconnects.
        self._samplers: dict[int, SiteSampler] = {}
        # Plausibility checks, per device. Also survive reconnects: the
        # teleport check remembers where each aircraft was last seen, and a
        # feeder reconnects whenever its link hiccups.
        self._guards: dict[int, FeedGuard] = {}

    async def sync_from_db(self):
        """Brings the running listeners into line with the database.

        This only ever started listeners. Nothing closed one whose device
        had stopped being feeder-enabled, so a feed disabled anywhere other
        than by the request that owns the listener kept being accepted
        until the service restarted - which matters now that the listeners
        follow the single-instance lease rather than startup, and that an
        operator can disable a feed from the admin panel.
        """
        db = self._session_factory()
        try:
            devices = (
                db.query(models.Device)
                .filter(models.Device.feeder_enabled.is_(True), models.Device.feeder_port.isnot(None))
                .all()
            )
            wanted = {device.feeder_port: device.id for device in devices}
        finally:
            db.close()
        # Closed first, so a port reassigned from one device to another is
        # free by the time the new listener asks for it.
        for port in [port for port in self._servers if port not in wanted]:
            await self.stop_for_device(port)
        for port, device_id in wanted.items():
            await self.start_for_device(device_id, port)

    async def start_for_device(self, device_id: int, port: int):
        if port in self._servers:
            return
        source = f"feeder:{device_id}"

        async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
            peer = writer.get_extra_info("peername")
            logger.info("feeder %s: connection from %s", device_id, peer)
            decoder = StreamDecoder()
            # An SBS stream never says where the receiver is, but the low
            # aircraft it hears do - see app/site_estimate.py.
            #
            # Kept per device, not per connection. A feeder reconnects
            # whenever its link hiccups - the deployment log shows exactly
            # that - and a sampler scoped to the connection threw away every
            # sighting each time, so on a flaky link it would never reach the
            # twelve airframes it needs and the estimate would never appear.
            sampler = self._samplers.setdefault(device_id, SiteSampler())
            # Nothing used to check what arrived here. An aircraft at 0,0 at
            # 900,000 feet, or one teleporting across the Atlantic between
            # messages, was merged and served to every customer exactly like
            # a real sighting. See app/feed_guard.py.
            guard = self._guards.setdefault(device_id, FeedGuard(device_id))
            guard.max_range_nm = self.aggregator.settings.get("feeder_max_range_nm")
            await self._in_thread(self._refresh_trusted_centre, guard, device_id)

            async def read_loop():
                while True:
                    raw = await reader.readline()
                    if not raw:
                        break
                    try:
                        line = raw.decode("ascii", errors="ignore")
                    except UnicodeDecodeError:
                        continue
                    decoder.feed_line(line)

            async def merge_loop():
                # Runs on its own fixed cadence independent of line arrival -
                # a burst of messages followed by a quiet stretch (normal for
                # real ADS-B traffic) must not leave the last-known state
                # unmerged just because nothing arrived to trigger a check.
                last_db_touch = 0.0
                # Both of these belong to this function. last_site_write used
                # to be bound in the enclosing scope while being assigned
                # here, which makes Python treat every read of it as a read
                # of an unassigned local - so the first pass raised
                # UnboundLocalError, after merging exactly once, and the loop
                # died. The connection stayed up and kept being drained, so
                # nothing looked wrong; the feed simply went quiet and the
                # customer's map emptied a minute later.
                last_site_write = 0.0
                loop = asyncio.get_event_loop()
                while True:
                    await asyncio.sleep(MERGE_INTERVAL_SECONDS)
                    positioned = [s.to_dict() for s in decoder.aircraft.values() if s.has_position()]
                    if self.aggregator.settings.get("feeder_position_checks"):
                        # Filter before merging and before sampling: a
                        # rejected position must not reach the shared cache,
                        # and must not drag this receiver's own estimated
                        # location towards wherever it claimed to be.
                        positioned = guard.filter(positioned)
                        allowed = {entry["hex"] for entry in positioned}
                    else:
                        allowed = None
                    if positioned:
                        await self.aggregator.cache.merge(source, positioned)
                    for state in decoder.aircraft.values():
                        if state.has_position() and (allowed is None or state.hex in allowed):
                            sampler.add(state.hex, state.lat, state.lon, state.alt_baro, state.updated_at)
                    decoder.prune_older_than(300)
                    now = loop.time()
                    # Both database writes go to a thread, and neither is
                    # allowed to end the loop. Run inline they blocked the
                    # event loop for as long as SQLite made them wait, which
                    # stops every other feeder connection being read at the
                    # same time; raised, they killed this connection's merge
                    # loop for good, so the feed stayed connected and silently
                    # stopped contributing aircraft.
                    if now - last_db_touch >= DB_TOUCH_INTERVAL_SECONDS:
                        last_db_touch = now
                        await self._in_thread(self._touch_device, device_id)
                    # Re-estimating on every merge would rewrite the row every
                    # two seconds for a value that barely moves.
                    if now - last_site_write >= SITE_ESTIMATE_INTERVAL_SECONDS:
                        last_site_write = now
                        estimate = sampler.estimate()
                        if estimate:
                            await self._in_thread(
                                self._store_site_estimate, device_id, estimate, len(sampler)
                            )

            async def guarded_merge_loop():
                # ensure_future swallows an exception until the task is
                # garbage collected, so the fault above produced no log line
                # at all. Never again: if this loop ends for any reason other
                # than being cancelled, it is a fault worth a traceback.
                try:
                    await merge_loop()
                except asyncio.CancelledError:
                    raise
                except Exception:  # noqa: BLE001
                    logger.exception(
                        "feeder %s: merge loop stopped - this feed is now "
                        "connected but contributing nothing", device_id,
                    )

            reader_task = asyncio.ensure_future(read_loop())
            merger_task = asyncio.ensure_future(guarded_merge_loop())
            try:
                await reader_task
            except (asyncio.IncompleteReadError, ConnectionResetError):
                pass
            finally:
                merger_task.cancel()
                logger.info("feeder %s: connection from %s closed", device_id, peer)
                writer.close()

        server = await asyncio.start_server(handle, "0.0.0.0", port)
        self._servers[port] = server
        logger.info("feeder %s: listening on port %s", device_id, port)

    async def stop_for_device(self, port: int | None):
        if port is None or port not in self._servers:
            return
        server = self._servers.pop(port)
        server.close()
        await server.wait_closed()

    async def _in_thread(self, fn, *args):
        try:
            await asyncio.to_thread(fn, *args)
        except Exception:  # noqa: BLE001 - a database hiccup must not end the feed
            logger.exception("feeder database write failed")

    def _touch_device(self, device_id: int):
        db = self._session_factory()
        try:
            db.query(models.Device).filter(models.Device.id == device_id).update(
                {"feeder_last_message_at": datetime.datetime.now(datetime.timezone.utc)}
            )
            db.commit()
        finally:
            db.close()

    def _refresh_trusted_centre(self, guard: FeedGuard, device_id: int):
        """Re-reads the device's independently known position.

        Per connection rather than once at startup: an owner who sets their
        device's location, or a receiver that starts reporting one, should
        see the range check start working on the next reconnect rather than
        after a service restart.
        """
        db = self._session_factory()
        try:
            device = db.query(models.Device).filter(models.Device.id == device_id).first()
            guard.set_trusted_centre(device.independent_location() if device else None)
        finally:
            db.close()

    def guard_stats(self) -> dict[int, dict]:
        """Per-device rejection counts, for the admin panel."""
        return {device_id: guard.stats() for device_id, guard in self._guards.items()}

    def _store_site_estimate(self, device_id: int, estimate, samples: int):
        lat, lon, spread = estimate
        db = self._session_factory()
        try:
            db.query(models.Device).filter(models.Device.id == device_id).update(
                {
                    "inferred_lat": lat,
                    "inferred_lon": lon,
                    "inferred_spread_nm": spread,
                    "inferred_at": datetime.datetime.now(datetime.timezone.utc),
                }
            )
            db.commit()
            logger.info(
                "feeder %s: receiver estimated at %.4f, %.4f (+/- %.0f nm, %d airframes)",
                device_id, lat, lon, spread, samples,
            )
        finally:
            db.close()

    async def stop_all(self):
        for port in list(self._servers):
            await self.stop_for_device(port)
