# Status — Ubuntu on OnePlus Pad 3 (erhai)

## What works

**First start**
* The first boot walks you through setting up your user account.

**Screen & graphics**
* Full **3392×2400** edge-to-edge panel, crisp 2× HiDPI scaling.
* Refresh rates **30 / 48 / 50 / 60 / 90 / 120 Hz**, changeable anytime
  (default 120 Hz).
* Brightness control.
* Short press of the power button: screen **completely** off (not just the
  backlight); press again to wake — takes 3-4 seconds.
* **Hardware accellerated graphics** for the desktop, games and apps.
* **External monitor** over USB-C, also through a hub with HDMI.
* **Hardware video decoding** in the browsers — smooth playback, less battery.

**Input**
* Multi-touch touchscreen — see **Touchscreen & pen** below.
* Magnetic **keyboard** with trackpad, plus any USB/Bluetooth keyboard or
  mouse. Connects instantly; auto-rotation pauses while it is attached.
* Fold the keyboard cover over the screen: screen and touchscreen switch off
  with a click, folding back wakes them (one-time setup prompt).

**Touchscreen & pen**
* **Pen** — the **OnePlus Pencil 2** pairs over Bluetooth and works; the magnetic
  pen dock charges it (**CPS8601**) and stops at 100 %.
* **Dock** — drag the app icons with one finger to scroll them.
* **Patched Desktop** — one-finger **long press** (background or icon) opens the menu.
* **Patched Files app (Nautilus)** — long press the **empty space** for the menu;
  **Show Select Boxes** in settings adds tick boxes for multi-select; drag files
  with one finger; rename works with the on-screen keyboard.
* **Patched Terminal app (ptyxis)** — finger scrolling, long-press to mark and copy text,
  **Paste** button in the header bar.
* **On-screen keyboard** — never pops up by itself while the magnetic keyboard
  is attached; Shift/Ctrl/Alt work;
  uses a patched version of GJS OSK by Vishram1123
* A three-finger swipe switches workspaces — and no longer crashes when files app
  is in multi-select mode.

**Sensors & LED**
* All sensors functional.
* Auto-rotation and automatic brightness (both in the quick settings).
* Set **torchlight** in quick-settings-menu.

**Internet & power**
* **Wi-Fi** and **Bluetooth** built in (USB Wi-Fi sticks also work).
* **USB-C**: hubs, storage, external displays and charging — even while the
  tablet hosts USB devices.
* **Fast charging**, on both erhai models: **SuperVOOC** (Southchip **SC8547A**
  charge pump) and **PPS/PD** (**PMIH0108**); the models differ only in the
  battery gauge (**TI bq27z561** vs **Qualcomm QBG**).
* **Bypass charging**: run from the charger with the battery held (quick
  settings; also automatic at the charge limit).
* Honest **battery percentage**, temperature and current; configurable charge
  limit.

**Sound & camera**
* **Six speakers** (stereo), plus sound through USB-C monitors.
* Rear and front camera; use **ErhaiCam** app (autofocus, upright photos, video).

**System**
* **dual-boot with Android**.
* **OTA (over-the-air) updates** of kernel and apps.
* Four **performance profiles** (cold ~70°C / cool ~80°C / balanced ~90°C / performance ~100°C) in the quick
  settings lets you limit temperature of the pad.
* **NPU** works - use **`npu-llm`** to run local LLMs: `npu-llm pull
  unsloth/Qwen3-4B-GGUF:Q4_0` once, then `npu-llm chat Qwen3-4B` (or
  `npu-llm run Qwen3-4B "prompt"`); beware of overheating (see below).

**Gaming**
* Run `sudo apt install steam` for an up-to-date version of the arm64 steam client
  that also asks for location where steam data and games should be stored;
  do not use the snap version that Ubuntu offers - it ships outdated graphics drivers
  and does not run on erhai
  
## What does not work (yet)

* **Real sleep** — screen off keeps the tablet running (the battery drains).
  The manual **Suspend** in the power menu is experimental and auto-sleep is
  off: shut the tablet down overnight instead.
* **144 Hz** — removed on purpose (the panel misbehaves sometimes at this rate).
* **Internal microphones** -> no video recording with sound.
* **USB-C headphones** — untested with real hardware.

## Tips & known quirks

* Reboot 2 x for correct scaling of login screen after initial setup.
* Wifi takes about 15 sec to come up after desktop loads. 5 GHz show up later than
  2.4 GHz networks (be patient).
* Reboot to Android does not work when tablet is hot. Let it cool down then retry.
* If the GPU freezes (rare), restart the tablet -> hold POWER + VOL-UP + VOL-DOWN
  for ~15 sec.
* Running LLMs on the NPU **overheats** tablet which leads to severe screen quirks -
  always use a cooler!!!
* Don't disable the touchscreen, sound or camera "autostart" services.
* Keep it updated: `sudo apt update && sudo apt upgrade`, short `sau && saug`.

---

Back to [README](../README.md) · Next → [Credits](credits.md)
