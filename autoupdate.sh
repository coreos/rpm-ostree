#!/bin/bash

NUM=1

function conn {
	sudo rpm-ostree upgrade --check
	echo "updated" > /usr/local/bin/auto_update_flag
	NUM=2
}

function nonn {
	sleep 60
}
	
while [ $NUM -eq 1 ]; do
	
	ping -q -w 1 -c 1 `ip r | grep default | cut -d ' ' -f 3` > /dev/null && conn || nonn
	
done
