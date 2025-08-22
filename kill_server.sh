#!/bin/bash

# Quick alias to kill dedup-server
# This is a simple wrapper around the main kill script

exec ./kill_dedup_server.sh "$@"
