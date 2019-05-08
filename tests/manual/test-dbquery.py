#!/usr/bin/env python3

import sys
from gi.repository import Gio, OSTree, RpmOstree

repopath, ref = sys.argv[1:3]

r = OSTree.Repo.new(Gio.File.new_for_path(repopath))
r.open(None)
qr = RpmOstree.db_query_all(r, ref, None)
print "Package list: "
for p in qr:
    print p.get_nevra()

_,removed,added,modold,modnew = RpmOstree.db_diff(r, ref + '^', ref, None)
for p in removed:
    print "D " + p.get_nevra()
for p in added:
    print "A " + p.get_nevra()
for o,n in zip(modold, modnew):
    print "M {0} {1} -> {2}".format(o.get_name(), o.get_evr(), n.get_evr())

