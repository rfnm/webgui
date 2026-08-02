# webgui — the RFNM Blue on-board web stack

Everything an RFNM Blue serves on port 80: the web GUI, the daemons behind it, and the
machine API that both share. The GUI is just the first client of that API — anything a
browser can do here, a script can do too.

## What's in the repo

| path | what it is |
|---|---|
| `daemon/spectrumd.cpp` | RTSA/spectrum daemon: librfnm local-transport RX (CS16, up to 122.88 MSps) → OpenCL FFT pipeline on the i.MX8MP GC7000UL GPU → RFSP v1 binary WebSocket on `127.0.0.1:7681` (proxied at `/ws`, also a declared direct data port). Includes the SigMF IQ recorder and the sweep compositor. |
| `daemon/appd.cpp` | App manager: scans `/rfnm/apps/<id>/app.json` manifests, owns the single-owner radio lease, generates the per-app nginx routes, serves the `/api/v1` control plane. Dependency-free static C++. |
| `daemon/ismmon/` | `ismmond` — 2.4 GHz ISM monitor (RX only): BLE LE 1M advertising decode plus Wi-Fi burst detection over one wideband capture; reuses spectrumd's GPU FFT engine (`fft_engine.h`). |
| `ddns/` | `ddnsd` — dynamic-DNS client. Registers `<name>.rfnm.me` (a board with no stored name self-registers as `<serial>.rfnm.me`) against the rfnm.me API, re-registers on address changes, and persists its config in the motherboard EEPROM settings exposed by the kernel. |
| `calib/` | `rfnm-calib-fetch` — factory-calibration distributor. For every daughterboard EEPROM found in sysfs it fetches `https://api.rfnm.me/calib/<serial>`, stores the blob at `/lib/firmware/rfnm/calib_<SERIAL>.json`, and derives vsgd's dBm power table. Runs at boot and 6-hourly via the shipped systemd timer. |
| `www/` | The GUI: a single-page, plain-JS, no-build-step, no-CDN app served by the on-board nginx. One full-screen pane per app (RTSA waterfall, VSG, LoRa, AIS, ADS-B, clock gen, board I/O, ISM monitor, settings). |
| `daemon/manifests/` | The stock `app.json` manifests appd installs from. |
| `daemon/*.service`, `daemon/nginx-site.conf` | systemd units for every daemon and the nginx site (`/` static GUI, `/api/` → appd, `/ws` → spectrumd). |
| `daemon/board.conf.*` | `/rfnm/board.conf` templates: board identity + TX policy (`TX_ALLOWED`) read by appd and vsgd. |
| `API.md` | **The normative machine API spec**: `/api/v1` control plane (state document, radio lease, per-app rf/config, logs, firmware-update arming) and the `/api/app/<id>/` per-app domain APIs. `rfnmctl` is the reference client. |
| `PROTOCOL.md` | RFSP v1, the binary spectrum WebSocket protocol (SPECTRA/STATUS/PATHS frames, client CONFIG/ACK, ACK-driven flow control). C definitions in `rfnm_spectrum_protocol.h`; `www/spectrum.js` is the reference decoder. |

## Hardware

Runs on the RFNM Blue motherboard (i.MX8MP, 4× Cortex-A53, NXP LA9310 baseband) with any
daughterboard. spectrumd/ismmond require the on-board GPU and the librfnm local transport,
so they only run on the device itself.

## Build

All daemons are cross-compiled aarch64 on a host machine — never on the board. spectrumd
and ismmond link librfnm statically and resolve transitive libraries (OpenCL, libusb,
spdlog) from a board root filesystem used as the sysroot; appd, ddnsd and rfnm-calib-fetch
are dependency-free static binaries.

```sh
# spectrumd, ismmond, appd (needs a board rootfs + a built librfnm)
RFNM_SYSROOT=/path/to/board-rootfs LIBRFNM=/path/to/librfnm make -C daemon
make -C daemon check          # host-side unit tests + arch sanity on every binary

# ddnsd and rfnm-calib-fetch (static, no sysroot needed)
make -C ddns  && make -C ddns test        # selftest under qemu-aarch64
make -C calib && make -C calib test
```

The easy path: the SDK at [github.com/rfnm/sdk](https://github.com/rfnm/sdk) clones and
builds the whole stack with the right sysroot wiring — `./rfnm-sdk build webgui`.

## Deploy / run

- `www/` + spectrumd → `/rfnm/spectrum/` on the board (nginx serves `/rfnm/spectrum/www`).
- appd, manifests and the per-app daemons → `/rfnm/apps/`.
- Install the `daemon/*.service` units; **start** them via systemd — appd is the exclusive
  app switcher and enforces the one-radio-owner rule, so per-app units are never enabled.
- After changing `www/`, bump the `?v=appN` cache-buster in `index.html`; after nginx
  config changes, `nginx -s reload`.

Then browse to `http://192.168.1.50/` (or the board's `<name>.rfnm.me`), or talk to
`http://192.168.1.50/api/v1/state` directly — see `API.md`.

## License

MPL-2.0, see `LICENSE`.
