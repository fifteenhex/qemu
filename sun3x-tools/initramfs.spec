dir /dev 0755 0 0
nod /dev/console 0600 0 0 c 5 1
nod /dev/null 0666 0 0 c 1 3
nod /dev/tty 0666 0 0 c 5 0
dir /proc 0755 0 0
dir /sys 0755 0 0
dir /bin 0755 0 0
dir /lib 0755 0 0
dir /root 0755 0 0
dir /tmp 0777 0 0
file /init /tmp/sun3-ir/files/init 0755 0 0
file /bin/busybox /tmp/sun3-ir/files/busybox 0755 0 0
slink /bin/sh busybox 0777 0 0
file /lib/ld.so.1 /tmp/sun3-ir/files/ld.so.1 0755 0 0
file /lib/libc.so.6 /tmp/sun3-ir/files/libc.so.6 0755 0 0
file /lib/libresolv.so.2 /tmp/sun3-ir/files/libresolv.so.2 0755 0 0
