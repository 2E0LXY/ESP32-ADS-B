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
