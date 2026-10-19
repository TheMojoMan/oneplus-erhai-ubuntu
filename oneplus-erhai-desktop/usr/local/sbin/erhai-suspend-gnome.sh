#!/bin/bash
# GNOME "Suspend" entry point (oneplus-erhai-desktop).
#
# systemd-suspend.service ExecStart override -> this wrapper -> the proven
# s2idle harness. Manual power-button wake ONLY (RTC_ALARM=0): the pm8941
# pwrkey is wake-armed during suspend, short press wakes, no unwedge timer.
#
# WHY QUIESCE=0 here (DELIBERATE — see MEMORY-2026-09-08 §20:10):
#   * NM reacts to logind's PrepareForSleep with its OWN FW-sanctioned radio
#     off BEFORE systemd-suspend.service even starts. If we ALSO call
#     `nmcli radio off` (harness STEP2), the two teardowns race the WCN:
#     - 19:27:10 crash: kworker died mid-mutex in
#       hif_exec_napi_kill/cnss_pci_dev_shutdown with preempt_count 1, then
#       suspend entry POR'd 6 s later
#     - 19:41/20:16 chip wedges: `hdd_open: VDEV SYNC TRANS fails -22` when
#       radio off->on is compressed into seconds (DRY run bug)
#   So on the GNOME suspend path, STEP2 only stops the discovery sweeper;
#   the radio-off is NM's job. QUIESCE=1 (manual harness runs) still does
#   the full radio off — the proven standalone path where NM is not
#   concurrently touching the chip.
#
# The harness also STOPs erhai-power-toggle.service across the sleep window
# (STEP2b/STEP6), so the wake press is never misread as a panel-toggle, and
# freezes the WCN psoc-idle timer (STEP2d/STEP6) so idle-shutdown can't race.
HARNESS=/usr/local/sbin/erhai-suspend-s2idle.sh
[ -x "$HARNESS" ] || HARNESS=/media/data/develop/erhai/tmp/gpu-hang-capture/erhai-suspend-s2idle.sh

export WAKE_SECS="${WAKE_SECS:-0}"
export RTC_ALARM="${RTC_ALARM:-0}"
export QUIESCE="${QUIESCE:-0}"
export DISPLAY_OFF_FIRST="${DISPLAY_OFF_FIRST:-1}"
export DRY="${DRY:-0}"
export RESTORE_WIFI="${RESTORE_WIFI:-1}"
export WIFI_WATCH_A="${WIFI_WATCH_A:-0}"
export WIFI_WATCH_B="${WIFI_WATCH_B:-0}"

echo "erhai-suspend-gnome: $0 -> $HARNESS (mem_sleep=$(cat /sys/power/mem_sleep 2>/dev/null)) QUIESCE=$QUIESCE DRY=$DRY"
exec /bin/bash "$HARNESS"
