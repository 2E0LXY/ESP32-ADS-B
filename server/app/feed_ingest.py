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
from .sbs import StreamDecoder

logger = logging.getLogger("feed_ingest")

PORT_RANGE_START = int(os.environ.get("FEEDER_PORT_RANGE_START", "30100"))
PORT_RANGE_END = int(os.environ.get("FEEDER_PORT_RANGE_END", "30999"))
MERGE_INTERVAL_SECONDS = 2
DB_TOUCH_INTERVAL_SECONDS = 15  # how often a live connection updates feeder_last_message_at


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

    async def sync_from_db(self):
        db = self._session_factory()
        try:
            devices = (
                db.query(models.Device)
                .filter(models.Device.feeder_enabled.is_(True), models.Device.feeder_port.isnot(None))
                .all()
            )
            for device in devices:
                await self.start_for_device(device.id, device.feeder_port)
        finally:
            db.close()

    async def start_for_device(self, device_id: int, port: int):
        if port in self._servers:
            return
        source = f"feeder:{device_id}"

        async def handle(reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
            peer = writer.get_extra_info("peername")
            logger.info("feeder %s: connection from %s", device_id, peer)
            decoder = StreamDecoder()

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
                loop = asyncio.get_event_loop()
                while True:
                    await asyncio.sleep(MERGE_INTERVAL_SECONDS)
                    positioned = [s.to_dict() for s in decoder.aircraft.values() if s.has_position()]
                    if positioned:
                        await self.aggregator.cache.merge(source, positioned)
                    decoder.prune_older_than(300)
                    now = loop.time()
                    if now - last_db_touch >= DB_TOUCH_INTERVAL_SECONDS:
                        last_db_touch = now
                        self._touch_device(device_id)

            reader_task = asyncio.ensure_future(read_loop())
            merger_task = asyncio.ensure_future(merge_loop())
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

    def _touch_device(self, device_id: int):
        db = self._session_factory()
        try:
            db.query(models.Device).filter(models.Device.id == device_id).update(
                {"feeder_last_message_at": datetime.datetime.now(datetime.timezone.utc)}
            )
            db.commit()
        finally:
            db.close()

    async def stop_all(self):
        for port in list(self._servers):
            await self.stop_for_device(port)
