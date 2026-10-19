# oneplus-erhai-thermal 1.2 — Android-style whole-die thermal governor

`erhai-thermal.sh` implements on Linux the SAME thermal architecture Android
uses on this exact SoC (SM8750): a userspace governor (`thermal-engine-v2` on
Android) watches the **whole-die envelope** and writes cooling LEVELS into the
kernel cooling devices as the envelope approaches its trip band.

## The Android reference (measured, burn-in test 2026-09-24)

Under a sustained **CPU+GPU+NPU** load, Android holds the SoC envelope at
**92–106 °C** with:

| plane | what Android does | result |
|---|---|---|
| CPU cluster0 (cores 0-5) | cooling level ~12 | **960 MHz** (27 %) |
| CPU cluster1 (cores 6-7) | cooling level 15 | **1018 MHz** (24 %) |
| GPU (Adreno) | devfreq levels 4-5 | 1100 → 832 → **734 MHz** |
| NPU | *nothing* (clock lives in the Hexagon runtime, its own DCVS) | runs on |
| cores/offlining/DDR/UFS/charger | never used | — |
| apps | never killed; only brightness gets capped (OPPO display manager) | — |

The trip order is what this package reproduces: the prime cluster's first
passive trip is **92 °C**, the other SoC sensors sit ~100–105 °C, and the hot
trip is 125 °C. Nothing is ever approached because the CPU/GPU clamps stop the
planes from adding heat once the die enters the trip band.

## What this package does

* At boot (`erhai-thermal.service`, Type=simple) and on every profile switch
  the kernel zones for the planes we control are set to **step_wise** with
their switch-on trips parked at **109 °C** — so the in-kernel governors stay
quiet below that (= **one writer**: this daemon), and the kernel gears up on
its own only in the 109–125 °C band as a backstop should the daemon ever die.
* The governor polls the **hottest** of all SoC sensors (`cpu-*`, `cpuss-*`,
  `gpuss*`, `nsphmx*`, `nsphvx*`, `ddr` — NPU and DDR included, which the old
  v1.1 power_allocator tables were **blind to**: that blindness is what let a
  full NPU load overheat the device) and writes the two **CPU** cooling
devices (cpufreq-cpu0 / cpufreq-cpu6) with a graduated ladder (one level per
poll, hysteresis, and a soak timer before full release):

  The **GPU plane is NOT driven by the die-governor**: the gpuss zones run
their own per-plane `step_wise` at the profile switch-on (v1.1 semantics).
The Adreno devfreq is a *display* device on this kernel, and a live-session
clamp down to 734 MHz (Android's value) coincided on 2026-09-24 with display
noise + an NVT in-cell touch recovery storm. The GPU throttles itself only
when IT is hot.

| step | hot-die ≥ | CPU cluster0 | CPU cluster1 | GPU |
|---|---|---|---|---|
| step | hot-die ≥ | CPU cluster0 | CPU cluster1 |
|---|---|---|---|
| 0 | trigger | level 6 (2227 MHz) | level 6 (3072 MHz) |
| 1 | +3 °C | level 8 (1785 MHz) | level 8 (2649 MHz) |
| 2 | +6 °C | level 10 (1363 MHz) | level 10 (2246 MHz) |
| 3 | +9 °C | level 12 (**960 MHz**) | level 15 (**1018 MHz**) |
| 4 | +12 °C | level 15 (384 MHz) | level 15 (1018 MHz) |

Step 0 is deliberately MILD: the first rung's clamp (≈2.7 GHz) lands the die INSIDE
the band so the governor can settle on the mildest clamp that holds the envelope,
instead of cliff-jumping straight to Android's deep levels (that overshoot used to
release mid-load). Step 3 = Android's measured steady state (960/1018/734 MHz);
step 4 (≈106 °C) is the deepest rung. The clamp releases fully only after the
die has been below **65 °C** (`RELEASE_TEMP`) for `ERHAI_THERMAL_SOAK` seconds.
65 °C is deliberately absolute: every ladder rung holds a running load at
≥ ~87 °C (measured), while the idle die sits at 45–56 °C, so a running load can
never trip the release and the clamps unwind within seconds of a load ending.
The per-profile `TRIGGER`/`RELEASE_TEMP` are in `/usr/local/sbin/erhai-thermal.sh`.

## Profiles (whole-die switch-on trigger)

| profile | trigger °C | deep floor °C | use |
|---|---|---|---|
| performance | 94 | 103 | stock Android behaviour, safe to its ~106 °C envelope |
| balanced | 84 | 93 | ~90 °C ceiling |
| cool | 74 | 83 | ~80 °C ceiling |
| cold | 64 | 73 | ~70 °C, lighter gaming |

Aliases: `100/90/80/70`, `android`, `stock`, `perf`, `gaming`.

## Switching

* GNOME Quick Settings → **Thermal** toggle (`erhai-thermal@erhai.local`;
  polkit permits the desktop user without a password).
* `sudo erhai-thermal.sh <profile>` — applies now and persists to
  `/etc/default/erhai-thermal`; the running governor picks the switch up within
  one poll interval (no service bounce).

## Tunables (`/etc/default/erhai-thermal`)

* `ERHAI_THERMAL_POLL` — envelope poll seconds (default 1.5).
* `ERHAI_THERMAL_SOAK` — cool seconds below the release trip before releasing
  (default 15).
* `ERHAI_THERMAL_BACKSTOP` — kernel step_wise switch-on trip, milli-°C
  (default 109000; the kernel then owns the 109–125 °C band only).

## Operations

* `erhai-thermal.sh status` — profile, hot-die temp, current clamps, kernel
  backstop trips.
* `systemctl stop erhai-thermal.service` — drops all cooling levels to zero
  (SIGTERM trap + ExecStop; idempotent) and stops the governor.
* Governor transitions are logged to the journal as `erhai-thermal:` lines;
  the latest state is in `/run/erhai-thermal.state`.

## Why v1.1 (power_allocator/IPA) was replaced

IPA is a *power* regulator that can only see the zones it is attached to — the
CPU planes. A heavy NPU workload (e.g. llama.cpp on the Hexagon HTP) heats the
`nsphmx`/`ddr` sensors while the CPU stays cool, so IPA had nothing to throttle
and the die ran away with no reaction until the 120–125 °C kernel trips. The
v1.2 governor watches the envelope *including* the NPU and DDR sensors and
does the same thing Android does when they climb: it clamps the CPU and GPU
planes. The NPU itself has no cooling device (as on Android) — its clock is
managed inside the Hexagon runtime, and the envelope clamp keeps the whole
package inside the same 92–106 °C window the Android stack achieves.
