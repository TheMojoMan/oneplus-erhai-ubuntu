#!/bin/bash
# erhai (OnePlus Pad 3) — apply the DOCUMENTED internal-microphone capture state.
#
# WHY THIS EXISTS (2026-10-20): the mic routing lives in the UCM profile's
# SectionDevice "Mic" EnableSequence, but WirePlumber runs this card with
# `api.alsa.use-acp = false` (51-erhai-sound-ucm.conf), so WirePlumber NEVER
# executes the UCM *device* sequences — only the HiFi VERB is applied, by
# erhai-audio-bringup.sh.  The capture controls are therefore whatever the card
# came up with, and on a freshly flashed image (no
# /var/lib/alsa/asound.state) that is the DRIVER DEFAULT: `ADC1/ADC4 Switch`
# off, `ADC1/ADC4 Volume` 0 (= mute) and `TX SMIC MUX0/1` = ZERO, i.e. the
# decimators are wired to nothing.  The result is a microphone that records
# digital silence in every app (camera, recorder, browser) with no error
# anywhere — measured on this pad, and the same class of failure as a stale
# asound.state restoring the vendor lane labels (SWR_MIC9 / SWR_MIC4, which
# wcd939x turns into the wrong SoundWire slave channels).
#
# This script applies the documented working state at EVERY boot, AFTER
# alsa-restore, so the result no longer depends on the stored state at all.
# erhai-audio-bringup.sh calls it too (its tail), which covers the boot where
# the LPASS chain only came up inside that script (the card did not exist when
# this unit's condition was evaluated).
#
# Values = the UCM SectionDevice "Mic" EnableSequence (HiFi.conf) plus the
# TX_AIF1_CAP Mixer DEC2..7 clearing dance that the UCM's plain "0" cannot do:
# tx_macro_tx_mixer_put() returns "unchanged" and swallows a lone "0" while
# active_decimator == -1, so the mask bit only clears right after that
# decimator was enabled (microphone-MEMORY-2026-09-19 §40.7 + §41.2).
set -u

log() { echo "erhai-mic-state: $*"; }

# libasound 1.2.16 (Ubuntu 26.10) resolves amixer's name= lookup differently from
# 1.2.15 (26.04): 'amixer -c 0 cget name=ADC4 Volume' fails with "Cannot find the
# given element from control sysdefault:0" although the control is listed by
# 'amixer -c 0 controls' -- so every setting below was skipped on stonking and
# the mic route stayed at the driver defaults.  Try the HARDWARE mixer first
# (-D hw:0) and both the raw CTL name ('ADC4 Volume') and the simple-element
# name ('ADC4'); the first combination that works wins.
amixer_dev() {	# $1 = cset|cget, rest = amixer args
	local d
	for d in "-D hw:0" "-c 0"; do
		if amixer $d "$@" 2>/dev/null; then return 0; fi
	done
	return 1
}
c() {		# $1 = control name, $2 = value
	if amixer_dev cset name="$1" "$2" >/dev/null ||
	   amixer_dev cset name="${1% Volume}" "$2" >/dev/null; then
		return 0
	fi
	log "warning: could not set '$1' to '$2'"
}
cg() {		# $1 = control name -> "values=.." (empty when unreadable)
	local o
	o="$(amixer_dev cget name="$1" | tail -1 | tr -d ' ')"
	[ -n "$o" ] || o="$(amixer_dev cget name="${1% Volume}" | tail -1 | tr -d ' ')"
	printf '%s' "$o"
}

# The card (and its control node) must exist; the unit's condition covers the
# normal boot, this covers being called from the bring-up script.
for i in $(seq 1 20); do
	[ -e /dev/snd/controlC0 ] && break
	sleep 0.5
done
if [ ! -e /dev/snd/controlC0 ]; then
	log "no /dev/snd/controlC0; nothing to do"
	exit 0
fi

# 1. Analog side: only ADC1 + ADC4 carry the internal mics (ADC2/ADC3 MUST be
#    off, else the slave port channel masks become 0x3/0x3 instead of 0x1/0x2).
c 'ADC2 Switch' off
c 'ADC3 Switch' off
c 'ADC1 Switch' on
c 'ADC4 Switch' on
c 'ADC1 MUX' 1
c 'ADC4 MUX' 4
c 'ADC1_MIXER Switch' on
c 'ADC4_MIXER Switch' on
# Vendor lane labels stay ZERO so the driver's ch_info masks apply: ADC1 ->
# lane 1 (SWR_MIC0), ADC4 -> lane 4 (SWR_MIC3) = SoundWire port mask 0x9.
c 'ADC1 ChMap' ZERO
c 'ADC4 ChMap' ZERO

# 2. TX macro: both decimators on, SMIC inputs routed to the two mic lanes.
c 'TX0 MODE' 1
c 'TX3 MODE' 1
c 'TX DEC0 MUX' 1
c 'TX DEC1 MUX' 1
c 'TX SMIC MUX0' SWR_MIC0
c 'TX SMIC MUX1' SWR_MIC3
c 'TX DMIC MUX0' 0
c 'TX DMIC MUX1' 0
c 'TX_AIF1_CAP Mixer DEC0' 1
c 'TX_AIF1_CAP Mixer DEC1' 1
for d in 2 3 4 5 6 7; do
	c "TX_AIF1_CAP Mixer DEC$d" 1
	c "TX_AIF1_CAP Mixer DEC$d" 0
done
# va-macro capture mixers are all Off in Android's live tree.
for d in 0 1 2 3; do
	c "VA_AIF1_CAP Mixer DEC$d" 0
	c "VA_AIF2_CAP Mixer DEC$d" 0
	c "VA_AIF3_CAP Mixer DEC$d" 0
done

# 3. Gain: ANALOG first (the SNR lever, before the ADC) then digital.
#    ADCn Volume 0..20 = 0..+5 dB (0 = MUTE!), TX_DECn 0..124 = -84..+40 dB
#    with 84 = 0 dB.  The tested setting is +5 dB analog + 10 dB digital
#    (peaks -9..-12 dBFS on a normal voice take, 0 clipped samples;
#    TX_DEC 98 clipped on loud speech — microphone-MEMORY-2026-10-19 §52.16).
c 'ADC1 Volume' 20
c 'ADC4 Volume' 20
c 'TX_DEC0 Volume' 94
c 'TX_DEC1 Volume' 94

# 4. Proof in the journal: the two controls whose vendor values silence the mic.
log "TX SMIC MUX0 = $(cg 'TX SMIC MUX0'), TX SMIC MUX1 = $(cg 'TX SMIC MUX1')"
log "mic capture state applied"
