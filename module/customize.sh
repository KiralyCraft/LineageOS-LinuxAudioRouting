#!/system/bin/sh
[ "$API" -ge 35 ] || abort "Linux Audio requires Android 15 / API 35 or newer"
[ "$ARCH" = arm64 ] || abort "This build is for arm64"
set_perm_recursive "$MODPATH" 0 0 0755 0644
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/bin/linux-audiod" 0 0 0755
ui_print "Android helper requires manual Start and permission approval after each reboot."
ui_print "This module does not replace Android audio drivers or the HDMI module."
