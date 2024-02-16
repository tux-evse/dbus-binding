#!/bin/bash

export LD_LIBRARY_PATH=/usr/local/lib64
cynagora-admin set '' 'HELLO' '' '*' yes
clear

# build test config dirname
DIRNAME=`dirname $0`
cd $DIRNAME/..
CONFDIR=`pwd`/etc

DEVTOOL_PORT=1239
echo Slac debug mode config=$CONFDIR/*.json port=$DEVTOOL_PORT

afb-binder --name=afb-dbus --port=$DEVTOOL_PORT -v \
  --config=$CONFDIR/binder-test-dbus.json \
  $*