#!/bin/sh
rm -f /tmp/BlueToolH4 /usr/local/bin/BlueToolH4
cp /usr/sbin/BlueTool /tmp/BlueToolH4
/usr/local/bin/PatchBlueTool /tmp/BlueToolH4
ldid -s /tmp/BlueToolH4
cp -f /tmp/BlueToolH4 /usr/local/bin
rm -f /tmp/BlueToolH4
