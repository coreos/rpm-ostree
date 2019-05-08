#!/usr/bin/python3

import os
import sys

out = """\
    "check-passwd": { "type": "data",\n\
                      "entries": { """
prefix_len = len('                      "entries": { ')
done = False
for fname in sys.argv[1:]:
    for line in open(fname):
        (user,xpass,uid,gid,x) = line.split(':', 4)
        if done:
            out += ',\n'
            out += ' ' * prefix_len
        done = True
        if uid == gid:
            out += '"%s": %s' % (user, uid)
        else:
            out += '"%s": [%s, %s]' % (user, uid, gid)
out += ' } },'
print(out)
