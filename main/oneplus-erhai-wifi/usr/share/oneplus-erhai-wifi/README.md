# oneplus-erhai-wifi — WCN7860 / ath12k

The OnePlus Pad 3 has one WiFi radio: a Qualcomm WCN7860 on PCIe function
`0000:01:00.0` (`pci17cb,110e`). This package pins the backend to **mainline
ath12k**; the vendor cnss2/qcacld stack is not shipped any more.

## What the package does

- `etc/modprobe.d/erhai-wifi-backend.conf` blacklists `cnss2` and `wlan`
  (qcacld-3.0 carries the same PCI modalias as ath12k, so udev would otherwise
  autoload it and win the endpoint).
- ath12k is *not* blacklisted, so udev binds `ath12k_pci` at boot, the driver
  downloads `ath12k/WCN7860/hw2.0/{amss,m3,aux_ucode}.bin` + the board set from
  `oneplus-erhai-firmware`, and NetworkManager manages the `wlp1s0` station.
- Requires the 7.3.4 kernel line at `+b6` or newer.
- No service, no script: nothing has to run for the radio to come up.

Boot-time notes that are normal, not errors:

    ath12k: failed to load firmware-2.bin: -2      (optional AMSS split file)
    ath12k ... failed to get ACPI DSM data: -95    (DT platform, no ACPI)

5 GHz is scanned passively by this firmware; country-code changes while
associated have crashed the firmware in testing, so leave the regdom alone. The
card reports a default MAC; NetworkManager's cloned MAC keeps it stable per
profile.

## Removed in 1.7 (the 1.5 default)

`erhai-wcn-wifi.service`, `erhai-wcn-holdfloor.service`,
`erhai-wcn-devcd.service`, `erhai-wifi-discover.service`, `erhai-wcn-wifi.sh`,
`erhai-wcn-recover.sh`, `save-wcn-devcd.sh`, `erhai-wifi-discover.sh`,
`erhai-wifi-profile-normalize.sh`, `wl_drvcmd`,
`WCNSS_qcom_cfg.ini` and `80-erhai-wifi.link`. The suspend harness no longer
starts the discovery sweeper. The old units' enable symlinks are cleaned up by
this package's postinst.

Never unload the active WiFi driver over the only network path (no second
interface, no USB dongle): the pad loses its link and needs a reboot.
