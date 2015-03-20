#!/usr/bin/env python

import sys
from gi.repository import Gio, OSTree, RpmOstree

repopath, ref = sys.argv[1:3]

r = OSTree.Repo.new(Gio.File.new_for_path(repopath))
r.open(None)
q = RpmOstree.db_query(r, ref, None, None)
print "Package list: "
for p in q.get_packages():
    print p

