#!/bin/bash
# Install a prepared NetworkManager keyfile without interrupting active Wi-Fi.
# The keyfile contains a password; never print it or enable shell tracing.
set -euo pipefail
export LC_ALL=C
if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    echo 'Usage: sudo ./deploy/configure-hotspot.sh PRIVATE_KEYFILE [HOME_WIFI_UUID]' >&2
    exit 1
fi
if [ "$(id -u)" -ne 0 ] || [ ! -f "$1" ]; then
    echo 'Run as root with the private hotspot keyfile path.' >&2
    exit 1
fi
DEPLOY_DIR=$(cd "$(dirname "$0")" && pwd)
test -f "$DEPLOY_DIR/improvdsp-select-network"
test -f "$DEPLOY_DIR/improvdsp-network.service"
# UUIDs also work for profiles with spaces, punctuation, or duplicate names.
# With no explicit UUID, use the client currently active on the Pi's wlan0.
HOME_UUID=${2:-$(nmcli -g GENERAL.CON-UUID device show wlan0)}
if [[ ! "$HOME_UUID" =~ ^[[:xdigit:]]{8}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{4}-[[:xdigit:]]{12}$ ]]; then
    echo 'No active Wi-Fi client on wlan0. Pass a saved home Wi-Fi profile UUID.' >&2
    exit 1
fi
if ! HOME_TYPE=$(nmcli -g connection.type connection show uuid "$HOME_UUID" 2>/dev/null); then
    echo 'The home Wi-Fi UUID must identify an existing saved profile.' >&2
    exit 1
fi
if [ "$HOME_TYPE" != 802-11-wireless ]; then
    echo 'The home profile must be a Wi-Fi client, not Ethernet or another connection type.' >&2
    exit 1
fi
HOME_MODE=$(nmcli -g 802-11-wireless.mode connection show uuid "$HOME_UUID")
if [ -n "$HOME_MODE" ] && [ "$HOME_MODE" != infrastructure ]; then
    echo 'The home profile must be a Wi-Fi client, not a hotspot or ad-hoc network.' >&2
    exit 1
fi
if grep -Fxq 'psk=REPLACE_WITH_YOUR_HOTSPOT_PASSWORD' "$1"; then
    echo 'Replace the example hotspot password in the private keyfile before installing.' >&2
    exit 1
fi
if ! command -v dnsmasq >/dev/null; then
    apt-get -o Acquire::Retries=5 -o DPkg::Lock::Timeout=300 update
    DEBIAN_FRONTEND=noninteractive apt-get -o Acquire::Retries=5 \
        -o DPkg::Lock::Timeout=300 install -y dnsmasq-base
fi
TARGET=/etc/NetworkManager/system-connections/improvdsp-hotspot.nmconnection
install -o root -g root -m 600 "$1" "$TARGET"
nmcli connection load "$TARGET"
nmcli connection modify improvdsp-hotspot connection.autoconnect yes connection.autoconnect-priority -999
nmcli connection modify uuid "$HOME_UUID" connection.autoconnect yes
printf 'HOME_CONNECTION_UUID=%s\n' "$HOME_UUID" > /etc/default/improvdsp-network
chmod 644 /etc/default/improvdsp-network
install -o root -g root -m 755 "$DEPLOY_DIR/improvdsp-select-network" /usr/local/sbin/improvdsp-select-network
install -o root -g root -m 644 "$DEPLOY_DIR/improvdsp-network.service" /etc/systemd/system/improvdsp-network.service
systemctl daemon-reload
systemctl enable improvdsp-network.service
nmcli -f connection.id,connection.autoconnect,connection.autoconnect-priority,802-11-wireless.ssid,802-11-wireless.mode,ipv4.method,ipv4.addresses \
    connection show improvdsp-hotspot
echo 'Hotspot fallback saved. Existing home Wi-Fi remains active.'
