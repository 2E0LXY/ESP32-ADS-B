"""End-to-end: a connected feeder must actually reach the shared cache.

There was no test that ran a real TCP connection through FeedIngestManager
and looked for the aircraft at the other end. That gap hid a merge loop
that raised on its first iteration and died silently: the socket kept being
drained by the read loop, so the connection looked perfectly healthy in the
logs while the feed contributed nothing and the customer's "my feed" map
stayed empty.
"""

import asyncio
import socket

from app.aggregator import Aggregator
from app.database import Base, SessionLocal, engine
from app.feed_ingest import MERGE_INTERVAL_SECONDS, FeedIngestManager

def _sbs_line(hex_ident: str, callsign: str) -> bytes:
    """One valid SBS-1 "airborne position" line: 22 comma-separated fields."""
    return (
        f"MSG,3,1,1,{hex_ident},1,2026/09/11,17:40:00.000,2026/09/11,17:40:00.000,"
        f"{callsign},35000,450,270,53.7264,-1.5744,0,7000,0,0,0,0\n"
    ).encode()


def _free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


async def test_a_connected_feeder_reaches_the_cache():
    Base.metadata.create_all(bind=engine)  # no client fixture here
    aggregator = Aggregator(0.0, 0.0, 50, SessionLocal)
    manager = FeedIngestManager(aggregator, SessionLocal)
    port = _free_port()
    await manager.start_for_device(7, port)
    try:
        _reader, writer = await asyncio.open_connection("127.0.0.1", port)
        writer.write(_sbs_line("4CA2D5", "RYR2BH"))
        await writer.drain()
        await asyncio.sleep(MERGE_INTERVAL_SECONDS + 0.5)
        first = await aggregator.cache.query_by_source("feeder:7")

        # A second aircraft, after the first merge has already happened.
        # Checking only the first would pass against a loop that runs once
        # and then dies - which is exactly the fault this covers, and which
        # looks to the customer like a map that fills and then empties.
        writer.write(_sbs_line("407F2B", "EZY68GD"))
        await writer.drain()
        await asyncio.sleep(MERGE_INTERVAL_SECONDS + 0.5)
        second = await aggregator.cache.query_by_source("feeder:7")

        writer.close()
    finally:
        await manager.stop_for_device(port)

    assert sorted(a["hex"] for a in first) == ["4ca2d5"], (
        "the feeder's aircraft never reached the cache at all"
    )
    assert sorted(a["hex"] for a in second) == ["407f2b", "4ca2d5"], (
        "the merge loop stopped after its first pass - the connection stays "
        "up and the feed goes quiet"
    )
