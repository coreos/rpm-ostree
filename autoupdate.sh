 #!/bin/bash

NUM1=1

while [ $NUM1 -eq 1 ]; do

	ping -q -w 1 -c 1 `ip r | grep default | cut -d ' ' -f 3` > /dev/null && con || non

	function con {
		echo "Updating Starts"
    sudo rpm-ostree upgrade --check
    echo "Updating is done"
    echo " "
    NUM1=2
    echo "Trying to reboot os"
    echo " "
    echo "Press Enter to Reboot | Press Ctrl + C to exit"
    read RB 
    echo "Rebooting"
    sudo reboot
	}

	function non {
		echo "offline"
		sleep 2
	}

done
