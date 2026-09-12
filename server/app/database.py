import os
from sqlalchemy import create_engine, event
from sqlalchemy.orm import sessionmaker, declarative_base

# SQLite is deliberately the default: this backend serves a handful of
# customer accounts and a modest number of devices polling every ~30s, not
# a high-write-concurrency workload. It is far easier to back up and reason
# about than running Postgres for a database this small. Point DATABASE_URL
# at Postgres later if usage ever outgrows this - nothing else here is
# SQLite-specific.
#
# It runs in WAL mode (see below), so a backup is "sqlite3 aggregator.db
# '.backup out.db'", not a copy of the one file: the newest commits live in
# the -wal sidecar until a checkpoint folds them in.
DATABASE_URL = os.environ.get("DATABASE_URL", "sqlite:////data/aggregator.db")

connect_args = {"check_same_thread": False} if DATABASE_URL.startswith("sqlite") else {}
engine = create_engine(DATABASE_URL, connect_args=connect_args)
SessionLocal = sessionmaker(autocommit=False, autoflush=False, bind=engine)
Base = declarative_base()

# How long a writer waits for another writer to finish before giving up.
# pysqlite's own default is 5s of *blocking the calling thread*, which on
# this deployment is often the asyncio event loop thread - see the pragma
# comment below.
SQLITE_BUSY_TIMEOUT_MS = 3000

if DATABASE_URL.startswith("sqlite"):

    @event.listens_for(engine, "connect")
    def _sqlite_pragmas(dbapi_connection, _connection_record):
        """Rollback-journal SQLite serialises *everything*.

        In the default journal mode a writer takes an exclusive lock on the
        whole file and every reader waits behind it. This process has
        several writers running concurrently - each device poll writes a
        UsageLog row and its reported position, every live feeder connection
        stamps feeder_last_message_at, the aggregator reads the device table
        three times a cycle - so contention is normal here, not exceptional,
        and a waiting writer parks its thread for the full busy timeout.

        WAL lets readers carry on while a write is in flight, which removes
        most of that waiting outright, and an explicit busy timeout bounds
        what is left.
        """
        cursor = dbapi_connection.cursor()
        try:
            cursor.execute("PRAGMA journal_mode=WAL")
            cursor.execute(f"PRAGMA busy_timeout={SQLITE_BUSY_TIMEOUT_MS}")
            # NORMAL rather than FULL: with WAL this still survives a process
            # crash, only losing the most recent commits to a power cut. For
            # a cache of aircraft sightings that is the right trade.
            cursor.execute("PRAGMA synchronous=NORMAL")
        finally:
            cursor.close()


def get_db():
    db = SessionLocal()
    try:
        yield db
    finally:
        db.close()


def add_missing_columns(base):
    """Adds columns that exist in the models but not yet in the database.

    There is no Alembic here and create_all() only creates missing tables -
    it will not touch a table that already exists. Without this, adding a
    column to a model leaves every existing deployment raising
    OperationalError on the next query, which is a rough upgrade for a
    customer who has done nothing wrong.

    Deliberately narrow: it only ever adds nullable columns, and never
    drops, renames or retypes anything. Anything beyond that needs a real
    migration written by hand, and should not happen silently on boot.
    """
    import logging

    from sqlalchemy import inspect, text

    logger = logging.getLogger("database")
    inspector = inspect(engine)
    existing_tables = set(inspector.get_table_names())
    with engine.begin() as connection:
        for table in base.metadata.sorted_tables:
            if table.name not in existing_tables:
                continue  # create_all() will make it, with every column
            present = {c["name"] for c in inspector.get_columns(table.name)}
            for column in table.columns:
                if column.name in present:
                    continue
                if not column.nullable and column.default is None and column.server_default is None:
                    logger.error(
                        "%s.%s is missing and NOT NULL with no default - needs a hand-written "
                        "migration, skipping", table.name, column.name,
                    )
                    continue
                ddl = column.type.compile(engine.dialect)
                connection.execute(text(f'ALTER TABLE {table.name} ADD COLUMN {column.name} {ddl}'))
                logger.info("added column %s.%s", table.name, column.name)

    # ALTER TABLE ADD COLUMN carries no index with it, so a newly added
    # column declared index=True or unique=True had neither on an upgraded
    # deployment - only on a database created fresh from the models. That is
    # a silent difference between the two, and for a column looked up on
    # every request (a share token, say) it is the difference between an
    # index seek and a table scan.
    inspector = inspect(engine)  # re-inspect: the columns above are new
    for table in base.metadata.sorted_tables:
        if table.name not in existing_tables:
            continue
        present = {i["name"] for i in inspector.get_indexes(table.name)}
        for index in table.indexes:
            if index.name in present:
                continue
            try:
                index.create(bind=engine)
                logger.info("added index %s", index.name)
            except Exception as exc:  # noqa: BLE001
                # A duplicate value already in the table can make a unique
                # index impossible to add. Worth saying loudly; not worth
                # refusing to boot over.
                logger.error("could not add index %s: %s", index.name, exc)
