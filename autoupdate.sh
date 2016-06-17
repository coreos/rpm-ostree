 #!/bin/bash
 
 #file_Location_/usr/bin/autoupdate.sh

NUM=1

while [ $NUM -eq 1 ]; do

	ping -q -w 1 -c 1 `ip r | grep default | cut -d ' ' -f 3` > /dev/null && conn || nonn

	function conn {
    	sudo rpm-ostree upgrade --check
    	NUM=2
	}

	function nonn {
		sleep 60
	}

done
