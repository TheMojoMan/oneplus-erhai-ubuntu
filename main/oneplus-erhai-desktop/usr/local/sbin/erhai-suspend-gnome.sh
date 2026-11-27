#!/bin/bash
# GNOME "Suspend" entry point (oneplus-erhai-desktop).
#
# systemd-suspend.service ExecStart override -> this wrapper -> the proven
# suspend harness, in **mem_sleep=deep** (a real firmware suspend: the firmware
# parks APSS and the pad wakes on the power key / RTC alarm).  Manual
# power-button wake ONLY (RTC_ALARM=0): no unwedge timer in the product path.
#
# WHY DEEP IS THE DEFAULT (2026-11-22): the whole deepsleep series (display
# CESTA handoff, SoCCP, LPASS/SoundWire/regulator suspend states, rpmhpd
# presync floor, WCN CESTA override) only pays off with literal `mem_sleep=deep`;
# s2idle never asks the AOSS to shut down.  Proven on the released line
# (`+b6-deepsleep-05`): several real deep sleeps of 15-21 s, display handoff
# `mode2 reached` every time, clean resume (WLAN, panel, touch, folio).
# Two prerequisites, both handled here:
#   * SSC_QUIESCE=1 (harness STEP4d): a live ADSP sensor stream makes the
#     firmware return from PSCI SYSTEM_SUSPEND in 0.03-0.2 s.  The harness stops
#     the folio gate/watcher and iio-sensor-proxy before the entry and restarts
#     them after the resume.
#   * the charger poller must not run past i2c-5's suspend (the sc8547a driver
#     queues it on system_freezable_wq since `+b6-deepsleep-05`).
# MEM_STATE=deep is overridable (`systemctl set-environment MEM_STATE=s2idle`)
# for an A/B run; s2idle is still the fallback if deep ever wedges.
#
# WHY QUIESCE=0 here: NetworkManager already turns the radio off on logind's
# PrepareForSleep, before systemd-suspend.service starts, so calling
# `nmcli radio off` again (harness STEP2) would only duplicate the teardown.
# The vendor (cnss2/qcacld) races that motivated the old caution are gone:
# ath12k has been the only backend since wifi 1.6/1.7. QUIESCE=1 (manual
# harness runs) still turns the radio off before the sleep.
#
# The harness also STOPs erhai-power-toggle.service across the sleep window
# (STEP2b/STEP6), so the wake press is never misread as a panel-toggle.
HARNESS=${ERHAI_SUSPEND_HARNESS:-/usr/local/sbin/erhai-suspend-s2idle.sh}

export WAKE_SECS="${WAKE_SECS:-0}"
export RTC_ALARM="${RTC_ALARM:-0}"
export QUIESCE="${QUIESCE:-0}"
# real firmware suspend (see the header); s2idle only on explicit override
export MEM_STATE="${MEM_STATE:-deep}"
# DISPLAY_OFF_FIRST: 1 = blank the panel through gnome-shell (SetActive true, i.e.
# a real DPU/DSI teardown) BEFORE suspending — the 2026-09-16 design, chosen so an
# ABORTED suspend would not resume into a compositor with a dropped display.
# 2026-10-21: every suspend that could not be woken (black screen, power key dead,
# SoC POR ~26 s later) had it ON, and the one controlled run that resumed cleanly
# (`suspend_stats success=1`, `PM: suspend exit`, power key woke it) had it OFF
# (`erhai-suspend-trace.sh --no-display-off`).  The kernel suspends the display
# itself anyway, so blanking first only adds a compositor-side teardown the resume
# must undo.  Default flipped to 0 for the test; set DISPLAY_OFF_FIRST=1 (or
# `systemctl edit` the drop-in) to get the old behaviour back.
export DISPLAY_OFF_FIRST="${DISPLAY_OFF_FIRST:-0}"
export DRY="${DRY:-0}"
export RESTORE_WIFI="${RESTORE_WIFI:-1}"
export WIFI_WATCH_A="${WIFI_WATCH_A:-0}"
export WIFI_WATCH_B="${WIFI_WATCH_B:-0}"
# USB_GUARD (harness STEP2f/STEP6): unmount any filesystem on USB media and unbind the
# xHCI host across the sleep, then re-probe it after the resume -- a suspend taken WITH
# USB devices attached can otherwise kill the xHCI controller (`HC died`), drop every
# device and burn ~17 s in resume timeouts (MEMORY-2026-10-21.md §31; the same
# unbind/bind re-enumerates the whole tree with no replug).  USB_GUARD_ABORT=0 lets a
# suspend proceed with USB media still mounted (STEP2f then does NOT unbind).
export USB_GUARD="${USB_GUARD:-1}"
export USB_GUARD_ABORT="${USB_GUARD_ABORT:-1}"

echo "erhai-suspend-gnome: $0 -> $HARNESS (mem_sleep=$(cat /sys/power/mem_sleep 2>/dev/null)) MEM_STATE=$MEM_STATE QUIESCE=$QUIESCE DRY=$DRY"
exec /bin/bash "$HARNESS"
