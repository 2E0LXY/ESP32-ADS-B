import os
from sqlalchemy import create_engine
from sqlalchemy.orm import sessionmaker, declarative_base

# SQLite is deliberately the default: this backend serves a handful of
# customer accounts and a modest number of devices polling every ~30s, not
# a high-write-concurrency workload. A single file is easier to back up
# (copy one file) and easier to reason about than running Postgres for a
# database this small. Point DATABASE_URL at Postgres later if usage ever
# outgrows this - nothing else here is SQLite-specific.
DATABASE_URL = os.environ.get("DATABASE_URL", "sqlite:////data/aggregator.db")

connect_args = {"check_same_thread": False} if DATABASE_URL.startswith("sqlite") else {}
engine = create_engine(DATABASE_URL, connect_args=connect_args)
SessionLocal = sessionmaker(autocommit=False, autoflush=False, bind=engine)
Base = declarative_base()


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
