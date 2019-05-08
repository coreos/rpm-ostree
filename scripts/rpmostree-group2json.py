#!/usr/bin/python3

import os
import sys

out = """\
    "check-groups": { "type": "data",\n\
                      "entries": { """
prefix_len = len('                      "entries": { ')
done = False
for fname in sys.argv[1:]:
    for line in open(fname):
        (group,x,gid,x) = line.split(':', 3)
        if done:
            out += ',\n'
            out += ' ' * prefix_len
        done = True
        out += '"%s": %s' % (group, gid)
out += ' } },'
print(out)
