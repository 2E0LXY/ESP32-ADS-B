# ADS-B feeder client

Forwards a local receiver's SBS/BaseStation output to the aggregator, for
Debian and Windows. Standard library Python 3 only — nothing to install.

## Do you need it?

Probably not. If your receiver runs **readsb** or **dump1090-fa**, it can push
SBS to us directly and the dashboard shows you the exact line:

```
--net-connector=feed.example.com,30117,sbs_out
```

That is fewer moving parts and one less process to keep alive. Use this client
when the direct route is unavailable:

- Windows receiver software with no outbound-connector support
- a managed image (PiAware, some prebuilt SD cards) whose config you would
  rather not edit
- a receiver you cannot reconfigure, but can reach over the network

## Getting the values

Account dashboard → your device → **Enable** under Feeder. It shows the
hostname and the port assigned to that device. The port is per-device: it is
how the aggregator tells your feed from everyone else's, so do not share it.

## Debian / Ubuntu / Raspberry Pi OS

```bash
sudo mkdir -p /opt/adsb-feeder
sudo cp adsb_feeder.py /opt/adsb-feeder/
sudo cp adsb-feeder.service /etc/systemd/system/
sudo systemctl edit adsb-feeder
```

In the editor that opens:

```ini
[Service]
Environment=ADSB_SERVER=feed.example.com
Environment=ADSB_PORT=30117
```

Then:

```bash
sudo systemctl enable --now adsb-feeder
journalctl -u adsb-feeder -f
```

Use `systemctl edit` rather than editing the unit file, so replacing the unit
later does not wipe your settings.

## Windows

1. Install Python 3 from python.org, ticking **Add python.exe to PATH**.
   Check it worked with `python --version` in a new terminal. If that prints
   the Microsoft Store message instead of a version, PATH did not get set -
   reinstall with the box ticked, or turn off the Store alias under Settings
   → Apps → Advanced app settings → App execution aliases.
2. Put `adsb_feeder.py` and `adsb-feeder.bat` in the same folder.
3. Edit `adsb-feeder.bat`, setting `ADSB_SERVER` and `ADSB_PORT`.
4. Double-click it to test.

Closing the window stops the feed. To run it unattended, use Task Scheduler:
Create Task → **Run whether user is logged on or not** → trigger **At
startup** → action **Start a program** → the `.bat`.

## When nothing arrives

Debian:

```bash
python3 adsb_feeder.py --check --server feed.example.com --port 30117
```

Windows — `python3` is not a command there, and typing it gets you the
Microsoft Store stub ("Python was not found; run without arguments to
install..."). Use `python`, or `py` if that fails:

```
cd C:\ESP32-ADS-B\server\tools
python adsb_feeder.py --check --server feed.example.com --port 30117
```

Substitute your own hostname and the port your dashboard shows for the
device - the values above are placeholders and will simply report the
aggregator unreachable.

That tests both ends separately and says which one is wrong. The usual causes:

- **Port 30005 instead of 30003.** 30005 is Beast binary; this needs SBS text
  on 30003. `--check` warns when the data does not look like SBS.
- **The feeder is not enabled** for that device on the dashboard, so nothing
  is listening on your port.
- **SBS output is off** in the receiver. In readsb/dump1090 that is
  `--net-sbs-port=30003`.

Nothing needs to be opened on your firewall — every connection is outbound.

## What it does

Copies bytes from the receiver to the aggregator, and reconnects to either
end with backoff when it drops. If the receiver goes silent for two minutes
it reconnects anyway, because a half-closed TCP connection looks identical to
a quiet sky from this side. It logs a message count every five minutes.
