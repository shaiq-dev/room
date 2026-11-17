# Not run on an upgrade. Stopping room.service kills every box.
if [ -d /run/systemd/system ]; then
    systemctl disable --now room.service || :
fi
