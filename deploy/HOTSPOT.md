# Home Wi-Fi with hotspot fallback

This setup targets Raspberry Pi OS Lite with NetworkManager and the Pi's built-in
`wlan0`. Configure and save your home Wi-Fi using Raspberry Pi Imager or
NetworkManager before installing the hotspot. Install ImprovDSP with
`./deploy/install.sh` first.

## Install

Create a private copy of the example on the Pi, outside the source checkout:

```sh
sudo install -o root -g root -m 600 deploy/improvdsp-hotspot.nmconnection.example /run/improvdsp-hotspot.nmconnection
sudo nano /run/improvdsp-hotspot.nmconnection
```

Replace `REPLACE_WITH_YOUR_HOTSPOT_PASSWORD` in the private copy with a WPA2
password of 8–63 ASCII characters. Use the editor to enter it so the password
does not enter shell history or command-line arguments. NetworkManager keyfiles
use GLib escaping: write a literal backslash as `\\` and spaces as `\s`. Keep the
profile ID, interface, AP mode, and shared IPv4 address from the example.

While the Pi is connected to home Wi-Fi on `wlan0`, run:

```sh
sudo ./deploy/configure-hotspot.sh /run/improvdsp-hotspot.nmconnection
sudo rm /run/improvdsp-hotspot.nmconnection
```

The installer detects the active Wi-Fi client UUID, installs the protected hotspot
profile and startup selector, and enables both saved profiles for automatic
connection. It installs `dnsmasq-base` if needed. It leaves the active connection
in place; the selector runs at the next boot.

When configuring over Ethernet, or while the hotspot is active, select the saved
home Wi-Fi profile explicitly:

```sh
nmcli -f NAME,UUID,TYPE connection show
sudo ./deploy/configure-hotspot.sh /run/improvdsp-hotspot.nmconnection HOME_WIFI_UUID
```

Replace `HOME_WIFI_UUID` with the chosen client's UUID. The installer rejects
Ethernet and AP profiles as the home client. Delete the private `/run` copy after
installation; the installed password remains in the root-owned mode-600 file
`/etc/NetworkManager/system-connections/improvdsp-hotspot.nmconnection`.

## What happens at startup

`improvdsp-network.service` waits up to 20 seconds for the Wi-Fi adapter to leave
NetworkManager's unmanaged/unavailable states. This matters because the device
can exist before its Wi-Fi supplicant is ready. If it stays unavailable, systemd
retries the selector after 10 seconds.

The selector then tries the saved home profile for up to 25 seconds. If that
fails, it starts the saved **ImprovDSP** hotspot on 2.4 GHz channel 6, with
DHCP/DNS and the Pi at `10.42.0.1/24`. The hotspot has autoconnect priority `-999`;
the explicit selector ensures home Wi-Fi is tried first even when the initial
scan has not discovered it yet.

An active hotspot stays active until the Pi is restarted or the connection is
switched manually. Restarting at home tries home Wi-Fi again. Ethernet remains
usable in either mode. At home, open `http://improv.local:8080/` if the Pi's
hostname is `improv`, or use its current address and port 8080.

## At the venue

1. Power on the Pi with its system drive attached.
2. Join **ImprovDSP** on the phone using the password chosen during setup.
3. Stay connected if the phone reports that Wi-Fi has no internet.
4. Open **http://10.42.0.1:8080/**, including `http://` and `:8080`.

Microphone input and recording require the USB audio interface connected to the
Pi. Recordings stay in the configured recording directory on the Pi's storage.

## Check or switch networks

```sh
systemctl status improvdsp improvdsp-network
journalctl -b -u improvdsp-network
nmcli -f DEVICE,STATE,CONNECTION device status

# Run the normal home-first selector now; switching interrupts Wi-Fi clients.
sudo systemctl restart improvdsp-network

# Start the hotspot manually; use Ethernet to keep SSH connected during a test.
sudo nmcli connection up improvdsp-hotspot ifname wlan0
```

The readiness fix, normal home Wi-Fi startup, and a separate reboot with a
deliberately absent client network were verified on a Pi 5. The fallback test
reached the hotspot after its 25-second client timeout and served the control
page with HTTP 200. Live audio validation still requires the UMC22.

References: [NetworkManager keyfiles](https://networkmanager.dev/docs/api/latest/nm-settings-keyfile.html),
[nmcli](https://networkmanager.dev/docs/api/latest/nmcli.html).
