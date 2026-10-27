# oneplus-erhai-pen — oneplus pad 3 (erhai) pen (stylus) support

Linux/BlueZ port of the stock OxygenOS **OplusPen** service
(`org.lineageos.pen`, source: `android_hardware_oplus/packages/Pen` in the
LineageOS erhai open-source tree).  It mirrors what stock does:

| Feature | Stock (Android) | This package (Linux) |
|---|---|---|
| Pen radio detection | CPS8601 wireless charger emits a `pencil_status / pencil_addr` kernel uevent when the pen is placed on the dock | BlueZ discovers the pen's BLE radio directly (`erhai-pen-monitor.py`), OUI-matched |
| Pairing | `OplusPen` service taps the notification, bonds by the reported MAC | auto `pair` + `trust` on first sighting (configurable), then `connect` |
| Reconnect | — | re-arms BLE discovery every `TICK_SECONDS` and reconnects while in range |
| **Auto-connect on the charger (0.4)** | — | **the monitor watches the CPS8601 charger debugfs `pen_present` for the ATTACH edge and starts a BLE scan + reconnect the moment the pen is placed on the coil** (the coil is what wakes the pen's radio).  It also reads the pen's BLE MAC from the coil (`ble_mac_addr`), learns it, and pages that address directly for instant reconnects.  `erhai-pen charger` shows the coil state.  `CONNECT_ON_ATTACH=no` disables it. |
| State for other tools | — | `/run/erhai-pen/{state,addr,updated}` |
| SoC toast on dock/undock | kernel `oplus_cps8601` debugfs (`pen_present`, `soc`) | `erhai-pen-notify.service`: **5 s toast** ("Pen attached — 75 %" / "Pen detached — 75 %") on attach/detach, delivered to the active graphical session (needs kernel **+b10** for the `soc` attribute; without it the toast still shows on dock events).  Since **0.3** the percentage comes from the pen over **BLE** (GATT Battery Service 0x180F/0x2A19 — see below), and only falls back to the coil `soc`. |
| Refresh-rate cap while writing | `config_penSupportedRefreshRate = 120.0` (erhai overlay) | not available on GNOME; no-op |
| Tip / position / pressure | NT36536 in-cell digitizer | same (touch driver) — nothing to do |
| Wireless pen charging (CPS8601 at i2c 0x41) | `oplus_cps8601.c` driver + `cps8601_firmware.bin` | **kernel +b9: ported** — `drivers/power/supply/oplus_cps8601.c` (regmap16 run-time window, 5 platform GPIOs, H-BoOST rail over pmic_glink, Q-cali, EPT/FOD, power expiry). No auto firmware-flash in v1 (the chip keeps its factory fw). Takes effect on the **next boot of `7.3.0-rc4-b9.efi`** (the running boot has neither the DT node nor the config). |

## Identifying the pen

The pen is a BLE HID input device (Goodix OUI `F8:6F:DE` or Maxeye OUI
`C0:87:06`; HID vendor `0x22D9` OPPO / `0x330A` Maxeye).  `pen.conf` lists
the OUIs; the radio sleeps when idle, so it only advertises while awake or
recently used.

Pairing is PIN-less (JustWorks) for the stock pen and is done automatically.
If a firmware revision ever asks for a numeric code, run
`erhai-pen pair <addr>` once from a terminal (bluetoothctl handles the code
prompt there).

## Pen state of charge (0.3)

The coil chip (CPS8601) *charges* the pen, but the pen's **battery level is a
Bluetooth value** — exactly as on stock Android, where the charger HAL writes it
into the charger driver as the writable `rx_soc` attribute (the chip's own coil
POWER packet only drives a charge-full check and is *not* what Android shows).
So `erhai-pen-monitor.py`, after connecting the pen:

* finds the GATT Battery Service (`BATTERY_SERVICE_UUID`, default `0x180F`) and
  its Battery Level characteristic (`BATTERY_LEVEL_UUID`, default `0x2A19`),
* reads it, and subscribes to notifications when the characteristic supports
  them (otherwise it re-reads on every sweep),
* publishes the value as `/run/erhai-pen/soc` (+ `soc_updated`).

`erhai-pen-notify.py` prefers that value for the toast while it is fresh
(`SOC_FRESH_SECONDS`, default 900) and a pen is connected; it falls back to the
coil `soc`, and shows "battery unknown" rather than a bogus `0 %`.
`SOC_ENABLE=no` turns the BLE read off.

Inspect it with:

    erhai-pen soc     # state files (state/addr/soc/soc_updated) + coil value
    erhai-pen gatt    # dump the connected pen's whole GATT tree

`erhai-pen gatt` is the way to find a vendor-specific battery characteristic if
the pen does not expose the standard `0x180F` service; set
`BATTERY_SERVICE_UUID` / `BATTERY_LEVEL_UUID` accordingly.

## Charge termination (0.5)

The coil used to keep charging the pen even at 100 %: the ported CPS8601 driver
recorded the ring's SoC but never acted on it (the vendor stops the coil at
100 % and re-arms it from the userspace `rx_soc`).  Since **kernel +b17** the
driver

* stops the coil when the pen reports itself full (100 %) over its POWER
  packet, and latches that so a removed/re-placed full pen is not topped up
  again, and
* exposes a writable `rx_soc` debugfs (`/sys/kernel/debug/cps-wls-charger/rx_soc`).

`erhai-pen-monitor.py` now writes the BLE SoC it already reads into that
`rx_soc`, so the driver re-arms charging once the pen has drained to/below
`rx,soc_threshold` (device tree, default 80).  `ERHAI_PEN_RX_SOC` overrides the
knob path; the wire to the coil only matters with kernel +b17 or newer.

## layout

- `/etc/erhai/pen.conf` — OUIs + auto-pair switch + timings + BLE SoC + charger trigger settings
- `/usr/local/sbin/erhai-pen-monitor.py` — the watcher (systemd: `erhai-pen.service`)
- `/usr/local/sbin/erhai-pen` — CLI: `status | soc | charger | gatt | scan [s] | pair [addr] | connect [addr] | forget [addr] | info [addr]`
- `/run/erhai-pen/{state,addr,soc,soc_updated}` — `state` = `1` when a pen is connected; `soc` = its battery level over BLE (%)
- `/run/erhai-pen/{known_addrs,charger_present,charger_mac}` — BLE addresses the monitor has learned + the last charger attach state / reported MAC

## history / source trail

- `LineageOS/android_device_oneplus_erhai` — `overlay/OPlusPenResTarget/`
  (device-specific `config_penSupportedRefreshRate = 120.0`), `device.mk`
  (`PRODUCT_PACKAGES += OplusPen`), pogo-keyboard `22d9:3869` idc/kl
- `LineageOS/android_hardware_oplus` — `packages/Pen/` (Kotlin `PenService.kt`,
  `BootCompletedReceiver.kt`, strings)
- `LineageOS/android_kernel_oneplus_sm8750-devicetrees` —
  `oplus-chg-erhai-common.dtsi`: `pencil-wireless-charger@41`
- `LineageOS/android_kernel_oneplus_sm8750-modules` —
  `oplus/kernel/charger/wireless_pen/oplus_cps8601.c` (GPL; uevent emitter,
  firmware updater for `cps8601_firmware.bin`)
- `TheMuppets/proprietary_vendor_oneplus_erhai` — firmware blobs
  (`wireless_pen/24976/cps8601_firmware.bin`, NT36536 touch image, scene config)