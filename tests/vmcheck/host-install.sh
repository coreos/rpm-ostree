#!/bin/bash
set -xeuo pipefail
eval $(findmnt /host/usr -o OPTIONS -P)
(IFS=,;
 for x in ${OPTIONS}; do
     if test ${x} == "ro"; then
	 mount -o remount,rw /host/usr
	 break
     fi
 done)
make install DESTDIR=/host/
     
