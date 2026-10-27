#!/bin/bash
# erhai-npu-status.sh -- one-shot health report of the Hexagon NPU / CDSP chain.
#
# The Hexagon NPU (HTP) is an accelerator hanging off the Hexagon COMPUTE DSP
# (CDSP), driven by DSP-side libs loaded over FastRPC. The chain this script
# checks, in order:
#
#   1. kernel + device tree  (&remoteproc_cdsp status, firmware-name)
#   2. firmware              (cdsp.mdt + split segments under /lib/firmware)
#   3. remoteproc            (cdsp powering up: /sys/class/remoteproc/*/state)
#   4. FastRPC nodes         (/dev/fastrpc-cdsp, -cdsp-secure)
#   5. transport layer       (oneplus-erhai-fastrpc: the fastrpc group, the
#                             unsigned-PD environment, the rfsa payload trees)
#   6. daemon                (cdsprpcd serves the CDSP; the disabled
#                             hexagonrpcd-cdsp.service fallback is reported too)
#   7. NPU payload           (HTP runtime + QNN v79 skel present in the tree)
#
# The erhai Hexagon stack is split across three packages: oneplus-erhai-fastrpc
# (transport: libraries, cdsprpcd, group, udev rule, DSP payload trees),
# oneplus-erhai-npu (this one: the CDSP remotefs tree, this script, npu-llm) and
# oneplus-erhai-sensors (the ADSP: hexagonrpcd-erhai + hexagonfs-erhai).

# Without arguments it only READS (safe as a normal user).
#
#   --pd-test   additionally prove that the CDSP executes Hexagon code we hand
#               it: hexagonrpcd's -c mode uploads /var/lib/hexagonfs-cdsp/dsp/
#               cdsp/fastrpc_shell_unsigned_3 with FASTRPC_IOCTL_INIT_CREATE,
#               i.e. the DSP boots a user PD from a host-provided ELF. This is
#               the exact mechanism a QNN/NPU client uses to get a DSP shell,
#               so a surviving daemon here means the Hexagon side is alive.
#               Needs root (the FastRPC node is root:fastrpc 0660).
#
# Exit code: 0 when the CDSP and its FastRPC nodes are up (steps 1-4), 1
# otherwise. Steps 5/6 report but never fail: they can legitimately be pending
# while a graph workload is still being brought up.
set -u

PD_TEST=0
[ "${1:-}" = "--pd-test" ] && PD_TEST=1

ok=0
warn=0

say()  { printf '%-14s %s\n' "$1" "$2"; }
pass() { say "OK" "$1"; }
bad()  { say "FAIL" "$1"; ok=1; }
note() { say "--" "$1"; warn=1; }


echo "=== erhai NPU / Hexagon CDSP status ($(date '+%F %T')) ==="

# ---------------------------------------------------------------- 1. kernel --
KREL="$(uname -r)"
pass "kernel $KREL"
CDSP_DT="/proc/device-tree/soc@0/remoteproc@32300000"
if [ -d "$CDSP_DT" ]; then
	st="$(tr -d '\0' < "$CDSP_DT/status" 2>/dev/null)"
	st="${st:-<none>}"
	if [ -f "$CDSP_DT/firmware-name" ]; then
		fwn="$(tr '\0' ' ' < "$CDSP_DT/firmware-name")"
		fwn="${fwn% }"
	else
		fwn="<driver default>"
	fi
	if [ "$st" = "okay" ]; then
		pass "DT cdsp node status=$st firmware-name=${fwn:-<driver default>}"
	else
		bad "DT cdsp node status=$st -- this kernel/DT does not enable the"
		bad "   CDSP; boot a kernel built from the NPU branch (+b63 or newer)"
	fi
else
	bad "no /proc/device-tree/soc@0/remoteproc@32300000 -- CDSP node absent"
fi

# ------------------------------------------------------------- 2. firmware ---
FW=/lib/firmware/qcom/sm8750/OnePlus
miss=""
for f in cdsp.mdt cdsp_dtb.mdt cdsp.b00 cdsp.b05 cdsp_dtb.b01; do
	[ -f "$FW/$f" ] || miss="$miss $f"
done
if [ -z "$miss" ]; then
	pass "CDSP firmware present ($(ls "$FW" | grep -c '^cdsp') cdsp* files in $FW)"
else
	bad "CDSP firmware incomplete:$miss (need oneplus-erhai-firmware >= 1.2)"
fi

# ------------------------------------------------------------ 3. remoteproc ---
found_cdsp=0
for r in /sys/class/remoteproc/remoteproc*; do
	[ -e "$r" ] || continue
	name="$(cat "$r/name" 2>/dev/null)"
	state="$(cat "$r/state" 2>/dev/null)"
	if [ "$name" = "cdsp" ]; then
		found_cdsp=1
		if [ "$state" = "running" ]; then
			pass "remoteproc $(basename "$r") = cdsp, state=running"
		else
			bad "remoteproc $(basename "$r") = cdsp, state=$state"
			note "check: dmesg -l err,warn | grep -i cdsp   (PAS load / XPU fault?)"
		fi
	else
		note "remoteproc $(basename "$r") = ${name:-?}, state=${state:-?}"
	fi
done
[ "$found_cdsp" = 1 ] || bad "no remoteproc named cdsp is registered at all"

# ------------------------------------------------------------- 4. FastRPC ----
rpc_ok=1
for d in /dev/fastrpc-cdsp /dev/fastrpc-cdsp-secure; do
	if [ -c "$d" ]; then
		pass "$d exists ($(stat -c '%U:%G %a' "$d"))"
	else
		bad "$d missing -- the CDSP fastrpc rpmsg driver did not probe"
		rpc_ok=0
	fi
done
if [ "$rpc_ok" = 1 ]; then
	pass "FastRPC client devices ready (this is the NPU's AP-side interface)"
fi

# --------------------------------------------------- 5. FastRPC transport ------
if getent group fastrpc >/dev/null 2>&1; then
	pass "the fastrpc group exists"
else
	bad "no fastrpc group -- install oneplus-erhai-fastrpc"
fi
if id -nG 2>/dev/null | tr ' ' '\n' | grep -qx fastrpc; then
	pass "user $(id -un) is in the fastrpc group"
else
	note "user $(id -un) is NOT in the fastrpc group (re-login needed; group"
	note "  membership only applies to a fresh session)"
fi
if [ -r /etc/environment.d/57-erhai-fastrpc-unsigned.conf ]; then
	pass "unsigned-PD environment shipped (FASTRPC_UNSIGNED_PD=1)"
else
	note "no /etc/environment.d/57-erhai-fastrpc-unsigned.conf (the Linux QNN/"
	note "  ggml skels are unsigned-only: install oneplus-erhai-fastrpc)"
fi
for d in /usr/lib/rfsa/adsp /usr/lib/rfsa/cdsp; do
	if [ -f "$d/fastrpc_shell_unsigned_3" ]; then
		pass "$d payload tree present"
	else
		note "$d incomplete (oneplus-erhai-fastrpc not installed?)"
	fi
done

# --------------------------------------------------------------- 6. daemon ----
# cdsprpcd (oneplus-erhai-fastrpc) is the daemon that works with this firmware;
# hexagonrpcd-cdsp (this package) is the disabled fallback.
for unit in cdsprpcd.service hexagonrpcd-cdsp.service; do
	state="$(systemctl is-enabled "$unit" 2>/dev/null || true)"
	case "$state" in
	enabled) pass "$unit enabled" ;;
	static)  pass "$unit static -- udev/SYSTEMD_WANTS starts it, no enable symlink" ;;
	*)       note "$unit ${state:-not found}" ;;
	esac
	if systemctl is-active --quiet "$unit"; then
		pass "$unit active -- remote files served to the CDSP"
	else
		note "$unit not active ($(systemctl is-active "$unit" 2>/dev/null))"
		note "journal: journalctl -u $unit -n 30 --no-pager"
	fi
done

# ------------------------------------------------------- 7. NPU payload ------
TREE=/var/lib/hexagonfs-cdsp/dsp/cdsp
if [ -d "$TREE" ]; then
	n=$(find "$TREE" -maxdepth 1 -type f | wc -l)
	pass "remotefs $TREE ($n files)"
	for f in fastrpc_shell_3 fastrpc_shell_unsigned_3 libhcp_rpc_skel.so \
		 libQnnHtpV79Skel.so; do
		if [ -f "$TREE/$f" ]; then
			pass "  payload $f ($(stat -c %s "$TREE/$f") bytes)"
		else
			note "  payload $f MISSING"
		fi
	done
else
	note "remotefs $TREE missing (oneplus-erhai-npu not installed?)"
fi

# ------------------------------------------------------- 8. PD test (opt) ----
if [ "$PD_TEST" = 1 ]; then
	echo
	echo "--- FastRPC user-PD test (INIT_CREATE of our own DSP shell) ---"
	SHELL_ELF=/var/lib/hexagonfs-cdsp/dsp/cdsp/fastrpc_shell_unsigned_3
	if [ "$(id -u)" != 0 ]; then
		note "--pd-test needs root (the FastRPC node is root:fastrpc 0660)"
	elif [ ! -x /usr/local/bin/hexagonrpcd-erhai ]; then
		note "the PD test needs hexagonrpcd-erhai (oneplus-erhai-sensors)"
	elif [ ! -c /dev/fastrpc-cdsp ]; then
		bad "no /dev/fastrpc-cdsp -- cannot create a CDSP user PD yet"
	elif [ ! -f "$SHELL_ELF" ]; then
		bad "no $SHELL_ELF (oneplus-erhai-npu remotefs incomplete)"
	else
		log="$(mktemp)"
		timeout 10 /usr/local/bin/hexagonrpcd-erhai \
			-f /dev/fastrpc-cdsp -d cdsp \
			-c "$SHELL_ELF" -R /var/lib/hexagonfs-cdsp \
			>"$log" 2>&1
		rc=$?
		sed 's/^/                   /' "$log"
		if grep -qiE 'Could not (attach|open)' "$log"; then
			bad "the CDSP rejected the user PD (see message above)"
		elif [ "$rc" = 0 ] || [ "$rc" = 124 ]; then
			pass "CDSP accepted the host-uploaded shell -- FASTrpc output PD runs"
		else
			bad "hexagonrpcd exited rc=$rc while creating the CDSP user PD"
		fi
		rm -f "$log"
	fi
fi

# --------------------------------------------------------------- evidence ----
echo
echo "--- kernel/driver evidence (Handover spam filtered) ---"
( dmesg 2>/dev/null || journalctl -k --no-pager 2>/dev/null ) \
	| grep -viE 'Handover signaled' \
	| grep -iE 'cdsp|fastrpc|q6v5-pas|remoteproc' | tail -15

echo
if [ "$ok" = 0 ]; then
	echo "RESULT: CDSP + FastRPC up -- Hexagon compute is available."
	[ "$warn" = 0 ] || echo "        (see -- notes above)"
else
	echo "RESULT: CDSP chain NOT up (see FAIL lines above)."
	echo "        Reminder: enabling it needs the +b63 (NPU branch) kernel"
	echo "        booted from the rEFInd menu -- a reboot is the owner's call."
fi
exit $ok
