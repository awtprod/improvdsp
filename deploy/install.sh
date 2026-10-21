#!/bin/bash
# Build and install improvdsp on Raspberry Pi OS (Bookworm or later), then
# start it at every boot. Run from the project folder:  ./deploy/install.sh
set -euo pipefail
cd "$(dirname "$0")/.."

# Also works unattended as root during the USB image's first boot.
SUDO=()
if [ "$(id -u)" -ne 0 ]; then
    SUDO=(sudo)
fi

echo "== installing build tools and ALSA headers"
"${SUDO[@]}" apt-get -o Acquire::Retries=5 -o DPkg::Lock::Timeout=300 update
"${SUDO[@]}" env DEBIAN_FRONTEND=noninteractive apt-get \
    -o Acquire::Retries=5 -o DPkg::Lock::Timeout=300 install -y \
    build-essential libasound2-dev alsa-utils avahi-daemon

echo "== building"
make clean
make
make test

echo "== installing /usr/local/bin/improvdsp"
"${SUDO[@]}" make install

CONF=/boot/firmware/improvdsp.conf
if [ -f "$CONF" ]; then
    echo "== keeping existing $CONF"
else
    echo "== writing default settings to $CONF"
    "${SUDO[@]}" cp deploy/improvdsp.conf "$CONF"
fi

"${SUDO[@]}" install -d -m 755 /var/lib/improvdsp/recordings
"${SUDO[@]}" systemctl enable --now avahi-daemon

echo "== sound cards ALSA can see:"
improvdsp --list-devices || true

echo "== installing and starting the service"
"${SUDO[@]}" cp deploy/improvdsp.service /etc/systemd/system/improvdsp.service
"${SUDO[@]}" systemctl daemon-reload
"${SUDO[@]}" systemctl enable improvdsp
"${SUDO[@]}" systemctl restart improvdsp
sleep 2
systemctl --no-pager --lines=8 status improvdsp

IP=$(hostname -I | awk '{print $1}')
echo
echo "Done. Control page: http://${IP:-<pi-address>}:8080/"
echo "Logs: journalctl -u improvdsp -f"
