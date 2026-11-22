# oneplus-erhai-wifi — WCN7860 backend

The OnePlus Pad 3 has one WiFi radio: a Qualcomm WCN7860 on PCIe function
`0000:01:00.0` (`pci17cb,110e`). Only one driver can own it. This package
selects **mainline ath12k** and keeps the vendor stack as a manual fallback.

## Selected backend: ath12k (1.6 and later)

- `etc/modprobe.d/erhai-wifi-backend.conf` blacklists `cnss2` and `wlan`
  (qcacld-3.0, which carries the same PCI modalias as ath12k).
- ath12k is *not* blacklisted, so udev binds `ath12k_pci` at boot, the driver
  downloads `ath12k/WCN7860/hw2.0/{amss,m3,aux_ucode}.bin` + the board set from
  `oneplus-erhai-firmware`, and NetworkManager manages the `wlp1s0` station.
- Requires the 7.3.4 kernel line at `+b6` or newer.
- No service is needed: the vendor units are shipped but disabled.

Boot-time notes that are normal, not errors:

    ath12k: failed to load firmware-2.bin: -2      (optional AMSS split file)
    ath12k ... failed to get ACPI DSM data: -95    (DT platform, no ACPI)

5 GHz is scanned passively by this firmware; country-code changes while
associated have crashed the firmware in testing, so leave the regdom alone. The
card reports a default MAC; NetworkManager's cloned MAC keeps it stable per
profile.

## Fallback: cnss2 + qcacld (the 1.5 default)

Needed only to compare backends or to bisect an ath12k regression. On the
running system:

    # 1. stop ath12k owning the endpoint, at the modprobe policy level
    printf 'blacklist ath12k\nblacklist ath12k_wifi7\n' \
        > /etc/modprobe.d/zz-blacklist-ath12k-manual.conf

    # 2. re-enable the vendor stack
    sed -i '/^blacklist \(cnss2\|wlan\)$/d' \
        /etc/modprobe.d/erhai-wifi-backend.conf

    # 3. reboot, then
    systemctl enable --now erhai-wcn-wifi.service
    systemctl enable --now erhai-wifi-discover.service

`erhai-wcn-holdfloor.service` (pwrseq rail floor for the cnss2 cold-boot
calibration) and `erhai-wcn-devcd.service` (persist a firmware devcoredump) are
part of the same fallback and are started by `erhai-wcn-wifi.service`'s
ordering, or enable them by hand.

Never unload the active WiFi driver over the only network path (no second
interface, no USB dongle): the pad loses its link and needs a reboot.

## Files of the fallback backend (shipped, not enabled)

    usr/local/sbin/erhai-wcn-wifi.sh          cnss2 -> qcacld bring-up
    usr/local/sbin/erhai-wcn-recover.sh       recovery helper
    usr/local/sbin/save-wcn-devcd.sh          devcoredump persister
    usr/local/sbin/erhai-wifi-discover.sh     UNII-1 discovery sweeper (qcacld)
    usr/local/sbin/wl_drvcmd                  vendor driver command tool
    usr/lib/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini
    usr/lib/systemd/network/80-erhai-wifi.link   pins the vendor netdev to wlan0
