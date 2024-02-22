#!/bin/bash

export LD_LIBRARY_PATH=/usr/local/lib64
cynagora-admin set '' 'HELLO' '' '*' yes
clear

# build test config dirname
DIRNAME=`dirname $0`
cd $DIRNAME/..
CONFDIR=`pwd`/etc

DEVTOOL_PORT=1231
echo dbus binding config=$CONFDIR/binder-test-dbus.json port=$DEVTOOL_PORT

afb-binder --port=$DEVTOOL_PORT -vvv \
  --config=$CONFDIR/binder-test-dbus.json \
  --binding=/usr/redpesk/dbus-binding/lib/dbus-binding.so \
  $*