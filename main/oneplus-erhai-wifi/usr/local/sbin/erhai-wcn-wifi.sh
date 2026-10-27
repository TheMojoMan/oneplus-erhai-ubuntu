#!/bin/bash
# Autostart the internal WCN7880 WiFi at boot.
# Sequence: PCIe EP -> cnss2 (retry) -> fs_ready (cold-boot-cal) -> WAIT cal done
#           -> SETTLE -> wlan.ko (qcacld mission) -> detect STA -> hand to NM.
# Run as root (systemd oneshot). Idempotent.
#
# NO network name lives here by design. The script only brings the internal
# station up and hands it to NetworkManager; NM auto-connects the last-used
# in-range saved network on its own (autoconnect ordering = autoconnect-priority
# then last-connected timestamp). Saved profiles must have autoconnect=yes and an
# UNPINNED interface-name so they can bind whatever the station enumerates as
# (wlan0 vs wlp1s0 races across boots).
#
# The internal WCN presents 3 netdevs on PCI 0000:01:00.0: the station (type
# "managed"), a P2P-device, and a NAN iface (the NAN one is what predictable
# naming renames to "wlp1s0" -- NOT the station). Interface names are not stable
# (an external dongle, if present, races for wlan0), so we DETECT the station by
# type=managed rather than hard-coding a name. Future default: no dongle, so the
# internal station is the sole managed wifi device.
FS=/sys/devices/platform/soc@0/b0000000.qcom,cnss-peach/fs_ready
EP=/sys/bus/pci/devices/0000:01:00.0
SETTLE=${SETTLE:-10}                          # let cal tear down before host driver

say(){ echo "erhai-wcn: $*"; }

# Crash mitigation (~50%->? cold-boot SoC reset): zero the PCIe cal->mission
# pre-retrain settle. The crash clusters in that window (breadcrumb "erhai-cc: 3"
# = WCN powered-off-but-reachable after deinit, before resume_noirq) — an MMIO
# access from another context hits the dead device -> external abort -> SoC
# reset (pci_cfg_access_lock already rules out config-space accessors). The
# LOAD-BEARING cold reset is the 3062ms WLAN_EN de-assert INSIDE resume_noirq,
# not this pre-settle, so zeroing it shrinks the exposure without warm-EE risk.
# Built-in module param, writable at runtime; set before any cold-cycle triggers.
SETTLE_MS_PARAM=/sys/module/pcie_qcom/parameters/qcom_pcie_wcn_relink_settle_ms
[ -w "$SETTLE_MS_PARAM" ] && echo "${ERHAI_WCN_SETTLE_MS:-0}" > "$SETTLE_MS_PARAM"

# Echo the internal station netdev name (type=managed on the WCN PCIe EP), if any.
find_sta(){
  local d n t
  for d in "$EP"/net/*; do
    [ -e "$d" ] || continue
    n=$(basename "$d")
    t=$(iw dev "$n" info 2>/dev/null | awk '/type/{print $2}')
    [ "$t" = "managed" ] && { echo "$n"; return 0; }
  done
  return 1
}

STA=$(find_sta) || STA=""
if [ -n "$STA" ]; then
  say "internal station $STA already present — skipping load."
else
  # Wait for the WCN PCIe endpoint. At boot the pwrseq cold-cycles WLAN_EN
  # (de-assert ~3s, re-assert ~15s) and the link trains only after; cnss2 loading
  # before the EP is up fails its PCI probe (no fs_ready). The EP enumerates
  # independently, so gate on it.
  say "waiting for WCN PCIe endpoint ($EP)..."
  for i in $(seq 1 40); do [ -e "$EP" ] && break; sleep 1; done
  [ -e "$EP" ] || { say "WCN PCIe endpoint never appeared"; exit 1; }

  # Load cnss2, retry-reloading until it probes and creates fs_ready.
  say "loading cnss2 (retry until fs_ready)..."
  loaded=0
  for attempt in 1 2 3 4 5; do
    modprobe cnss2 || true
    for i in $(seq 1 15); do [ -e "$FS" ] && break; sleep 1; done
    [ -e "$FS" ] && { loaded=1; break; }
    say "cnss2 probe failed (fs_ready absent), reload attempt $attempt..."
    rmmod cnss2 2>/dev/null || true
    sleep 3
  done
  [ "$loaded" = 1 ] || { say "cnss2 never created fs_ready"; exit 1; }
  sleep 2

  say "triggering cold-boot-cal (fs_ready=1)..."
  echo 1 > "$FS"

  say "waiting for cold-boot-cal to complete..."
  ok=0
  for i in $(seq 1 60); do
    dmesg | grep -q "Calibration completed successfully" && { ok=1; break; }
    sleep 1
  done
  [ "$ok" = 1 ] || { say "calibration did not complete"; exit 1; }

  say "cal done. settling ${SETTLE}s before host driver..."
  sleep "$SETTLE"

  say "loading wlan (qcacld -> mission)..."
  modprobe wlan || { say "modprobe wlan failed"; exit 1; }

  say "waiting for internal station netdev..."
  for i in $(seq 1 30); do STA=$(find_sta) && break; sleep 1; done
  [ -n "$STA" ] || { say "internal station never appeared"; exit 1; }
fi
say "internal station = $STA"

ip link set "$STA" up || true

# === 5GHz-at-cold-boot ===
# The real gate on "5GHz only shows after connecting to 2.4G" is NOT country/reg
# (proven: US and DE both made no difference) -- it is scan_mode_6ghz. The qcacld
# platform default is SCAN_MODE_6G_PSC_DUTY_CYCLE(4), which fills ALL ~59 6GHz
# channels (5955-7115) into every scan request. On WCN7880 while DISCONNECTED
# that oversized sweep makes the fw drop the 5GHz results -- a full scan visits
# 5180 but returns nothing, while a scan that omits 6GHz (or a directed 5180
# scan) finds the AP fine. Associating to 2.4G tames 6GHz (RNR / connected-mode
# dwell) which is why 5GHz appeared only after connecting. Fix lives in the INI:
#   scan_mode_6ghz=0   (SCAN_MODE_6G_NO_CHANNEL -- drop 6GHz from scan requests)
# so NetworkManager's autonomous scans become 2.4+5GHz and 5GHz shows at boot.
#
# Country is still set so indoor UNII-1 (5180, the common home-5G channel) is
# ACTIVE-scannable rather than world-passive. Plain `iw reg set` CANNOT do this:
# hdd_reg_notifier drops user hints that are not NL80211_USER_REG_HINT_CELL_BASE,
# so the request never reaches ucfg_reg_set_country. The qcacld android private
# command "DRIVER COUNTRY <cc>" (drv_cmd_country -> hdd_reg_set_country) does
# reach it; wl_drvcmd issues it via SIOCDEVPRIVATE+1. Needs INI
# gCountryCodePriority=1 (else hdd_reg_set_country returns "user country not
# allowed"). Override country via ERHAI_WCN_COUNTRY.
if [ -x /usr/local/sbin/wl_drvcmd ]; then
  /usr/local/sbin/wl_drvcmd "$STA" COUNTRY "${ERHAI_WCN_COUNTRY:-DE}" || true
  sleep 1
fi
# Kick one scan so 5GHz populates the cfg80211 cache before NM asks.
iw "$STA" scan >/dev/null 2>&1 || true
sleep 1

# Hand the station to NetworkManager and let IT auto-connect the last-used saved
# network (NM autoconnect ordering = autoconnect-priority then last-connected
# timestamp). No network name here.
say "handing $STA to NetworkManager for last-used autoconnect..."
nmcli device set "$STA" managed yes 2>/dev/null || true

con=""
for i in $(seq 1 40); do
  nmst=$(nmcli -t -f DEVICE,STATE device 2>/dev/null | awk -F: -v d="$STA" '$1==d{print $2}')
  con=$(nmcli -t -f DEVICE,CONNECTION device 2>/dev/null | awk -F: -v d="$STA" '$1==d{print $2}')
  [ "$nmst" = "connected" ] && break
  sleep 1
done
say "NM $STA state: ${nmst:-unknown} connection: ${con:-none}"

say "done. $STA:"
ip -br addr show "$STA" | sed 's/^/erhai-wcn:   /'
