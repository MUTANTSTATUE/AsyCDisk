#!/bin/bash

INTERFACE="lo" # Local loopback
ACTION=$1
DELAY=$2
LOSS=$3

if [ "$ACTION" == "set" ]; then
    sudo tc qdisc add dev $INTERFACE root netem delay ${DELAY:-100ms} loss ${LOSS:-1%}
    echo "[*] Network simulation set: delay ${DELAY:-100ms}, loss ${LOSS:-1%}"
elif [ "$ACTION" == "unset" ]; then
    sudo tc qdisc del dev $INTERFACE root
    echo "[*] Network simulation removed."
else
    echo "Usage: $0 {set|unset} [delay] [loss]"
fi
