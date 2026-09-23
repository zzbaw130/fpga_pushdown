EXTENSION = csd_pg
MODULE_big = csd_pg
OBJS = csd_pg.o csd_cost.o csd_lower.o
DATA = csd_pg--0.1.sql
PGFILEDESC = "csd_pg - CEMU computational-storage planner"

PG_CONFIG ?= pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
