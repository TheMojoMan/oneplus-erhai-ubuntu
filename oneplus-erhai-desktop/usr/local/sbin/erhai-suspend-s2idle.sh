#!/bin/bash
# erhai SUSPEND stage-1 harness — s2idle only, auto-wake by RTC alarm.
# (2026-09-16, after the first `echo mem` attempt aborted by WLAN and left the
#  session's display dead: see gpu-MEMORY-2026-09-12.md §2026-09-16.)
#
# WHY EVERY STEP IS HERE
#  * DISPLAY OFF FIRST: the previous attempt let the kernel suspend devices
#    with the DPU/DSI *live*; the suspend then aborted mid-way and the panel
#    rails were dropped+raised under a running compositor -> backlight on,
#    screen black, input dead.  Turning the panel off through the PROVEN
#    `erhai-display-toggle.sh off` path first means the compositor's pipeline
#    is already blanked, so an abort resumes into a state it understands.
#  * QUIESCE WLAN: the abort was `pm_wakeup_pending()` -> `pm_abort_suspend`,
#    and the only in-tree `pm_system_wakeup()` callers are cnss2/qcacld
#    (cnss2/main.c:1973 Dev-SOL IRQ, cnss2/pci.c:8364 WLAN wake IRQ,
#    qcacld hif_exec.c:1129 / hif_main.c:3037,3112 / qdf_lock.c:429).
#    Stopping traffic + the discover sweeper removes the RX/wake IRQs that
#    fire inside the suspend window.
#  * RTC ALARM: /sys/class/rtc/rtc1/wakealarm with the **+N** form. rtc1
#    (rtc-pm8xxx) counts in its OWN namespace (since_epoch ~5.6e5, wall clock
#    1.79e9) and rtc0 (rtc-efi) cannot be read at all, so an absolute wall
#    clock value never fires (and rtcwake refuses with EIO).  `+N` bases the
#    alarm on the RTC's own clock -> fires N s later.
#    rtc-pm8xxx.c:653 `devm_pm_set_wake_irq(alarm_irq)` = real wake IRQ.
#  * POWER BUTTON backstop: pm8941-pwrkey.c:227 `enable_irq_wake()` in its
#    suspend callback; `power/wakeup=enabled`.  Owner can always wake it.
#  * LONGS: everything is appended+fsync'd to the survival log, so a silent
#    SoC POR still leaves the pre-crash lines on sda19.
#
# USAGE (always as a SYSTEM unit so it survives session teardown):
#   systemd-run --unit=erhai-suspend-s2idle --collect \
#     --setenv=WAKE_SECS=25 --setenv=QUIESCE=1 --setenv=DISPLAY_OFF_FIRST=1 \
#     /bin/bash /media/data/develop/erhai/tmp/gpu-hang-capture/erhai-suspend-s2idle.sh
#
# KNOBS
#   WAKE_SECS=25        RTC alarm = now + this (auto-resume; also the timeout)
#   RTC_ALARM=1         0 = do NOT arm the RTC alarm (owner presses power to
#                          wake). Also skips the +WAKE_SECS+45 unwedge timer
#                          (that net would unblank the panel mid-sleep while
#                          waiting for a power press — wrong for manual wake).
#   QUIESCE=1           stop discover sweeper + `nmcli radio wifi off` first
#   DISPLAY_OFF_FIRST=1 run erhai-display-toggle.sh off before suspending
#   DRY=0               DRY=1 = do everything except `echo mem`
#   RESTORE_WIFI=1      bring wifi back at the end
#   MEM_STATE=s2idle    value written to /sys/power/mem_sleep
#   PARK_GPU=0          1 = unpin GPU (echo auto) and wait for a FRESH runtime
#                          park right before entering s2idle, so the display
#                          teardown happens with the GPU ALREADY collapsed (the
#                          serialization that made display OFF->ON survivable at
#                          08:02-08:15 on this kernel). ABORT if it cannot park
#                          within PARK_WAIT — never force an unverified state.
#   PARK_WAIT=15        seconds budget for the GPU park confirmation
#   SETACTIVE_FIRST=0   1 = issue a REAL display blank (gnome-shell ScreenSaver
#                          SetActive true) before PARK_GPU. Backlight-only off
#                          (DISPLAY_OFF_FIRST) keeps the DPU/DSI pipeline + a
#                          live compositor alive, so the GPU can never park
#                          (proven 15:35 DRY: 15x active). A real blank stops
#                          mutter, letting runtime-PM actually suspend the GPU.
set -u

WAKE_SECS=${WAKE_SECS:-25}
RTC_ALARM=${RTC_ALARM:-1}
QUIESCE=${QUIESCE:-1}
DISPLAY_OFF_FIRST=${DISPLAY_OFF_FIRST:-1}
DRY=${DRY:-0}
RESTORE_WIFI=${RESTORE_WIFI:-1}
MEM_STATE=${MEM_STATE:-s2idle}
PARK_GPU=${PARK_GPU:-0}
PARK_WAIT=${PARK_WAIT:-15}
SETACTIVE_FIRST=${SETACTIVE_FIRST:-0}
USE_WAKEUP_COUNT=${USE_WAKEUP_COUNT:-0}   # 0: unusable here, see STEP4b
WIFI_WATCH_A=${WIFI_WATCH_A:-60}         # post-resume: wifi UNTOUCHED watch
WIFI_WATCH_B=${WIFI_WATCH_B:-60}         # post-resume: after explicit re-enable
# USB_GUARD (STEP2f): a suspend taken WITH USB devices attached killed the xHCI host
# on resume (`HC died; cleaning up`, every device dropped, ~17 s of resume timeouts --
# MEMORY-2026-10-21.md §31).  Until the kernel side is fixed, STEP2f unmounts any
# filesystem on USB media and then UNBINDS the xHCI host, and STEP6 re-binds it (the
# tree re-enumerates with NO replug -- proven live 23:33).  SAFETY: if a USB
# filesystem cannot be unmounted the suspend is ABORTED (an unclean removal in the
# middle of the sleep is a data-loss risk); USB_GUARD_ABORT=0 = warn and suspend anyway
# WITHOUT unbinding (the old behaviour = HC-death risk).
USB_GUARD=${USB_GUARD:-1}
USB_GUARD_ABORT=${USB_GUARD_ABORT:-1}
D=/media/data/develop/erhai/tmp/gpu-hang-capture
RESUMEDF=/run/erhai-suspend-resumed

LOG=/media/data/develop/erhai/tmp/gpu-hang-capture/suspend-s2idle.log
DONEF=/run/erhai-suspend-done
SURVLOG=/usr/local/sbin/erhai-surv-log
TOGGLE=/usr/local/sbin/erhai-display-toggle.sh
RTC=/sys/class/rtc/rtc1
# ...but the PMIC RTC's INDEX is not stable: kernel +b8 (2026-09-29) registered
# the very same rtc-pm8xxx device as rtc0 (it was rtc1 before), so the hardcoded
# path silently stopped arming the alarm ("$RTC/wakealarm not writable") and the
# RTC auto-wake has been DEAD since.  Resolve it BY DRIVER NAME, never by index.
for _d in /sys/class/rtc/rtc*; do
    [ -e "$_d/device/driver" ] || continue
    if [ "$(basename "$(readlink -f "$_d/device/driver")")" = rtc-pm8xxx ]; then
        RTC="$_d"
        break
    fi
done
BL=/sys/class/backlight/ktz8868-backlight
BATT=/sys/class/power_supply/bq27z561-0
GPU=/sys/devices/platform/soc@0/3d00000.gpu/power
GENPD=/sys/kernel/debug/pm_genpd/pm_genpd_summary
USER_NAME=themojoman
UID_N=$(id -u "$USER_NAME")
RT=/run/user/$UID_N

# A pad that is not our dev workspace has no /media/data/develop/erhai/tmp: an
# `exec >>` into a missing directory is FATAL to the whole suspend (the unit dies
# before it can even blank the panel), so fall back to /var/log (MEMORY-2026-10-21
# §2: the three hardcoded dev paths in the panel/suspend helpers).
if ! mkdir -p "$(dirname "$LOG")" 2>/dev/null || [ ! -w "$(dirname "$LOG")" ]; then
    LOG=/var/log/erhai-suspend-s2idle.log
fi
exec >> "$LOG" 2>&1
rm -f "$DONEF"
say() { echo "[$(date +%T) up=$(cut -d' ' -f1 /proc/uptime)] $*"; \
        [ -x "$SURVLOG" ] && "$SURVLOG" "suspend-s2idle: $*" 2>/dev/null; }
kmsg() { echo "erhai-suspend: $*" > /dev/kmsg 2>/dev/null; }
# live pm_test selection (the BRACKETED entry of /sys/power/pm_test).  Read out at
# STEP4b (kmsg, survives a reset) and at STEP5, so the log PROVES which phase test
# was in effect.  WHY: the tracer sets pm_test, but its own restore_tweaks used to
# fire 1-2 s into the run (systemd-run without --wait) and silently put it back to
# `none` before STEP5 -- so a "--pm-test freezer" run was really a full suspend and
# nothing in the log said so.
sel_pm_test() { tr ' ' '\n' < /sys/power/pm_test 2>/dev/null | sed -n 's/^\[\(.*\)\]$/\1/p'; }
snap() { # one-line state: panel + gpu + genpd
    printf 'bl=%s gpu=%s/%s gdsc[%s]\n' \
        "$(cat $BL/actual_brightness 2>/dev/null)" \
        "$(cat $GPU/control 2>/dev/null)" "$(cat $GPU/runtime_status 2>/dev/null)" \
        "$(grep -E '^(mdss_gdsc|mdss_int2_gdsc)' $GENPD 2>/dev/null | awk '{printf "%s=%s ",$1,$2}')"
}
ss() { runuser -u "$USER_NAME" -- env XDG_RUNTIME_DIR="$RT" \
        DBUS_SESSION_BUS_ADDRESS="unix:path=$RT/bus" timeout 10 gdbus call --session \
        --dest org.gnome.Shell --object-path /org/gnome/ScreenSaver \
        --method org.gnome.ScreenSaver.SetActive "$1" 2>&1; }

# --- STEP2f / STEP6 USB quiesce helpers -------------------------------------
# See the USB_GUARD comment above and MEMORY-2026-10-21.md §31-§33.  Only ever
# touches xhci-hcd.<N>.auto -- NEVER dwc3-qcom / qcom-qmp-combo-phy (both are
# forbidden: unbinding them bricks or hangs the port, see .clinerules).
USB_UNBOUND=/run/erhai-suspend-usb-unbound
usb_xhci() { ls -d /sys/bus/platform/devices/xhci-hcd.*.auto 2>/dev/null | head -1; }
usb_nonroot_devs() { lsusb 2>/dev/null | grep -vc 'Linux Foundation'; }
usb_media_mounts() {   # "<device> <mountpoint>" for MOUNTED usb block devices
    command -v lsblk >/dev/null 2>&1 || return 0
    lsblk -rno NAME,TRAN,MOUNTPOINT 2>/dev/null | awk '$2 == "usb" && $3 != "" {print "/dev/" $1, $3}'
}
usb_quiesce() {        # unmount USB media, then unbind the xHCI host; 3 = media busy
    local X i left
    if [ "$(usb_media_mounts | wc -l)" -gt 0 ]; then
        usb_media_mounts | while read -r dev mp; do
            say "STEP2f unmounting USB media: $dev on $mp"
            if [ -x /usr/bin/udisksctl ]; then
                udisksctl unmount -b "$dev" --no-user-interaction >/dev/null 2>&1
            fi
            mountpoint -q "$mp" 2>/dev/null && umount "$mp" 2>/dev/null
        done
        sync
        left=$(usb_media_mounts | wc -l)
        if [ "$left" -gt 0 ]; then
            say "STEP2f !!! $left USB filesystem(s) STILL MOUNTED — will not remove the device"
            usb_media_mounts | while read -r dev mp; do say "STEP2f     still mounted: $dev on $mp"; done
            return 3
        fi
        say "STEP2f USB media unmounted cleanly"
    fi
    X=$(usb_xhci)
    [ -n "$X" ] || { say "STEP2f no xHCI host (port not in host mode) — nothing to unbind"; return 0; }
    if [ "$(usb_nonroot_devs)" = 0 ]; then
        say "STEP2f no USB devices attached — nothing to unbind"
        return 0
    fi
    say "STEP2f unbinding the xHCI host ${X##*/} ($(usb_nonroot_devs) device(s) disappear for the sleep)"
    kmsg "USB unbind ${X##*/}"
    if ! echo "${X##*/}" > /sys/bus/platform/drivers/xhci-hcd/unbind 2>/dev/null; then
        say "STEP2f !!! unbind FAILED — suspending WITH devices attached (xHCI HC-death risk)"
        return 1
    fi
    : > "$USB_UNBOUND"
    i=0
    while [ "$i" -lt 10 ]; do
        [ "$(usb_nonroot_devs)" = 0 ] && break
        sleep 1; i=$((i+1))
    done
    say "STEP2f devices now: $(usb_nonroot_devs) (after ${i}s)"
    return 0
}
usb_restore() {        # re-probe the host iff STEP2f unbound it (idempotent, trap-safe)
    [ -e "$USB_UNBOUND" ] || return 0
    local X i
    X=$(usb_xhci)
    rm -f "$USB_UNBOUND"
    if [ -z "$X" ]; then
        say "STEP6 usb: xHCI host device gone — cannot re-probe (usb-host-recover.sh is the fallback)"
        return 1
    fi
    say "STEP6 usb: re-binding ${X##*/} (the tree re-enumerates, no replug needed)"
    echo "${X##*/}" > /sys/bus/platform/drivers/xhci-hcd/bind 2>/dev/null
    i=0
    while [ "$i" -lt 15 ]; do
        [ "$(usb_nonroot_devs)" -gt 0 ] && break
        sleep 1; i=$((i+1))
    done
    say "STEP6 usb: $(usb_nonroot_devs) device(s) enumerated after ${i}s"
    kmsg "USB rebind done devices=$(usb_nonroot_devs)"
    return 0
}
# belt-and-braces: an abort path that exits after the unbind still re-binds (idempotent)
trap 'usb_restore 2>/dev/null || true' EXIT INT TERM

say "===== SUSPEND s2idle RUN START (WAKE_SECS=$WAKE_SECS QUIESCE=$QUIESCE DISPLAY_OFF_FIRST=$DISPLAY_OFF_FIRST DRY=$DRY MEM_STATE=$MEM_STATE) ====="
kmsg "START WAKE_SECS=$WAKE_SECS QUIESCE=$QUIESCE"
ORIG_MEM_SLEEP=$(cat /sys/power/mem_sleep 2>/dev/null | grep -oE '\[[a-z0-9]+' | tr -d '[')
say "pre: mem_sleep=[$(cat /sys/power/mem_sleep)] orig=${ORIG_MEM_SLEEP:-unknown} $(snap)"

# ---- safety net: independent unit that restores the panel even if this dies.
# Only with an RTC auto-wake: with RTC_ALARM=0 the owner presses power to
# resume, so a wall-clock unwedge would fire mid-sleep and unblank the panel —
# wrong. Skipped entirely in manual-wake mode.
if [ "$RTC_ALARM" = 1 ]; then
    systemd-run --unit=erhai-suspend-unwedge --collect --on-active=$((WAKE_SECS+45)) \
        /bin/bash -c "$TOGGLE on; $SURVLOG 'suspend-s2idle: UNWEDGE fired (safety net)'" \
        >/dev/null 2>&1 && say "safety net armed (+$((WAKE_SECS+45))s panel unblank)"
else
    say "safety net SKIPPED (RTC_ALARM=0 — manual power-button wake)"
fi

# ---- STEP 1: display off first (proven path)
if [ "$DISPLAY_OFF_FIRST" = 1 ]; then
    say "STEP1 display off -> $("$TOGGLE" off 2>&1; echo rc=$?)"
    sleep 2
    say "STEP1 post: $(snap)"
else
    say "STEP1 skipped"
fi

# ---- STEP 2: quiesce WLAN (remove the abort source)
# DRY guard: NEVER radio-off in a DRY run. A DRY run returns in ~2 s, so the
# radio off->on churn is compressed into milliseconds -> WCN VDEV wedge
# (`hdd_open: VDEV SYNC TRANS fails -22`, 19:39/19:41/20:16 — recoverable only
# by reboot). DRY only validates the plumbing, never the WLAN teardown.
if [ "$DRY" = 0 ] && [ "$QUIESCE" = 1 ]; then
    if systemctl is-active --quiet erhai-wifi-discover.service; then
        systemctl stop erhai-wifi-discover.service && say "STEP2 discover sweeper stopped"
    else
        say "STEP2 discover sweeper not active"
    fi
    say "STEP2 nmcli radio wifi off -> $(nmcli radio wifi off 2>&1; echo rc=$?)"
elif [ "$DRY" = 1 ] && [ "$QUIESCE" = 1 ]; then
    say "STEP2 SKIPPED radio off in DRY run (would wedge the WCN) — sweeper not stopped either"
fi

# ---- STEP 2c: WAIT until the WLAN is FULLY settled before `echo mem`.
# In the GNOME-menu path logind's PrepareForSleep makes NetworkManager tear the
# WCN down itself (its sleep inhibitor), racing our nmcli radio off — 19:27:10
# crash showed a kworker wedged in hif_exec_napi_kill/hdd_psoc_idle_shutdown/
# cnss_pci_dev_shutdown holding a mutex when `echo mem` ran 6 s later
# (preempt_count 1). Every SURVIVING run entered suspend with wlp1s0 reported
# `unavailable`. So: poll until the radio is NOT connected/connecting/
# activating, then require one clean `unavailable|unmanaged` reading, else ABORT
# (§"never force an unverified state") — a too-slow WLAN settle must never be
# carried into suspend.
if [ "$DRY" = 0 ] && [ "$QUIESCE" = 1 ]; then
    WLAN_SETTLE=${WLAN_SETTLE:-12}
    say "STEP2c waiting for WLAN to fully settle (budget ${WLAN_SETTLE}s)..."
    t_s0=$(cut -d' ' -f1 /proc/uptime)
    wl_settled=no
    i=0
    while [ "$i" -lt "$WLAN_SETTLE" ]; do
        wl=$(nmcli -t -f DEVICE,STATE dev 2>/dev/null | grep -E '^wlp' | head -1)
        st=${wl#*:}
        [ -z "$st" ] && st="(no device)"
        case "$st" in
            unavailable|unmanaged) wl_settled=yes; say "STEP2c t=${i}s state=$st -> SETTLED"; break ;;
            *) say "STEP2c t=${i}s state=$st (waiting...)" ;;
        esac
        sleep 1; i=$((i+1))
    done
    if [ "$wl_settled" = yes ]; then
        say "STEP2c WLAN settled: $(nmcli -t -f DEVICE,STATE dev 2>/dev/null | grep wlp | head -2 | tr '\n' ' ')"
    else
        say "STEP2c !!! WLAN did NOT settle within ${WLAN_SETTLE}s — ABORTING (never suspend mid-WLAN-teardown)"
        say "===== SUSPEND s2idle RUN END (ABORTED at STEP2c) ====="
        exit 3
    fi
else
    say "STEP2 quiesce skipped"
fi

# ---- STEP 2b: stop the power-watch service across the whole sleep window
# The power key that WAKES a manual-wake (RTC_ALARM=0) s2idle is ALSO queued
# in the evdev ring buffer and delivered to erhai-power-watch.py after resume
# (kernel input core: the event is only read by clients whose fd was open AT
# the time). With the watcher running it treats that same physical press as a
# NEW toggle and calls erhai-panel-power.sh — which, with no /run/erhai-panel.state
# marker and the panel still physically down, BLANKS the freshly-restored
# screen (seen 18:42:14-16: STEP6 display on -> nvt_ts_suspend). Stopping the
# service closes its fd, so the wake event is never delivered to it; the fresh
# open at STEP6 only reads events from after reopen. Restart in STEP6.
POWERWATCH_ACTIVE=0
if systemctl is-active --quiet erhai-power-toggle.service; then
    POWERWATCH_ACTIVE=1
    if systemctl stop erhai-power-toggle.service 2>/dev/null; then
        say "STEP2b power-watch service STOPPED (prevents wake-press double-toggle)"
    else
        say "STEP2b power-watch service stop FAILED (rc=$?) — double-toggle risk!"
    fi
else
    say "STEP2b power-watch service not active (nothing to stop)"
fi

# ---- STEP 2d: freeze the WCN psoc idle-shutdown timer for the sleep window.
# The 19:27:10 GNOME-suspend POR showed the WLAN idle-shutdown racing suspend:
# NM's PrepareForSleep radio-off triggers `hdd_psoc_idle_timeout_callback`
# (psoc idle timeouts after 250 ms in this kernel's config default) ->
# `cnss: Doing idle shutdown` -> NULL-pointer-deref in the wlan driver
# (htc_stop/hif teardown race) -> kernel tainted, then suspend entry 6 s later
# silently reset the SoC. The QDF timer multiplier
# (/sys/module/wlan/parameters/timer_multiplier, range 1..0xFFFFFFFF) scales
# EVERY qdf_delayed_work — including psoc_idle_timeout_work. Setting a huge
# value here makes the idle-shutdown effectively unreachable for the whole
# sleep, so it can never collide with `echo mem`. Restored in STEP6.
WLAN_TM=/sys/module/wlan/parameters/timer_multiplier
WLAN_TM_ORIG=""
if [ "$DRY" = 0 ] && [ -w "$WLAN_TM" ]; then
    WLAN_TM_ORIG=$(cat "$WLAN_TM" 2>/dev/null)
    if [ "$WLAN_TM_ORIG" != 1000000 ]; then
        echo 1000000 > "$WLAN_TM" 2>/dev/null && say "STEP2d WCN psoc-idle timer frozen (timer_multiplier 1 -> 1000000, was $WLAN_TM_ORIG)"
    else
        say "STEP2d WCN timer_multiplier already 1000000"
    fi
else
    if [ "$DRY" = 1 ]; then
        say "STEP2d SKIPPED in DRY run (timer churn unnecessary; real run freezes it)"
    else
        say "STEP2d $WLAN_TM not writable — psoc idle-shutdown could race suspend"
    fi
fi

# ---- STEP 2e: WLAN TEARDOWN GUARD (2026-10-21) — NEVER enter s2idle on a
# crashed / half-dead WLAN.  Owner's 4-test session on 2026-10-21: tests 1-3
# could not be woken with the power key and needed a HARD RESET, test 4 slept
# 20.2 s and woke normally.  The journal difference is the WLAN teardown NM does
# on logind's PrepareForSleep: the 250 ms psoc idle timer (`gInterfaceChangeWait`)
# fires `cnss: Doing idle shutdown` -> hdd_psoc_idle_shutdown -> ... ->
# hif_exec_napi_kill, which in the crashing runs Oopsed
# ("Unable to handle kernel paging request" + "kworker/u33:N exited with
# preempt_count 1") 0.1-0.2 s after "htc_stop: stopping hif layer" and left the
# cnss IDLE_SHUTDOWN event NEVER completed ("Completed event: IDLE_SHUTDOWN"
# count 0); `echo mem` 2.7-3.0 s later then never returned.  The surviving run
# had that SAME shutdown COMPLETE cleanly ("... ret: 0/0") before the suspend.
# STEP2d freezes the timer, but it runs AFTER NM's radio-off (logind's
# PrepareForSleep reaches NM before systemd-suspend.service starts), i.e. too
# late to matter — so instead of trying to win the race we refuse to sleep when
# it was lost.  Full analysis: MEMORY-2026-10-21.md §suspend.
WLAN_GUARD=${WLAN_GUARD:-1}
GUARD=/usr/local/sbin/erhai-suspend-wlan-guard.sh
[ -x "$GUARD" ] || GUARD=$D/erhai-suspend-wlan-guard.sh
if [ "$DRY" = 1 ]; then
    say "STEP2e WLAN guard SKIPPED in DRY run (no radio teardown happens)"
elif [ "$WLAN_GUARD" != 1 ]; then
    say "STEP2e WLAN guard DISABLED (WLAN_GUARD=$WLAN_GUARD) — no hard-reset protection"
elif [ -x "$GUARD" ]; then
    g_out=$("$GUARD" 2>&1); g_rc=$?
    printf '%s\n' "$g_out" | while IFS= read -r l; do say "STEP2e   $l"; done
    if [ "$g_rc" = 0 ]; then
        say "STEP2e WLAN guard PASSED — safe to suspend"
    else
        say "STEP2e !!! WLAN guard FAILED (rc=$g_rc) — ABORTING the suspend (this is the case that used to need a hard reset)"
        echo "wlan-guard-failed rc=$g_rc up=$(cut -d' ' -f1 /proc/uptime)" > /run/erhai-suspend-wlan-guard
        say "STEP2e restore: SetActive false -> $(ss false)"
        say "STEP2e restore: display on -> $("$TOGGLE" on 2>&1; echo rc=$?)"
        if [ "$PARK_GPU" = 1 ]; then
            echo on > "$GPU/control" 2>/dev/null
            say "STEP2e restore: gpu re-pinned: $(snap)"
        fi
        if [ -n "${ORIG_MEM_SLEEP:-}" ]; then
            echo "$ORIG_MEM_SLEEP" > /sys/power/mem_sleep 2>/dev/null
        fi
        if [ -n "$WLAN_TM_ORIG" ] && [ "$WLAN_TM_ORIG" != 1000000 ]; then
            echo "$WLAN_TM_ORIG" > "$WLAN_TM" 2>/dev/null
        fi
        if [ "$POWERWATCH_ACTIVE" = 1 ]; then
            systemctl start erhai-power-toggle.service 2>/dev/null
        fi
        # The crash that made the guard abort also leaves the qcacld driver broken
        # (`hdd_open: VDEV SYNC TRANS start fails -16`, NM shows no networks).
        # NEVER try to REPAIR that from here: in this state `rmmod wlan` HANGS in
        # D-state (2026-10-21: "Hard Unloading driver" then nothing, module left
        # GOING/refs -1, and the stuck unit then made the owner's plain reboot
        # hang too -> hard reset).  So: REPORT ONLY (read-only + bounded), and
        # tell the user to reboot with --force.  WLAN_GUARD_STATUS=0 skips it.
        if [ "${WLAN_GUARD_STATUS:-1}" = 1 ]; then
            WREC=/usr/local/sbin/erhai-wcn-recover.sh
            [ -x "$WREC" ] || WREC=$D/erhai-wcn-recover.sh
            if [ -x "$WREC" ]; then
                r_out=$(timeout 40 "$WREC" status 2>&1); r_rc=$?
                printf '%s\n' "$r_out" | while IFS= read -r l; do say "STEP2e wlan-state  $l"; done
                say "STEP2e wlan-state rc=$r_rc (read-only; a wedged driver needs 'systemctl reboot --force')"
            else
                say "STEP2e wlan-state: $WREC not found"
            fi
        fi
        say "STEP2e post-abort: $(snap)"
        say "===== SUSPEND s2idle RUN END (ABORTED at STEP2e — WLAN guard) ====="
        exit 3
    fi
else
    say "STEP2e $GUARD not found — guard skipped (no hard-reset protection)"
fi

# ---- STEP 2f: USB quiesce (unmount USB media, then unbind the xHCI host)
# WHY: with USB devices attached the resume can kill the xHCI controller (`HC died`),
# drop every device and burn ~17 s (MEMORY-2026-10-21.md §31).  Taking the host down
# before the sleep and re-probing it after it (STEP6) avoids that entirely -- the tree
# re-enumerates with no replug.  Data safety first: media is unmounted, and if that
# fails the suspend is ABORTED (USB_GUARD_ABORT=0 = suspend anyway, without unbinding).
if [ "$DRY" = 1 ]; then
    say "STEP2f USB guard SKIPPED in DRY run (no sleep happens)"
elif [ "$USB_GUARD" != 1 ]; then
    say "STEP2f USB guard DISABLED (USB_GUARD=$USB_GUARD) — devices stay attached (xHCI HC-death risk)"
else
    usb_quiesce; usb_rc=$?
    if [ "$usb_rc" = 3 ]; then
        if [ "$USB_GUARD_ABORT" = 1 ]; then
            say "STEP2f !!! USB media is still mounted — ABORTING the suspend (data safety)"
            echo "usb-busy up=$(cut -d' ' -f1 /proc/uptime)" > /run/erhai-suspend-usb-busy
            say "STEP2f restore: SetActive false -> $(ss false)"
            say "STEP2f restore: display on -> $("$TOGGLE" on 2>&1; echo rc=$?)"
            if [ "$PARK_GPU" = 1 ]; then
                echo on > "$GPU/control" 2>/dev/null
                say "STEP2f restore: gpu re-pinned: $(snap)"
            fi
            if [ -n "${ORIG_MEM_SLEEP:-}" ]; then
                echo "$ORIG_MEM_SLEEP" > /sys/power/mem_sleep 2>/dev/null
            fi
            if [ -n "${WLAN_TM_ORIG:-}" ]; then
                echo "$WLAN_TM_ORIG" > "$WLAN_TM" 2>/dev/null
            fi
            if [ "$POWERWATCH_ACTIVE" = 1 ]; then
                systemctl start erhai-power-toggle.service 2>/dev/null
            fi
            say "STEP2f post-abort: $(snap)"
            say "===== SUSPEND s2idle RUN END (ABORTED at STEP2f — USB media busy) ====="
            exit 3
        fi
        say "STEP2f USB_GUARD_ABORT=0: suspending WITH devices attached (xHCI HC-death risk)"
    fi
fi

# ---- STEP 3: arm the RTC alarm (our auto-wake) and verify the readback.
# RTC_ALARM=0 -> skip entirely (owner wakes with a short power press).
RTC_BASE=""
if [ "$RTC_ALARM" = 1 ]; then
    if [ -w "$RTC/wakealarm" ]; then
        echo 0 > "$RTC/wakealarm" 2>/dev/null
        base=$(cat "$RTC/since_epoch" 2>/dev/null)
        echo "+$WAKE_SECS" > "$RTC/wakealarm" 2>/dev/null
        rb=$(cat "$RTC/wakealarm" 2>/dev/null)
        say "STEP3 rtc alarm +$WAKE_SECS -> readback=[$rb] (rtc now=$base, expected=$((base+WAKE_SECS)))"
        [ -n "$rb" ] || say "STEP3 !!! ALARM NOT SET — power button is the only wake !!!"
    else
        say "STEP3 !!! $RTC/wakealarm not writable — power button is the only wake !!!"
    fi
else
    say "STEP3 SKIPPED (RTC_ALARM=0) — power button is the ONLY wake"
fi

# ---- STEP 4: select s2idle
say "STEP4 mem_sleep before=[$(cat /sys/power/mem_sleep)]"
echo "$MEM_STATE" > /sys/power/mem_sleep 2>/dev/null
say "STEP4 mem_sleep after=[$(cat /sys/power/mem_sleep)] state=[$(cat /sys/power/state)]"
# ---- STEP 4b: enable the wakeup-event gate (what systemd does on x86).
# Writing the current count to /sys/power/wakeup_count sets events_check_enabled
# + saved_count, so ANY wakeup-source event (including the RTC alarm IRQ's
# rtc_update_irq -> pm_stay_awake, which bumps combined_event_count) makes
# pm_wakeup_pending() true and terminates the sleep.  Without it, an arm64
# s2idle loop can only be exited by a HARD wake IRQ (pwrkey) — the alarm would
# fire its handler while the pad stayed asleep.
if [ "$USE_WAKEUP_COUNT" = 1 ]; then
    wc_ok=0
    for i in 1 2 3 4 5; do
        cnt=$(cat /sys/power/wakeup_count 2>/dev/null)
        if [ -n "$cnt" ] && echo "$cnt" > /sys/power/wakeup_count 2>/dev/null; then
            wc_ok=1; say "STEP4b wakeup_count gate SET (count=$cnt, attempt $i)"; break
        fi
        say "STEP4b wakeup_count attempt $i raced (event in flight); retrying"
        sleep 1
    done
    [ "$wc_ok" = 1 ] || say "STEP4b !!! gate NOT set — only the power button can end s2idle !!!"
else
    say "STEP4b wakeup_count gate disabled"
fi
kmsg "ENTER mem (mem_sleep=$MEM_STATE WAKE_SECS=$WAKE_SECS wc_gate=$USE_WAKEUP_COUNT PARK_GPU=$PARK_GPU pm_test=[$(sel_pm_test)])"

# ---- STEP 4c: OPTIONAL — REAL blank first, then park the GPU before s2idle
# Step 1 (SETACTIVE_FIRST): SetActive true = real DPU/DSI teardown under the
#   compositor.  This stops mutter from submitting frames, which is the only
#   way the GPU can reach runtime-suspend at all (backlight-only NEVER does —
#   proven 15:35 DRY: 15x active).
# Step 2 (PARK_GPU): unpin (echo auto) and wait for a FRESH runtime park, then
#   suspend immediately on the first `suspended` observation — the GPU is
#   ALREADY collapsed when the kernel tears the display down inside s2idle.
# If either cannot complete within its budget we ABORT — never write
# /sys/power/state from an unverified state.
if [ "$SETACTIVE_FIRST" = 1 ] || [ "$PARK_GPU" = 1 ]; then
    if [ "$SETACTIVE_FIRST" = 1 ]; then
        say "STEP4c REAL blank: SetActive true -> $(ss true)"
        sleep 3
        say "STEP4c REAL blank post: $(snap)"
        dmesg 2>/dev/null | grep -aE 'nvt_ts_panel_power|display on|display off' | tail -4 | while read -r l; do say "  dmesg| $l"; done
    fi
    if [ "$PARK_GPU" = 1 ]; then
        say "STEP4c PARK: echo auto -> $(echo auto > "$GPU/control" 2>&1; echo rc=$?)"
        say "STEP4c PARK: waiting for runtime_status=suspended (budget ${PARK_WAIT}s)..."
        t_p0=$(cut -d' ' -f1 /proc/uptime)
        parked=no
        i=0
        while [ "$i" -lt "$PARK_WAIT" ]; do
            st=$(cat "$GPU/runtime_status" 2>/dev/null)
            say "STEP4c park poll t=${i}s status=$st"
            if [ "$st" = suspended ]; then
                parked=yes
                break
            fi
            sleep 1; i=$((i+1))
        done
        t_p1=$(cut -d' ' -f1 /proc/uptime)
        say "STEP4c PARK: parked=$parked after_wait=$i s (uptime ${t_p0}->${t_p1})"
        if [ "$parked" != yes ]; then
            say "STEP4c !!! GPU never parked within ${PARK_WAIT}s — ABORTING (no suspend)"
            echo on > "$GPU/control" 2>/dev/null
            say "STEP4c abort: GPU re-pinned $(snap)"
            exit 3
        fi
        say "STEP4c PARK confirmed — proceeding to STEP5 immediately"
    fi
fi

# ---- STEP 5: THE SUSPEND
if [ "$DRY" = 1 ]; then
    say "STEP5 DRY run — not writing /sys/power/state"
else
    t0=$(cut -d' ' -f1 /proc/uptime)
    kmsg "GO uptime=$t0"
    soc_pre_cap=$(cat "$BATT/capacity" 2>/dev/null)
    soc_pre_chg=$(cat "$BATT/charge_now" 2>/dev/null)
    soc_pre_v=$(cat "$BATT/voltage_now" 2>/dev/null)
    say "STEP5 pre-suspend SOC: capacity=${soc_pre_cap}% charge_now=${soc_pre_chg}uAh voltage=$(awk -v v="$soc_pre_v" 'BEGIN{printf "%.3f", v/1e6}')V"
    say "STEP5 ENTER /sys/power/state <- mem   (t0=$t0) PM_TEST=[$(sel_pm_test)]  *** pad may sleep now ***"
    echo mem > /sys/power/state
    rc=$?
    t1=$(cut -d' ' -f1 /proc/uptime)
    gap=$(awk -v a="$t0" -v b="$t1" 'BEGIN{printf "%.2f", b-a}')
    say "STEP5 RETURNED rc=$rc t1=$t1 gap=${gap}s  (real sleep expects >= $WAKE_SECS)"
    kmsg "BACK uptime=$t1 gap=$gap rc=$rc"
    say "STEP5 $(snap)"
    soc_post_cap=$(cat "$BATT/capacity" 2>/dev/null)
    soc_post_chg=$(cat "$BATT/charge_now" 2>/dev/null)
    soc_post_v=$(cat "$BATT/voltage_now" 2>/dev/null)
    say "STEP5 post-resume SOC: capacity=${soc_post_cap}% charge_now=${soc_post_chg}uAh voltage=$(awk -v v="$soc_post_v" 'BEGIN{printf "%.3f", v/1e6}')V"
    # discharge consumption estimate: dAh * avgV = Wh. charge_now falling
    # during a sleep => consumed. If it ROSE, something charged it (abort path).
    awk -v c0="$soc_pre_chg" -v c1="$soc_post_chg" -v v0="$soc_pre_v" -v v1="$soc_post_v" \
        -v cap0="$soc_pre_cap" -v cap1="$soc_post_cap" -v gap="$gap" \
        'BEGIN{
            if (c0 != "" && c1 != "" && c0 > 0 && c1 > 0) {
                d_mah = (c0 - c1) / 1000.0
                vavg = (v0 + v1) / 2e6
                wh = d_mah * vavg / 1000.0
                if (d_mah < -5) {
                    printf "STEP5 consumption: INVALID — charge_now ROSE by %.1f mAh (charger attached during sleep); unplug for a valid power test\n", -d_mah
                } else {
                    if (gap > 0) watts = wh / (gap/3600.0)
                    else watts = 0
                    printf "STEP5 consumption: dQ=%.1f mAh  E=%.4f Wh  avg_power=%.3f W over %.1f s (SoC %d%% -> %d%%)\n", d_mah, wh, watts, gap, cap0, cap1
                }
            } else {
                print "STEP5 consumption: SKIPPED (charge_now unavailable/unreadable)"
            }
        }'
    say "STEP5 suspend_stats: success=$(cat /sys/power/suspend_stats/success 2>/dev/null) fail=$(cat /sys/power/suspend_stats/fail 2>/dev/null) failed_step=$(cat /sys/power/suspend_stats/last_failed_step 2>/dev/null) failed_errno=$(cat /sys/power/suspend_stats/last_failed_errno 2>/dev/null) failed_dev=[$(cat /sys/power/suspend_stats/last_failed_dev 2>/dev/null)]"
    say "STEP5 wakealarm after=[$(cat $RTC/wakealarm 2>/dev/null)]"
    dmesg 2>/dev/null | grep -aE 'suspend entry|suspend exit|Some devices failed|Wakeup pending|Triggering wakeup|hdd_suspend_wlan|hdd_resume_wlan|trigger sys resume|nvt_ts_panel_power|Restarting tasks' | tail -25 | while read -r l; do say "  dmesg| $l"; done
fi

# ---- STEP 6: restore
# USB first: STEP2f unbound the xHCI host, so re-probe it now (idempotent; the trap
# also calls this on any abort/exit after the unbind).
usb_restore
say "STEP6 restore: SetActive false -> $(ss false)"
say "STEP6 display on -> $("$TOGGLE" on 2>&1; echo rc=$?)"
sleep 2
if [ "$PARK_GPU" = 1 ]; then
    echo on > "$GPU/control" 2>/dev/null
    say "STEP6 gpu re-pinned: $(snap)"
fi
say "STEP6 post-restore: $(snap)"
if [ "$RESTORE_WIFI" = 1 ] && [ "$QUIESCE" = 1 ]; then
    say "STEP6 nmcli radio wifi on -> $(nmcli radio wifi on 2>&1; echo rc=$?)"
    systemctl start erhai-wifi-discover.service 2>/dev/null && say "STEP6 discover sweeper restarted"
fi
if [ -n "${ORIG_MEM_SLEEP:-}" ]; then
    echo "$ORIG_MEM_SLEEP" > /sys/power/mem_sleep 2>/dev/null
    say "STEP6 mem_sleep restored=[$(cat /sys/power/mem_sleep)] (orig=$ORIG_MEM_SLEEP)"
fi
if [ -n "$WLAN_TM_ORIG" ]; then
    if [ "$WLAN_TM_ORIG" != 1000000 ]; then
        echo "$WLAN_TM_ORIG" > "$WLAN_TM" 2>/dev/null && say "STEP6 WCN timer_multiplier restored -> $WLAN_TM_ORIG"
    fi
fi
# Restore the power-watch service. Its fd was closed during suspend, so the
# wake-press evdev event was never delivered to it (see STEP2b); opening the
# device freshly here only reads events from after this reopen.
if [ "$POWERWATCH_ACTIVE" = 1 ]; then
    if systemctl start erhai-power-toggle.service 2>/dev/null; then
        say "STEP6 power-watch service RESTARTED"
    else
        say "STEP6 power-watch service start FAILED (rc=$?)"
    fi
fi

# ---- STEP 7: WiFi-repeat observation (owner requested a repeat test)
# With QUIESCE=1 the radio is OFF after resume. WIFI_WATCH_A watches the
# untouched state (does it come back by itself? — answer: NO, only
# gnome-control-center's wifi panel re-enables it, as proven this boot at
# 09:28:26 pid 55718). WIFI_WATCH_B watches the explicit re-enable.
rfk() { printf 'rfkill: '; rfkill list 2>/dev/null | grep -A1 'Wireless LAN' | tr '\n' ' '; echo; }
nmst() { nmcli -t -f DEVICE,STATE dev 2>/dev/null | grep wlp | head -2 | tr '\n' ' '; echo; }
if [ "$QUIESCE" = 1 ]; then
    say "STEP7 WATCH_A start (wifi left OFF, untouched for ${WIFI_WATCH_A}s): $(nmst)"
    i=0
    while [ "$i" -lt "$WIFI_WATCH_A" ]; do
        sleep 10; i=$((i+10))
        say "STEP7 A t=${i}s $(nmst)$(rfk)"
    done
    # NOTE: the tracer passes WIFI_WATCH_A=0 to keep the run short, so this window
    # is normally 0 s and a "came back by itself?" verdict is then MEANINGLESS
    # (2026-10-21: it printed NO while wlp1s0 had in fact returned as a netdev and
    # NM reconnected seconds later).  The real verdict is the netdev STATE:
    # `wlp1s0:disconnected` = wlan.ko re-attached (OK, NM will connect),
    # `wlp1s0:unavailable` = the post-resume WEDGE (see §25.3).
    if [ "${WIFI_WATCH_A:-0}" -gt 0 ]; then
        say "STEP7 WATCH_A end: $(nmst) — conclusion: came back by itself? $(nmcli -t -f DEVICE,STATE dev | grep -q wlp1s0:connected && echo YES || echo NO)"
    else
        say "STEP7 WATCH_A end: $(nmst) — window 0s (tracer): auto-reconnect NOT measured; the netdev state above IS the verdict (disconnected = wlan.ko back, unavailable = wedged), wlan refs=$(lsmod 2>/dev/null | awk '$1=="wlan"{print $3}')"
    fi
    if [ "$RESTORE_WIFI" = 1 ]; then
        say "STEP7 explicit re-enable: nmcli radio wifi on -> $(nmcli radio wifi on 2>&1; echo rc=$?)"
        systemctl start erhai-wifi-discover.service 2>/dev/null && say "STEP7 discover sweeper restarted"
        say "STEP7 WATCH_B start (${WIFI_WATCH_B}s): $(nmst)"
        i=0
        while [ "$i" -lt "$WIFI_WATCH_B" ]; do
            sleep 10; i=$((i+10))
            say "STEP7 B t=${i}s $(nmst)$(rfk)"
        done
        say "STEP7 WATCH_B end: $(nmst)"
    fi
elif [ "$RESTORE_WIFI" = 1 ]; then
    systemctl start erhai-wifi-discover.service 2>/dev/null && say "STEP7 discover sweeper restarted"
fi
systemctl stop erhai-suspend-unwedge.timer erhai-suspend-unwedge.service 2>/dev/null
touch "$DONEF"
say "===== SUSPEND s2idle RUN END (done marker $DONEF set) ====="
