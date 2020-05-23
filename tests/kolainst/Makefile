DESTDIR ?=

TESTDIRS := $(shell find -mindepth 1 -maxdepth 1 -type d)

KOLA_TESTDIR = $(DESTDIR)/usr/lib/coreos-assembler/tests/kola/rpm-ostree/

all:
	@echo "No build step"

install:
	install -d -m 0755 $(KOLA_TESTDIR)
	rsync -prlv ./nondestructive $(KOLA_TESTDIR)/
	rsync -prlv ../common/ $(KOLA_TESTDIR)/nondestructive/data
