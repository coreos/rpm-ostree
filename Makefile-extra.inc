# -*- mode: makefile -*-
# This file defines rules that don't depend on Automake.
# The main idea here is some things we may want to execute
# from CI before we've even run automake, and secondly
# one can avoid rerunning Automake when these rules change.
#
# Specifically right now one can invoke e.g.
# `make -f Makefile-extra.inc rustfmt` directly.

rustfmt:
	rustfmt rust/src/*.rs
.PHONY: rustfmt

