#!/bin/bash
# die-budget.sh -- shared die-budget library for the erhai NPU tooling.
#
# The OnePlus Pad 3 display/touch complex FAILS when the whole-die temperature
# stays >= ~97.5-98.5 C (measured repeatedly, kernel-independent; the correct
# die readings come from the thermal governor's own state file).  The NPU has
# no clock to throttle, so the ONLY safe lever is a DIE BUDGET: refuse to start
# a load when the die is already over budget and kill it if the budget is
# breached while running.  Charging/ambient shift the ramp speed, so this must
# close the loop on TEMPERATURE, not on context size (MEMORY-2026-09-25.md).
#
# Usage (sourced):
#   . die-budget.sh
#   die_budget_mc                   # echo the budget in milli-C (default 94000)
#   die_now_mc                      # echo current die in milli-C (0 if unknown)
#   die_wait_idle <mc> [<soak>]     # 0 once die stays <= budget for soak s
#   die_watch <pid> <mc> [<soak>]   # return 0 when pid exits; kill -9 on breach
#   die_budget_status               # human line: die/budget/margin
#
# DIE_BUDGET_MC overrides the budget (0 = disable the watch/start gates).

DIE_BUDGET_MC=${DIE_BUDGET_MC:-94000}     # 94 C: 3.5 C margin below the 97.5 C wall
DIE_BUDGET_SOAK=${DIE_BUDGET_SOAK:-15}    # seconds the die must hold before we act

die_now_mc() {
	local hot
	hot=$(sed -n 's/^.*hot=\([0-9]\{1,\}\).*/\1/p' \
		/run/erhai-thermal.state 2>/dev/null | tail -1)
	[ -n "$hot" ] && { echo "$hot"; return 0; }
	# fallback: the nsphmx zone the governor reads
	hot=$(cat /sys/class/thermal/thermal_zone42/temp 2>/dev/null)
	[ -n "$hot" ] && { echo "$hot"; return 0; }
	echo 0
}

die_budget_mc() { echo "$DIE_BUDGET_MC"; }

# Wait until the die has been AT OR BELOW the budget for <soak> consecutive
# seconds (deadline: <deadline> s; default 180).  Returns 0 = ready,
# 1 = budget disabled, 2 = timed out before the die came under budget.
die_wait_idle() {
	local budget=${1:-$(die_budget_mc)} soak=${2:-$DIE_BUDGET_SOAK} \
		deadline=${3:-180} ok=0 t started
	[ "$budget" -eq 0 ] && return 1
	started=$(date +%s)
	while [ "$ok" -lt "$soak" ]; do
		t=$(die_now_mc)
		if [ "$t" -le "$budget" ]; then ok=$((ok + 1)); else ok=0; fi
		if [ $(( $(date +%s) - started )) -ge "$deadline" ]; then
			return 2
		fi
		sleep 1
	done
	return 0
}

# Watch <pid>: return 0 when it exits on its own; kill -9 it and return 1 if
# the die is over budget for <soak> consecutive seconds.
die_watch() {
	local pid=${1:?die_watch: pid} budget=${2:-$(die_budget_mc)} \
		soak=${3:-$DIE_BUDGET_SOAK} breach=0 t
	[ "$budget" -eq 0 ] && { while kill -0 "$pid" 2>/dev/null; do sleep 2; done; return 0; }
	while kill -0 "$pid" 2>/dev/null; do
		t=$(die_now_mc)
		if [ "$t" -ge "$budget" ]; then
			breach=$((breach + 1))
		else
			breach=0
		fi
		if [ "$breach" -ge "$soak" ]; then
			echo "die-budget: kill: die ${t} mC >= budget ${budget} mC for ${soak}s (pid ${pid})" >&2
			kill -9 "$pid" 2>/dev/null
			return 1
		fi
		sleep 1
	done
	return 0
}

die_budget_status() {
	local now b
	now=$(die_now_mc); b=$(die_budget_mc)
	if [ "$b" -eq 0 ]; then
		echo "die-budget: DISABLED (die ${now} mC)"
	else
		echo "die-budget: die ${now} mC / budget ${b} mC (margin $((b > now ? b - now : 0)) mC)"
	fi
}