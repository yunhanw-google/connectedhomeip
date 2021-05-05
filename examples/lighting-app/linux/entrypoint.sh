#!/bin/bash

service dbus start
sleep 1
service avahi-daemon start
/usr/sbin/otbr-agent -I wpan0 spinel+hdlc+uart:///dev/ttyUSB0 &
sleep 1
ot-ctl panid 0x1234
ot-ctl ifconfig up
ot-ctl thread start

gdb -return-child-result -q -ex run -ex bt --args chip-lighting-app --thread
