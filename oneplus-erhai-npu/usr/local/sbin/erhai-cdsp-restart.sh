#!/bin/bash
# erhai-cdsp-restart.sh -- restart ONLY the compute DSP (remoteproc) and bring
# the CDSP remotefs daemon back up.
#
#   sudo erhai-cdsp-restart.sh            # quiet
#   sudo VERBOSE=1 erhai-cdsp-restart.sh  # show the state transitions + the log
#
# WHY YOU NEED THIS: the CDSP FastRPC mapping arena is finite (~4 GiB of IOVA
# under a 32-bit DMA mask).  Every load that fails strands mappings, and the
# arena also keeps a per-CPU IOVA rcache, so after a few loads/short probes a
# model load dies with
#     ggml-hex: HTP0 buffer mapping failed : domain_id 3 size ... error 0x00000001
#     kernel:   map_attach: dma_buf_map_attachment failed (... err -12)
#     app:      GenieXError(-100201) Model loading failed
# A CDSP restart makes the DSP redo its one-shot shell/skel loads and clears the
# arena -- it is THE recovery (verified 5/5 on 2026-09-21).
#
# It restarts the CDSP (remoteproc) ONLY and NEVER the ADSP: stopping the ADSP
# (remoteproc0) hard-freezes the whole SoC and ends in a PMIC/watchdog reset.
set -euo pipefail

VERBOSE="${VERBOSE:-0}"
log() { if [ "$VERBOSE" = 1 ]; then echo "$@"; fi; }

[ "$(id -u)" = 0 ] || { echo "erhai-cdsp-restart: needs root (sysfs + systemctl)" >&2; exit 1; }

# Find the CDSP remoteproc by NAME -- the index is not stable across kernels.
CDSP=""
for r in /sys/class/remoteproc/remoteproc*; do
	[ -e "$r/name" ] || continue
	if [ "$(cat "$r/name" 2>/dev/null)" = cdsp ]; then CDSP="$r"; break; fi
done
if [ -z "$CDSP" ]; then
	echo "erhai-cdsp-restart: no remoteproc named cdsp is registered" >&2
	echo "  (this kernel/DT does not enable the CDSP: oneplus-erhai-linux >= +b63)" >&2
	exit 1
fi
log "CDSP remoteproc: $CDSP"

systemctl stop hexagonrpcd-cdsp.service >/dev/null 2>&1 || true
systemctl stop cdsprpcd.service >/dev/null 2>&1 || true

echo stop > "$CDSP/state"
sleep 2
log "state after stop:  $(cat "$CDSP/state")"
echo start > "$CDSP/state"
sleep 3
log "state after start: $(cat "$CDSP/state")"

if [ "$(cat "$CDSP/state")" != "running" ]; then
	echo "erhai-cdsp-restart: CDSP did not come back up (state $(cat "$CDSP/state"))" >&2
	exit 1
fi
log "$(ls -l /dev/fastrpc-cdsp* 2>&1)"

systemctl start cdsprpcd.service 2>/dev/null || true
if [ "$(systemctl is-active cdsprpcd.service 2>/dev/null)" = active ]; then
	echo "erhai-cdsp-restart: CDSP restarted, cdsprpcd active (mapping arena cleared)"
else
	echo "erhai-cdsp-restart: CDSP restarted, but cdsprpcd is not active" >&2
	echo "  (is oneplus-erhai-fastrpc installed? journalctl -u cdsprpcd -n 30)" >&2
	exit 1
fi
if [ "$VERBOSE" = 1 ]; then
	journalctl -u cdsprpcd.service -b --no-pager -o cat --since "-2min"
fi
exit 0

