#!/bin/bash
#
# erhai: Android-style whole-die CPU+GPU thermal governor (NPU-aware).
#
# MODEL.  On Android the closed-source daemon thermal-engine-v2 watches the
# WHOLE SoC envelope (CPU + GPU + NPU + DDR sensors) and, as it approaches the
# passive trips, writes LEVELS into the kernel cooling devices:
#
#     cooling_device0   cpufreq-cpu0         (cores 0-5,  level 0..15)
#     cooling_device1   cpufreq-cpu6         (cores 6-7,  level 0..15)
#     cooling_device2   devfreq-3d00000.gpu  (Adreno GPU, level 0..13)
#
# Burnout-Benchmark telemetry recorded on Android 16 (tmp/thermal-adb/REPORT.md)
# shows what the reference behaviour looks like: under a sustained CPU+GPU+NPU
# load the engine holds the SoC envelope at 92-106 C, clamping cluster0 to
# ~960 MHz (level 12), cluster1 to ~1018 MHz (level 15) and the GPU 1100 ->
# 832 -> 734 MHz (devfreq levels 4/5). Nothing else fires (no core offlining,
# no NPU cooling device, no battery/charger levers) and no app is ever killed;
# the shutdown thresholds (socd 99 / SoC hot 125 C) are never approached.
#
# This package implements the SAME architecture on the Linux port:
#   * the kernel zones are set to step_wise with their passive trips parked
#     HIGH (109/120 C, below the 125 C hot trip) so the in-kernel governors
#     stay out of the way and the 125 C hot trip remains the last defence,
#   * the 'monitor' daemon polls the whole-die envelope (cpu-*, cpuss-*,
#     gpuss*, nsphmx*, nsphvx*, ddr) every ERHAI_THERMAL_POLL seconds and
#     writes the two CPU cooling devices (cpufreq-cpu0/cpu6) through a
#     graduated, hysteresis'd ladder (attack/ease one level per poll).  A clamp
#     is only fully released after ERHAI_THERMAL_SOAK seconds with the die
#     below RELEASE_TEMP (65C), an absolute level far under every clamped
#     equilibrium and just above idle.  The envelope is read from the shell
#     process itself, ONCE per poll and with no fork per zone (bash builtin
#     `$(<file)` reads + a cached zone inventory): one `cat` per zone per poll
#     cost ~12% of a core on this SoC, i.e. the governor was heating what it
#     measures.
#   * the GPU plane is NOT driven here: the gpuss zones run their own per-plane
#     step_wise at the profile switch-on (v1.1 semantics).  The Adreno devfreq
#     is a display device on this kernel, and a live-session clamp down to
#     734 MHz coincided with display noise + an NVT touch recovery storm on
#     2026-09-24, so the die-governor never touches cdev2/GPU.
#
# The NPU has no cooling device of its own -- as on Android, its clock lives
# inside the Hexagon runtime -- so it is protected by the ENVELOPE clamp: once
# the die approaches the trip band, the CPU/GPU planes stop adding heat, which
# is precisely what keeps the whole package (NPU included) inside Android's
# 92-106 C envelope on the same workload.
#
# This design supersedes the 1.1 power_allocator (IPA) tables deliberately:
# IPA could only see the CPU planes, so a full NPU load (cold CPU, cold GPU,
# hot nsphmx/ddr) gave it nothing to throttle and the die ran away.  The
# per-plane ceilings it enforced are preserved as PROFILE TRIGGERS here, now
# applied to the hottest of ALL SoC sensors.
#
# Usage:
#   erhai-thermal.sh                apply the persisted profile
#   erhai-thermal.sh <profile>      apply <profile> now AND persist it
#   erhai-thermal.sh monitor        run the die-governor loop (the boot service)
#   erhai-thermal.sh release        drop all cooling levels to 0 and exit
#   erhai-thermal.sh status         show profile, hot-die temp, clamps and hold
#   erhai-thermal.sh list           list the available profiles
#
# EXTERNAL HOLD (the NPU pre-cap).  The governor never EASES below the floor a
# live holder requests and raises to it INSTANTLY when the request is new, so a
# client can pre-cap the CPU before a known-hot load (an NPU model run) instead
# of waiting for this daemon to catch the spike one rung at a time.  npu-llm is
# the only client today; it leases the clamp by writing one line to the
# world-writable /run/erhai-thermal.hold (created by the governor at start):
#     pid=<pid> step=<0-4> gpu=<-1|0-5>
#   pid   - the lease owner: the hold applies only while `kill -0 <pid>` works.
#           npu-llm writes ITS OWN PID and exec's the model runner, so the lease
#           lives exactly as long as the run and self-releases at the latest one
#           poll after the process dies, even on SIGKILL.  A manual one-off:
#             ( sleep 120 ) & echo "pid=$! step=2 gpu=-1" > /run/erhai-thermal.hold
#   step  - CPU ladder floor 0..4 (the watcher's own hot-die clamps:
#           0=2227/3072  1=1785/2649  2=1363/2246  3=960/1018  4=384/1018 MHz).
#   gpu   - optional devfreq(cdev2) clamp for the GPU plane, which this governor
#           otherwise NEVER touches (2026-09-24 incident); -1 = leave it alone.
#
# Tunables (env or /etc/default/erhai-thermal, override the profile):
#   ERHAI_THERMAL_POLL     seconds between envelope polls (default 1.5)
#   ERHAI_THERMAL_SOAK     seconds of cool time below the release trip before
#                          the clamps drop to zero (default 15)
#   ERHAI_THERMAL_BACKSTOP kernel switch-on trip in milli-C (default 109000);
#                          the kernel step_wise then owns the 109-125 C band
#                          only, as a backstop if this daemon ever dies

STATE=/etc/default/erhai-thermal
RUNSTATE=/run/erhai-thermal.state

# --- profile -> whole-die switch-on trigger (milli-C) -------------------------
# performance = Android's observed envelope: the grip starts at 94 C and the
# graduated ladder reaches its deep floor (960/1018/734 MHz) at 103+ C, so the
# die holds at Android's measured 92-106.  balanced/cool/cold use the same
# machine with the grip 10/20/30 C lower (their advertised ~90/80/70 ceilings
# are where the deep floor lands).
profile_params() {
	case "$1" in
	performance | android | stock | perf | 100)
		CANON=performance ; TRIGGER=94000 ;;
	balanced | 90)
		CANON=balanced   ; TRIGGER=84000 ;;
	cool | 80)
		CANON=cool       ; TRIGGER=74000 ;;
	cold | gaming | 70)
		CANON=cold       ; TRIGGER=64000 ;;
	*) return 1 ;;
	esac
}

# --- graduated ladder: for step $i, levels (cdev0, cdev1, cdev2) --------------
# thresholds are TRIGGER + DT[i]; levels are the LIVE tables measured on this
# device (level -> clamp): cdev0 6=2227 8=1785 10=1363 12=960 15=384;  cdev1
# 6=3072 8=2649 10=2246 15=1018;  cdev2 2=967 3=900 4=832 5=734.
# Step 0 is MILD (a ~2.2-3.0 GHz clamp) so the die lands INSIDE the band and the
# governor settles on the mildest effective clamp; step 3 is Android's measured
# steady state (960/1018/734 MHz) and step 4 (~106C) the deepest rung.
STEPS_DT=(0 3000 6000 9000 12000)
STEPS_C0=(6 8 10 12 15)
STEPS_C1=(6 8 10 15 15)
STEPS_G=(0 0 0 0 0)   # cdev2/GPU is NOT driven by the die-governor (see reconfigure_zones)

DEFAULT_POLL=1.5
DEFAULT_SOAK=15
DEFAULT_BACKSTOP=109000
# The released state is keyed on an ABSOLUTE temperature, not a band below the
# trigger: a clamped die under a running load sits >= ~87C on every ladder rung,
# while the idle die sits 45-56C - so "die below 65C for the soak" can only be
# reached after the load actually ended (a deep-clamped load that still dips below
# 65C simply hands the speed back and re-attacks to the rung it needs).
RELEASE_TEMP=65000
HOT=125000

[ -r "$STATE" ] && . "$STATE"
POLL=${ERHAI_THERMAL_POLL:-$DEFAULT_POLL}
SOAK=${ERHAI_THERMAL_SOAK:-$DEFAULT_SOAK}
BACKSTOP=${ERHAI_THERMAL_BACKSTOP:-$DEFAULT_BACKSTOP}

list_profiles() {
	echo "available profiles (whole-die ceiling, applied to the hottest SoC sensor - CPU, GPU, NPU or DDR):"
	echo "  performance  ~103C deep floor at 94C switch-on - Android's exact clamp (960/1018/734 MHz), safe to its ~106C envelope"
	echo "  balanced     ~90C"
	echo "  cool         ~80C"
	echo "  cold         ~70C  for lighter gaming"
	echo "aliases: 100/90/80/70, android, stock, perf, gaming"
}

read_state_profile() {
	local cur line
	cur=""
	# builtin line scan, last match wins - same result as
	# `sed -n 's/^ERHAI_THERMAL_PROFILE=//p' "$STATE" | tail -1`, without the
	# two extra processes on every poll.
	if [ -r "$STATE" ]; then
		while IFS= read -r line; do
			case "$line" in
			ERHAI_THERMAL_PROFILE=*) cur=${line#ERHAI_THERMAL_PROFILE=} ;;
			esac
		done < "$STATE"
	fi
	cur=${cur:-performance}
	# A persisted file written before the android->performance rename still says "android".
	profile_params "$cur" && echo "$CANON" || echo performance
}

# --- zone inventory + fork-free sysfs readers ----------------------------------
# A zone's TYPE is registered by its driver once at boot, so the (path, type)
# pairs are collected here and reused; only temperatures are re-read per poll.
# The reads are bash builtins: $(<file) costs ~0.01 ms and no process, while a
# `cat` per zone costs ~1.9 ms PLUS a fork+exec - ~85 such reads per poll were
# ~0.17 s of every 1.5 s poll, i.e. ~12% of a core spent heating the die this
# daemon is supposed to cool.  A function CALL is fork-free too (only a command
# substitution forks), so the readers below return their value in $READ_NUM
# instead of printing it.
ZONE_DIRS=()
ZONE_TYPE=()

zone_scan() {
	local z t
	ZONE_DIRS=(); ZONE_TYPE=()
	for z in /sys/class/thermal/thermal_zone*; do
		[ -d "$z" ] && [ -r "$z/type" ] || continue
		t=$(<"$z/type") 2>/dev/null
		[ -n "$t" ] || continue
		ZONE_DIRS+=("$z")
		ZONE_TYPE+=("$t")
	done
}

# Numeric sysfs read into READ_NUM; $2 (optional) is the fallback for an
# unreadable or non-numeric file.  Builtin: no fork, no `cat`, no `awk`.
read_num() {
	READ_NUM=$(<"$1") 2>/dev/null
	case "${READ_NUM:-}" in
	'' | *[!0-9]*) READ_NUM=${2:-} ;;
	esac
}

# --- external hold ("NPU pre-cap"), PID-leased ---------------------------------
# npu-llm leases a clamp floor by writing /run/erhai-thermal.hold (created
# world-writable here); semantics are documented in the header.
HOLD_FILE=/run/erhai-thermal.hold

ensure_hold_file() {
	[ -e "$HOLD_FILE" ] || : > "$HOLD_FILE" 2>/dev/null || true
	chmod 666 "$HOLD_FILE" 2>/dev/null || true
}

# Sets the globals hold_pid/hold_step/hold_gpu and returns 0 ONLY when a valid
# lease is live (pid > 0 and alive, step 0-4, gpu -1..5).  Returns 1 otherwise.
# All file content is range-checked before use - the file is world-writable.
read_hold() {
	hold_pid=0; hold_step=-1; hold_gpu=-1
	[ -r "$HOLD_FILE" ] || return 1
	local ln pid step gpu
	# builtin first-line read (a `head -1` is a fork+exec on every poll; a
	# trailing-newline-less file is read the same way `head` printed it).
	ln=""
	IFS= read -r ln < "$HOLD_FILE" 2>/dev/null || true
	case "$ln" in
	pid=*" step="*" gpu="*) ;;
	*) return 1 ;;
	esac
	pid=${ln#pid=}; pid=${pid%% *}
	step=${ln#* step=}; step=${step%% *}
	gpu=${ln#* gpu=}
	case "$pid" in (''|*[!0-9]*) return 1 ;; esac
	case "$step" in (''|*[!0-9]*) return 1 ;; esac
	case "$gpu" in (''|*[!0-9-]*) return 1 ;; esac
	[ "$pid" -gt 0 ] || return 1
	kill -0 "$pid" 2>/dev/null || return 1
	[ "$step" -ge 0 ] && [ "$step" -le 4 ] || return 1
	[ "$gpu" -ge -1 ] && [ "$gpu" -le 5 ] || return 1
	hold_pid=$pid; hold_step=$step; hold_gpu=$gpu
	return 0
}

# True while ANY GPU sensor is at/over the per-plane switch-on trip: there the
# kernel step_wise owns cdev2 and this governor must never fight it.
gpu_hot() {
	local i tbase=${TRIGGER:-94000}
	[ "${#ZONE_DIRS[@]}" -gt 0 ] || zone_scan
	for i in "${!ZONE_DIRS[@]}"; do
		case "${ZONE_TYPE[i]}" in
		gpuss*)
			read_num "${ZONE_DIRS[i]}/temp"
			[ -n "$READ_NUM" ] || continue
			[ "$READ_NUM" -ge "$tbase" ] && return 0 ;;
		esac
	done
	return 1
}

# hottest of the whole-die zones in milli-C (CPU clusters, GPU, NPU, DDR)
hot_die() {
	local i hot=0
	[ "${#ZONE_DIRS[@]}" -gt 0 ] || zone_scan
	for i in "${!ZONE_DIRS[@]}"; do
		case "${ZONE_TYPE[i]}" in
		cpu-* | cpuss* | gpuss* | nsphmx* | nsphvx* | ddr*)
			read_num "${ZONE_DIRS[i]}/temp"
			[ -n "$READ_NUM" ] || continue
			[ "$READ_NUM" -gt "$hot" ] && hot=$READ_NUM ;;
		esac
	done
	echo "$hot"
}

# --- kernel-side configuration ------------------------------------------------
# Put the planes we control (CPU clusters + GPU) on step_wise with their trips
# parked at BACKSTOP/120C: below that the kernel governors do nothing (one
# writer: this daemon); between BACKSTOP and the 125C hot trip the kernel gears
# up on its own should this daemon ever die.  NPU/DDR zones have no cooling
# device and are left alone; their trips are observers.
reconfigure_zones() {
	local i z t
	zone_scan
	for i in "${!ZONE_DIRS[@]}"; do
		z=${ZONE_DIRS[i]}; t=${ZONE_TYPE[i]}
		[ -w "$z/policy" ] || continue
		case "$t" in
		cpu-*)
			echo step_wise > "$z/policy" 2>/dev/null
			[ -w "$z/trip_point_0_temp" ] && echo "$BACKSTOP" > "$z/trip_point_0_temp" 2>/dev/null
			[ -w "$z/trip_point_1_temp" ] && echo "120000"       > "$z/trip_point_1_temp" 2>/dev/null
			;;
		gpuss*)
			# GPU plane: per-plane step_wise on the gpuss sensors at the profile
			# switch-on.  The die-governor NEVER writes the GPU devfreq cooling
			# device: a live-session devfreq clamp (down to 734 MHz) coincided with
			# display noise + an NVT touch recovery storm on 2026-09-24, and the
			# msm/display stack on this kernel is not proven against it.  The GPU
			# throttles itself only when IT is hot, as in v1.1 (gaming-stable).
			echo step_wise > "$z/policy" 2>/dev/null
			[ -w "$z/trip_point_0_temp" ] && echo "$TRIGGER" > "$z/trip_point_0_temp" 2>/dev/null
			[ -w "$z/trip_point_1_temp" ] && echo "120000"   > "$z/trip_point_1_temp" 2>/dev/null
			;;
		esac
	done
}

# --- the Android-style governor -----------------------------------------------
governor() {
	local profile last_profile hot new_profile
	local -i step=-1 idle=0 desired i t
	local want_c0 want_c1 want_g cur_c0 cur_c1 cur_g changed=0 hold_extra=""

	profile=$(read_state_profile)
	profile_params "$profile"
	reconfigure_zones
	ensure_hold_file

	while :; do
		# pick up profile switches from the quick-settings toggle without a bounce:
		# compare the persisted file against the running profile directly (a mtime
		# comparison against the state file itself can never fire - comparing the
		# file against a touched marker loses the race when both land in one second).
		new_profile=$(read_state_profile)
		if [ "$new_profile" != "$profile" ]; then
			profile="$new_profile"
			profile_params "$profile"
			reconfigure_zones
			echo "erhai-thermal: monitor switched to profile $CANON (trigger $((TRIGGER / 1000))C)"
		fi

		hot=$(hot_die)
		if [ -z "$hot" ]; then
			sleep "$POLL"; continue
		fi

		# desired step: highest ladder step whose threshold the hot-die crosses
		desired=-1
		for i in 0 1 2 3 4; do
			t=$(( TRIGGER + STEPS_DT[i] ))
			[ "$hot" -ge "$t" ] && desired=$i
		done

		# external hold (NPU pre-cap): a live holder's floor wins over the ladder
		# and is applied INSTANTLY (see the attack path) - the clamp must land
		# before the NPU loads, not one rung per poll after the die has already
		# crossed the trip.  A live lease that does not raise the ladder is still
		# a FLOOR below which easing cannot pass.
		hold_live=0
		if read_hold; then
			hold_live=1
			(( hold_step > desired )) && desired=$hold_step
		fi

		if (( desired > step )); then
			# attack: one rung per poll, never skip (protects the die on the way up).
			# If the die is already REALLY hot (>= 103C, i.e. desired >= 3) the rungs
			# jump straight to step 2 so the deep clamps land inside ~3 polls.
			# EXCEPTION: a live external hold raises to the full floor in one step.
			if (( hold_live && desired == hold_step )); then
				step=$desired
			elif (( desired >= 3 && step <= 1 )); then
				step=2
			else
				(( step++ ))
			fi
			idle=0
		elif (( desired < step )); then
			if (( step > 0 )); then
				# ease off one rung per poll: with the mild first rung the die re-warms
				# into the band instead of cliff-jumping; the clamp never floors below
				# step 0 while the die is simply cool-because-clamped.
				if (( hot < RELEASE_TEMP )); then
					(( step-- ))
				fi
			else
				# step == 0 and the die below the whole band: count the soak, but only
				# release once it has been below RELEASE_TEMP (65C, far under every
				# clamped equilibrium and just above the idle range) - a running load
				# can never sit there, so the soak proves the load ended.
				if (( hot < RELEASE_TEMP )); then
					(( idle++ )) || true
					if (( idle >= SOAK_POLLS )); then
						step=-1
						idle=0
						echo "erhai-thermal: released (hot-die $((hot / 1000))C, ${SOAK}s below $((RELEASE_TEMP / 1000))C)"
					fi
				else
					idle=0
				fi
			fi
		else
			idle=0
	fi
		# a live holder is the ladder floor: never ease below it.  The lease dies
		# with the npu-llm process; the normal 65C-soak release then resumes.
		if (( hold_live && step >= 0 && step < hold_step )); then
			step=$hold_step
			idle=0
		fi
		if (( step < 0 )); then
			want_c0=0; want_c1=0; want_g=0
		else
			want_c0=${STEPS_C0[step]}
			want_c1=${STEPS_C1[step]}
			# The GPU plane is the per-plane step_wise governor's.  This daemon
			# writes cdev2 ONLY to honour an explicit external-hold gpu level;
			# otherwise, as released, we reclaim any clamp it previously wrote.
			if (( hold_live && hold_gpu >= 0 )); then
				want_g=$hold_gpu
			else
				want_g=0
			fi
		fi

		read_num /sys/class/thermal/cooling_device0/cur_state 0; cur_c0=$READ_NUM
		read_num /sys/class/thermal/cooling_device1/cur_state 0; cur_c1=$READ_NUM
		read_num /sys/class/thermal/cooling_device2/cur_state 0; cur_g=$READ_NUM
		# ... and never WHILE a gpuss zone is at/over the per-plane trip: there
		# the kernel step_wise owns cdev2 and two writers would glitch the GPU.
		write_g=0
		if (( want_g != cur_g )); then
			if (( want_g > 0 )); then
				(( hold_live && hold_gpu >= 0 )) && write_g=1
			elif ! gpu_hot; then
				write_g=1
			fi
		fi

		if (( want_c0 != cur_c0 || want_c1 != cur_c1 || write_g )); then
			echo "$want_c0" > /sys/class/thermal/cooling_device0/cur_state 2>/dev/null
			echo "$want_c1" > /sys/class/thermal/cooling_device1/cur_state 2>/dev/null
			if (( write_g )); then
				echo "$want_g" > /sys/class/thermal/cooling_device2/cur_state 2>/dev/null
			fi
			(( changed++ ))
			hold_extra=""
			(( hold_live )) && hold_extra=", hold pid=$hold_pid step=$hold_step gpu=$hold_gpu"
			echo "erhai-thermal: heat=$(( hot / 1000 ))C clamp=$want_c0/$want_c1 g=$want_g (step $step$hold_extra, $CANON trigger $((TRIGGER / 1000))C)"
		fi

		cur_c0="$want_c0"; cur_c1="$want_c1"; cur_g="$want_g"
		printf 'profile=%s trigger=%d hot=%d c0=%d c1=%d g=%d step=%d changed=%d hold=%d/%d\n' \
			"$profile" "$TRIGGER" "$hot" "$cur_c0" "$cur_c1" "$cur_g" "$step" "$changed" \
			"$hold_live" "$hold_step" \
			> "$RUNSTATE" 2>/dev/null || true
		sleep "$POLL"
	done
}

# --- release everything (SIGTERM / explicit) -----------------------------------
release() {
	# Only the CPU cooling devices belong to the die-governor; the GPU plane
	# (cdev2) is the per-plane step_wise governor's and is left untouched.
	local d
	for d in 0 1; do
		echo 0 > "/sys/class/thermal/cooling_device$d/cur_state" 2>/dev/null || true
	done
	# Drop any external hold and reclaim a GPU clamp it requested, unless the
	# per-plane governor is live and owns cdev2 right now.
	if [ -n "${HOLD_FILE:-}" ]; then
		printf 'pid=0 step=-1 gpu=-1\n' > "$HOLD_FILE" 2>/dev/null || true
		if ! gpu_hot; then
			echo 0 > /sys/class/thermal/cooling_device2/cur_state 2>/dev/null || true
		fi
	fi
	echo "erhai-thermal: cooling levels released"
}

show_status() {
	local profile hot
	profile=$(read_state_profile)
	profile_params "$profile" 2>/dev/null || true
	hot=$(sed -n 's/^.*hot=\([0-9]*\).*/\1/p' "$RUNSTATE" 2>/dev/null | tail -1)
	hot=${hot:-$(hot_die)}
	echo "profile: $CANON (whole-die switch-on $((TRIGGER / 1000))C)"
	[ -z "$hot" ] || echo "hot-die: $(( hot / 1000 ))C"
	[ -r "$RUNSTATE" ] && grep -oE '(c0|step|changed)=[0-9]+' "$RUNSTATE" | tr '\n' ' '
	echo
	[ -r /sys/class/thermal/thermal_zone1/trip_point_0_temp ] && \
		printf "kernel backstop trips: cpu zone1 switch-on %sC, gpu zone23 switch-on %sC (hot 125C)\n" \
		"$(( $(</sys/class/thermal/thermal_zone1/trip_point_0_temp) / 1000 ))" \
		"$(( $(</sys/class/thermal/thermal_zone23/trip_point_0_temp) / 1000 ))"
	printf "cdev: cpu0 %s/%s cpu6 %s/%s gpu %s/%s\n" \
		"$(cat /sys/class/thermal/cooling_device0/cur_state 2>/dev/null)" "$(cat /sys/class/thermal/cooling_device0/max_state 2>/dev/null)" \
		"$(cat /sys/class/thermal/cooling_device1/cur_state 2>/dev/null)" "$(cat /sys/class/thermal/cooling_device1/max_state 2>/dev/null)" \
		"$(cat /sys/class/thermal/cooling_device2/cur_state 2>/dev/null)" "$(cat /sys/class/thermal/cooling_device2/max_state 2>/dev/null)"
	if [ -n "${HOLD_FILE:-}" ] && [ -r "$HOLD_FILE" ]; then
		if read_hold; then
			echo "hold: ACTIVE cpu-floor step=$hold_step gpu=$hold_gpu (pid $hold_pid)"
		else
			echo "hold: none"
		fi
	fi
}

apply() {
	if [ -z "$1" ]; then
		profile=$(read_state_profile)
		profile_params "$profile" || { echo "erhai-thermal: bad persisted profile '$profile'" >&2; exit 1; }
	else
		profile_params "$1" || {
			echo "erhai-thermal: unknown profile '$1'" >&2
			list_profiles >&2
			exit 1
		}
	fi
	reconfigure_zones
	ensure_hold_file
	if [ -n "$1" ]; then
		printf '# erhai thermal profile; set with: erhai-thermal.sh <profile>\nERHAI_THERMAL_PROFILE=%s\n' \
			"$CANON" > "$STATE"
	fi
	echo "erhai-thermal: profile $CANON (whole-die switch-on $((TRIGGER / 1000))C, deep floor $(( (TRIGGER + 9000) / 1000 ))C)"
}

trap 'release; exit 0' TERM INT

case "${1:-apply}" in
monitor)
	# polls of SOAK seconds at POLL intervals, rounded up (POLL may be fractional)
	SOAK_POLLS=$(awk -v s="$SOAK" -v p="$POLL" 'BEGIN{print (s > 0 && p > 0) ? int(s / p) + 1 : 1}')
	governor ;;
apply)
	apply "$2" ;;
release)
	release ;;
status)
	show_status ;;
list | -h | --help)
	list_profiles ;;
*)
	apply "$1" ;;
esac
