srpm:
	./ci/installdeps.sh
	# similar to https://github.com/actions/checkout/issues/760, but for COPR
	git config --global --add safe.directory '*'
	# if we have a git repo with remotes, fetch tags so `git describe` gives a nice NEVRA when
	# building the RPM
	if git remote | grep origin; then git fetch origin --tags; fi
	git submodule update --init --recursive
	# Our primary CI build goes via RPM rather than direct to binaries
	# to better test that path, including our vendored spec file, etc.
	make -C packaging -f Makefile.dist-packaging srpm
	if test -n "$$outdir"; then mv packaging/*.src.rpm $$outdir; fi

