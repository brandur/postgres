# src/test/modules/test_inet/Makefile

MODULE_big = test_inet
OBJS = test_inet.o $(WIN32RES)
PGFILEDESC = "test_inet - test code for the inet type"

EXTENSION = test_inet
DATA = test_inet--1.0.sql

REGRESS = test_inet

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = src/test/modules/test_inet
top_builddir = ../../../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif
