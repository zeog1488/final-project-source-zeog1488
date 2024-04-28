#!/bin/sh

case "$1" in
    start)
        echo "Starting securitySystem"
        start-stop-daemon -S -n securitySystem -a /usr/bin/securitySystem -- -d
        ;;
    stop)
        echo "Stopping securitySystem"
        start-stop-daemon -K -n securitySystem -s TERM
        ;;
    *)
        echo "Usage: $0 {start|stop}"
    exit 1
esac
exit 0