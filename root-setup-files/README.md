# root-setup-files

Everything `build-root.sh` copies into the OnePlus Pad 3 (erhai) rootfs image.

```
overlay/         mirrors the rootfs 1:1 — copied in with `cp -a overlay/. rootdir/`
apt/             Mozilla Firefox .deb repo (signing key, deb822 source, priority pin)
dconf-seed/      00-erhai-greeter — greeter dconf keys, compiled by gdm's own
                 ExecStartPre into /var/lib/gdm3/greeter-dconf-defaults
skel-dconf.txt   the per-user SEED IN TEXT FORM — hand-editable source of truth
skel-dconf.ini   HISTORICAL reference only — nothing compiles it any more
```

## etc/skel/.config/dconf/user (the per-user dconf seed)

A pre-built **binary GVDB** and what a freshly created account starts with.
`build-root.sh` copies the overlay verbatim and only CHECKS that the file is
there (it aborts if missing, because gjs-osk would not be enabled). The old
`dconf compile` step from `skel-dconf.ini` was removed 2026-09-05: whenever the
build host had `dconf`, it silently recompiled the seed from that stale ini and
refreshed seeds never landed in the image — so the GVDB stays the SHIPPED
artifact and nothing compiles `skel-dconf.txt` at build time.

### Editing the seed (`skel-dconf.txt` + `tmp/dconf-text.py`)

`skel-dconf.txt` is the same database **decoded to text** (glib keyfile /
GVariant syntax), so it can be read, diffed and hand-edited. The binary is
generated from it with the editor in `tmp/dconf-text.py`. Run these from the
workspace root (`/media/data/develop/erhai`):

```sh
# see what the shipped seed has vs. the template (read-only, exit != 0 if they differ)
python3 tmp/dconf-text.py check \
        oneplus-erhai-ubuntu/root-setup-files/skel-dconf.txt \
        oneplus-erhai-ubuntu/root-setup-files/overlay/etc/skel/.config/dconf/user

# compile the edited text onto the seed (prints a key-level diff, backs the old
# one up into tmp/dconf-text-backup/, keeps root:root 0644)
python3 tmp/dconf-text.py apply \
        oneplus-erhai-ubuntu/root-setup-files/skel-dconf.txt \
        oneplus-erhai-ubuntu/root-setup-files/overlay/etc/skel/.config/dconf/user

# or edit a database straight in $EDITOR (same diff/backup/compile path)
python3 tmp/dconf-text.py edit <db>
```

`dump`, `extract`, `keylist`, `get`, `set`, `del`, `diff`, `roundtrip` and
`verify` round the tool out (`--help` lists them). Two rules it enforces:

* **Never `dconf write/reset` on a file you intend to ship.** Those are
  delegated over D-Bus to the running session's dconf *service*, which ignores
  the client's `XDG_CONFIG_HOME` and edits the live user's own database. The
  editor reads a private copy and writes via `dconf compile` — a plain file
  write.
* Editing the **running** session's `~/.config/dconf/user` needs `--force`: the
  session holds its values in memory and rewrites the whole file on its next
  change, so an external edit is easily lost (use `gsettings set` for live
  tweaks).

`python3 tmp/dconf-text.py roundtrip <db>` proves the decode → compile → decode
trip is lossless for a given database (both current databases are STABLE).

### Regenerating the template from a live session

```sh
python3 tmp/seed-user-defaults.py --emit-text oneplus-erhai-ubuntu/root-setup-files/skel-dconf.txt
python3 tmp/seed-user-defaults.py --apply-sudo          # + install the compiled GVDB
```

That tool is the *policy* one: it starts from an explicit ALLOWLIST of groups
(everything else is dropped), takes the look&feel keys from the live database
(pinned apps, apps-overview folders + grid layout, wallpaper, dark mode,
accessibility icon, rotation lock), forces `sources=[('xkb', 'us')]`, and
refuses to write unless the result passes an allowlist check, a
personal-marker scan and a list of required-key assertions. `--apply-sudo`
installs the compiled GVDB; without it nothing is touched. The older tool that
refreshes **only** the gjs-osk groups is still available as
`python3 tmp/seed-gjsosk-refresh.py --apply`.

**Do not ship personal state.** A wholesale copy of a live database drags the
session's own data into every new user's defaults: the calendar's
`reminders-past` (real events with CalDAV UIDs), absolute `last-folder-path`s
(`/home/<user>/…`, `/media/data/develop/erhai/…`), deja-dup's
online-account backend, per-app notification and window state, terminal/Ptyxis
profile uuids, `nm-applet` EAP entries. `skel-dconf.txt` is the stripped form:
40 groups / 125 keys out of the 122 groups / 315 keys it was captured from.

## Placeholders

Two tokens are substituted after the overlay is copied, so the desktop user is
set in one place (`USERNAME`/`USERUID` in `build-root.sh`):

| token | used in |
|---|---|
| `@USERNAME@` | `etc/gdm3/custom.conf`, `usr/local/sbin/erhai-desktop-gl.sh`, `usr/local/sbin/erhai-cam-arm.sh` |
| `@USERUID@` | `usr/local/sbin/erhai-cam-arm.sh` (`XDG_RUNTIME_DIR=/run/user/<uid>`) |

## What is in overlay/

### Services (`etc/systemd/system/`, all enabled by `build-root.sh`)

| unit | why |
|---|---|
| `erhai-touchscreen.service` | Rebinds `NVT-ts-spi spi0.0` until the firmware reports *boot firmware ready*, waits for the input node, **then starts gdm**. The panel is in-cell: the touch sense layer only syncs during a display-manager bring-up, so gdm is disabled and this is the only thing that starts it. |
| `erhai-desktop-gl.service` | Picks the GL environment to match the booted kernel (`environment.d` is read once, when the user manager starts). GPU node okay → freedreno/turnip, otherwise llvmpipe. |
| `erhai-gpu-pin.service` | Arms the GMU (dev kernels gate it behind `msm.erhai_gmu_enable`) and pins `power/control=on` before the display manager. |
| `erhai-host-charge.service` | Ramps the ADSP input current limit while the tablet is USB data-host, so a powered hub/charger actually charges (no BC1.2 detection runs in DFP mode; ICL otherwise sits at the 100 mA SDP default). |
| `erhai-power-toggle.service` | evdev watcher on `pmic_pwrkey` → backlight blank/unblank, Android style. |
| `erhai-thermal.service` + `erhai-thermal@.service` | Applies the CPU (IPA) + GPU (step_wise) thermal profile from `/etc/default/erhai-thermal`; the templated unit is what the GNOME Quick Settings toggle starts. |
| `erhai-cam-arm.service` | Arms the CCI CSR and binds both sensors — camss only completes its async notifier once *both* are bound. |

### Config

| path | what |
|---|---|
| `etc/gdm3/custom.conf` | Wayland + autologin `@USERNAME@` |
| `etc/default/erhai-thermal` | active thermal profile (`performance`/`balanced`/`cool`/`cold`) |
| `etc/bluetooth/main.conf` | BlueZ config carrying `FilterDiscoverable = false` — the OnePlus Pencil advertises Flags `0x04` (BR/EDR-Not-Supported, **no** discoverable bit), which bluez's default filter drops before the device object is created, so the pen can never be paired. `apt` runs with `--force-confold`, so this overlay copy wins over bluez's own conffile. (`oneplus-erhai-bluetooth` 2.3 makes the same change on a live system.) |
| `etc/dconf/{profile/user,db/local.d/00-erhai}` | GNOME defaults: power button does nothing (our watcher owns it), user extensions enabled, erhai extensions listed. `dconf update` compiles them. |
| `etc/polkit-1/rules.d/50-erhai-thermal.rules` | lets the local desktop user start `erhai-thermal@<profile>` without a password |
| `etc/systemd/logind.conf.d/10-erhai-powerkey.conf` | `HandlePowerKey=ignore`, long press still powers off |
| `usr/lib/firmware/wlan/qca_cld/WCNSS_qcom_cfg.ini` | qcacld platform config (`scan_mode_6ghz=0` so 5GHz networks show at cold boot) — shipped by the **oneplus-erhai-wifi** deb, together with the `10-mt7921u-usb-wifi.link` that pins the mt7921u dongle's factory MAC (v7.2 mt76 fails the EFUSE read and would randomise it every boot, renaming the interface and breaking the saved NM profile). The erhai-wcn-* service units, their scripts, wl_drvcmd and the modprobe.d policy all live in that deb too |
| `etc/udev/rules.d/70-erhai-cam-i2c.rules`, `99-nvt-touch-calibration.rules` | camera I²C permissions, touch calibration |
| `usr/share/alsa/ucm2/` | *(not in the overlay)* — erhai UCM profile + `conf.d/sm8750/` selection now ship in the `oneplus-erhai-audio` deb (see packages/README.md); PipeWire/ACP applies the HiFi verb at session start |
| `usr/share/gnome-shell/extensions/erhai-{thermal,cam-focus}@erhai.local/` | Quick Settings thermal toggle, camera focus control |
| `etc/skel/.config/monitors.xml` | 3392×2400 @ scale 2 (HiDPI) |
| `etc/skel/.bash_aliases` | shell shortcuts (`sau`/`saug`/`sai`, snap, fastboot, `..`/`...`, `l`) |
| `etc/skel/.config/dconf/user` | pre-built GVDB, the seed a new user starts with (source of truth — see the section above) |
| `etc/skel/.config/environment.d/10-desktop.conf{,.gpu,.llvmpipe}` | GL env + the two templates `erhai-desktop-gl.service` switches between |
| `etc/fstab` | `PARTLABEL=linux / ext4 …,x-systemd.growfs` |

## Notes

* The GPU template selects `GALLIUM_DRIVER=freedreno`. The mesa **/usr
  takeover** replaced Ubuntu's six runtime mesa packages in place (same names,
  higher version — no env opt-in, no `/opt/mesa`), so a session needs nothing
  from these templates but the driver name; `erhai-desktop-gl.service` still
  swaps between `10-desktop.conf.gpu` (hardware) and `.llvmpipe` (software)
  per boot. System Mesa is too old for gen8: its freedreno gallium refuses the
  chip and its turnip does not know the chip id, which is why the replacement
  set exists at all.
* Firmware blobs are **not** here; they ship in the `oneplus-erhai-firmware`
  .deb (ADSP, WCN BT, camera, iris `vpu35_p4.mbn`, aw88261 ACF, tplg, QDSS cfg).
