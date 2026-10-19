#!/bin/bash
# erhai (OnePlus Pad 3) — bring up the AudioReach sound card, then apply the
# UCM HiFi verb routing.
#
# Problem this fixes: the LPASS audio platform devices (lpass-lpi-pinctrl at
# 7760000, the rx/tx/va macros at 6ac/6ae/7660000, both SoundWire masters and
# finally the snd-sc8280xp "sound" node) can all end up UNBOUND when their
# modules are loaded by late udev coldplug — the bus never retries the probe,
# fw_devlink keeps everyone waiting, /proc/asound stays empty ("no
# soundcards") and GNOME shows no audio. Verified on sda17 (2026-08-23):
# binding 7760000.pinctrl + reloading the ASoC modules brings the whole chain
# up in order and card0 appears.
#
# Everything here is idempotent: on a system where the drivers already bound
# at boot (e.g. modules built-in or loaded early), every step is a no-op.
set -u

SYS=/sys/bus/platform

log() { echo "erhai-audio-bringup: $*"; }

bound() { [ -n "$(readlink "$SYS/devices/$1/driver" 2>/dev/null)" ]; }

# 1. LPI pinctrl: the macros' pinctrl provider. Its driver allows manual bind.
if ! bound 7760000.pinctrl; then
	log "binding lpass-lpi-pinctrl 7760000"
	echo 7760000.pinctrl > "$SYS/drivers/qcom-sm8650-lpass-lpi-pinctrl/bind" 2>/dev/null \
		|| log "warning: could not bind 7760000.pinctrl"
	sleep 1
fi

# 2. Macros + SoundWire masters: these use suppress_bind_attrs (no bind file),
#    so if they missed their probe window the only fix is reloading the
#    modules — with the pinctrl now bound, registration re-matches every
#    device and the probes run in dependency order.
if ! bound 6ac0000.codec || ! bound 6ae0000.codec || \
   ! bound 7660000.codec || ! bound 6ad0000.soundwire || ! bound 7630000.soundwire; then
	log "re-running LPASS module probe chain"
	modprobe -r snd_soc_sc8280xp snd_soc_qcom_sdw soundwire_qcom \
		snd_soc_lpass_rx_macro snd_soc_lpass_tx_macro \
		snd_soc_lpass_va_macro snd_soc_lpass_wsa_macro 2>/dev/null
	modprobe snd_soc_lpass_va_macro
	modprobe snd_soc_lpass_rx_macro
	modprobe snd_soc_lpass_tx_macro
	modprobe snd_soc_lpass_wsa_macro 2>/dev/null || true
	modprobe soundwire_qcom
	sleep 2
fi

# 3. Machine driver -> registers the card once all codec components exist.
if ! bound sound && [ ! -e /proc/asound/card0 ]; then
	log "loading snd-sc8280xp machine driver"
	modprobe snd_soc_sc8280xp
fi

# 4. Wait for the card (kernel side: /proc/asound/card0).
for i in $(seq 1 30); do
	[ -e /proc/asound/card0 ] && break
	sleep 1
done
if [ ! -e /proc/asound/card0 ]; then
	log "ERROR: card0 never appeared"
	exit 1
fi
log "sound card up: $(cat /proc/asound/card0/id 2>/dev/null)"

# 4b. Wait for the udev-created control device node as well. /proc/asound/card0
#     appears when the KERNEL registers the card, but /dev/snd/controlC0 is
#     created by UDEV — on a fresh flash (busy coldplug backlog) the node can
#     lag far behind, and alsaucm/amixer then fail with "Cannot get card index
#     for 0": the verb AND every route forcing are silently lost, all FE<->BE
#     mixers stay off,off -> "no backend DAIs enabled" -> Dummy Output.
#     (sda17 2026-09-03: nodes landed ~1 min after /proc/asound/card0.)
for i in $(seq 1 30); do
	[ -e /dev/snd/controlC0 ] && break
	sleep 1
done
if [ ! -e /dev/snd/controlC0 ]; then
	log "warning: /dev/snd/controlC0 still missing after 30 s (udev backlog?)"
fi

# 5. Apply the HiFi verb (FE->BE mixer routing + AW88261 amp un-mute).
#    Idempotent; also covers a stale asound.state. Retry: a ctl node that
#    appeared between the wait and the open must not kill the whole chain.
verb_ok=0
for i in $(seq 1 10); do
	if alsaucm -c 0 set _verb HiFi 2>/dev/null; then
		verb_ok=1
		break
	fi
	sleep 1
done
[ "$verb_ok" = 1 ] || log "warning: alsaucm HiFi verb failed"
log "applied UCM HiFi verb routing on card0"

# 5b. Force the FE<->BE routes with plain amixer as well. On fresh flashes
#     the verb reported success yet FE opens still failed with "no backend
#     DAIs enabled" (route not connected at open time) — so do not trust the
#     UCM path alone. Values match the verb's EnableSequence and the known-
#     good sda18 state exactly: these mixers have TWO channels, and the
#     working state is on,off (a bare "1" would set BOTH -> dual route).
amixer -c 0 cset "name=SECONDARY_MI2S_RX Audio Mixer MultiMedia1" on,off >/dev/null 2>&1 \
	|| log "warning: could not set MM1->SECONDARY_MI2S_RX route"
amixer -c 0 cset "name=DISPLAY_PORT_RX_0 Audio Mixer MultiMedia2" on,off >/dev/null 2>&1 \
	|| log "warning: could not set MM2->DISPLAY_PORT_RX_0 route"
amixer -c 0 cset "name=MultiMedia3 Mixer TX_CODEC_DMA_TX_3" on,off >/dev/null 2>&1 \
	|| log "warning: could not set MM3<-TX_CODEC_DMA_TX_3 route"
amixer -c 0 cset "name=MultiMedia4 Mixer VA_CODEC_DMA_TX_0" on,off >/dev/null 2>&1 \
	|| log "warning: could not set MM4<-VA_CODEC_DMA_TX_0 route"
# and make sure no OTHER MM1 back-end is dual-routed (fails the DSP graph):
for be in RX_CODEC_DMA_RX_0 WSA_CODEC_DMA_RX_0 DISPLAY_PORT_RX_0; do
	amixer -c 0 cset "name=$be Audio Mixer MultiMedia1" off,off >/dev/null 2>&1 || true
done
log "routes forced; MM1 route now: $(amixer -c 0 cget "name=SECONDARY_MI2S_RX Audio Mixer MultiMedia1" 2>/dev/null | grep -o ': values=.*' || echo '?')"

# 6. Raise AW88261 smart-amp gain (+5 dB over stock). Amp mixer controls:
#    0..360 steps of 0.25 dB (0 = -90 dB, 360 = 0 dB). Stock runtime value
#    300 (-15 dB); 320 = -10 dB = x1.78 amplitude. Kept below 360
#    deliberately: no clipping of the amp's internal headroom, the AW88261
#    DSP protection (aw88261_acf.bin) stays fully effective.
AMP_GAIN=320
for ctl in Spk0 Spk1 Spk2 Spk3 Spk5 Spk6; do
	amixer -c 0 cset name="$ctl PCM Playback Volume" "$AMP_GAIN" >/dev/null 2>&1 \
		|| log "warning: could not set $ctl PCM Playback Volume"
done
log "amp gain offset applied (all six AW88261 -> $AMP_GAIN)"

# 7. Re-probe safety net: a PipeWire/WirePlumber that already enumerated the
#    card before the verb was applied keeps its failed (aborted) nodes — the
#    ALSA monitor does not retry on its own. A sound "change" uevent makes it
#    revalidate the card and (re-)create the nodes.
udevadm trigger --subsystem-match=sound --action=change 2>/dev/null \
	|| log "warning: udevadm trigger failed (no running monitor to rescue)"

