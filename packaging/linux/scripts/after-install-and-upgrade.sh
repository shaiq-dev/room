# Under systemd room only uses the cgroup of room.service, so start the unit.
# enable --now leaves a running unit alone. An upgrade must not restart it,
# because that kills every box.
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload
    systemctl enable --now room.service
fi
