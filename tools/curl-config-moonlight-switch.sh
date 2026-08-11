#!/bin/sh

case "$1" in
    --version)
        echo "libcurl 8.11.0"
        ;;
    --feature)
        printf '%s\n' SSL IPv6 libz threadsafe
        ;;
    --protocols)
        printf '%s\n' HTTP HTTPS WS WSS
        ;;
    *)
        exit 1
        ;;
esac
