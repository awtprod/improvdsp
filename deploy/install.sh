#!/bin/bash
# Build and install improvdsp on Raspberry Pi OS (Bookworm or later), then
# start it at every boot. Run from the project folder:  ./deploy/install.sh
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== installing build tools and ALSA headers"
sudo apt-get update
sudo apt-get install -y build-essential libasound2-dev

echo "== building"
make clean
make
make test

echo "== installing /usr/local/bin/improvdsp"
sudo make install

CONF=/boot/firmware/improvdsp.conf
if [ -f "$CONF" ]; then
    echo "== keeping existing $CONF"
else
    echo "== writing default settings to $CONF"
    sudo cp deploy/improvdsp.conf "$CONF"
fi

echo "== sound cards ALSA can see:"
improvdsp --list-devices || true

echo "== installing and starting the service"
sudo cp deploy/improvdsp.service /etc/systemd/system/improvdsp.service
sudo systemctl daemon-reload
sudo systemctl enable improvdsp
sudo systemctl restart improvdsp
sleep 2
systemctl --no-pager --lines=8 status improvdsp || true

IP=$(hostname -I | awk '{print $1}')
echo
echo "Done. Control page: http://${IP:-<pi-address>}:8080/"
echo "Logs: journalctl -u improvdsp -f"
