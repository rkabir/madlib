DATA_built = bayes.sql bayes.py bayes_drop.sql
PYTHON_built = bayes.py

# SOURCES = $(wildcard *.c)

REGRESS = bayesRegression_test

# commenting the next line means that, for the regression tests, the shared
# library will be looked for in the PostgreSQL library directory and not in
# the current directory
REGRESS_OPTS := --dlpath=.

# MODULE_big = naiveBayesClassification
# OBJS = $(SOURCES:%.c=%.o)

# We want to include debugging symbols.
# For release versions, comment out the next line.
# COPT = -g3 -O0 -std=c99

PG_CONFIG = pg_config 
PGXS := $(shell $(PG_CONFIG) --pgxs) 

include $(PGXS)
include config.mk 

# do not delete .o files because we want them for debugging
# .PRECIOUS: %.o
