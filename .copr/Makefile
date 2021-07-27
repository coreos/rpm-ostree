srpm:
	./ci/installdeps.sh
	# fetch tags so `git describe` gives a nice NEVRA when building the RPM
	git fetch origin --tags
	git submodule update --init --recursive
	ci/installdeps.sh
	# Our primary CI build goes via RPM rather than direct to binaries
	# to better test that path, including our vendored spec file, etc.
	# The RPM build expects pre-generated bindings, so do that now.
	make -f Makefile.bindings bindings
	make -C packaging -f Makefile.dist-packaging srpm
	if test -n "$$outdir"; then mv packaging/*.src.rpm $$outdir; fi

