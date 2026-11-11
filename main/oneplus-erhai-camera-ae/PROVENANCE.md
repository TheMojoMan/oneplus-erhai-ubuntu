# oneplus-erhai-camera-ae: provenance

Vendored from https://github.com/nadelacruz-00/erhai-camera-ae at commit
`a18016f50324bb3129781589330f751f48878863` (2026-11-11: every stream starts
at the base frame length, like ErhaiCam). The upstream README there has the
full design notes, the device verification, and the
after-a-libcamera-upgrade checklist.

Maintainers: nadelacruz-00 (engine), TheMojoMan (signs and publishes the .deb).

## Credits and licence

- **ErhaiCam** is by **TheMojoMan**. The engine ports ErhaiCam's
  auto-exposure controller (`ae_step`), its frame statistics
  (`sample_gray`/`stats_cpu`) and its autofocus loop, constants included,
  from `erhai-cam.cpp`.
- ErhaiCam's autofocus algorithm comes from **CFM880**'s nabu-camera
  project (`nabu-autofocus`).
- GPL-2.0-only, as a derived work of ErhaiCam (see `LICENSE`, shipped as
  `/usr/share/doc/oneplus-erhai-camera-ae/copyright`).

## What was vendored

| Here | Upstream | Notes |
|---|---|---|
| `camera_ae/` | `camera_ae/` | patched, see below |
| `tests/` | `tests/` | one path patched; run by the builder, never packed |
| `tuning/simple.yaml` | same | unchanged |
| `LICENSE` | same | unchanged |
| `camera-ae.py` | `camera-tone.py` | rewritten for the system path, adds `doctor` |
| `systemd/erhai-camera-ae.service` | `systemd/camera-gamma.service` | renamed, system path |
| `systemd/{pipewire,wireplumber}-camera-ae.conf` | same names | system tuning path |
| `DEBIAN/` | (none) | replaces `install.sh` |

Not vendored: `install.sh` (per-user installer; the deb replaces it),
`legacy/`, `tools/` (on-device test helpers), `README.md`.

## Patches against upstream

The upstream tree is a per-user install under `~/.local`; the deb is
system-wide. Only paths and the unit name change; the engine logic is
untouched.

- `camera_ae/override.py`: `DEFAULT_DIR` is
  `/usr/share/oneplus-erhai-camera-ae/ipa` (was `~/.local/share/camera-ae/ipa`).
- `camera_ae/doctor.py`: the install record is
  `/var/lib/oneplus-erhai-camera-ae/installed.txt` (written by `postinst`;
  was `~/.local/share/camera-ae/installed.txt`); the checked unit is
  `erhai-camera-ae.service` (was `camera-gamma.service`); the activation hint
  says "log out and back in, or reboot".
- `camera_ae/main.py`: docstring names the new unit.
- `tests/test_doctor.py`: the sample tuning path is the system one.
- Per-user state stays per user: `~/.local/state/camera-ae/remembered.json`.

## Payload (`build-camera-ae-deb.sh`)

```
/usr/lib/oneplus-erhai-camera-ae/camera-ae.py
/usr/lib/oneplus-erhai-camera-ae/camera_ae/*.py
/usr/share/oneplus-erhai-camera-ae/ipa/simple/{sc820cs,sc1320cs,uncalibrated}.yaml
/usr/lib/systemd/user/erhai-camera-ae.service
/usr/lib/systemd/user/pipewire.service.d/oneplus-erhai-camera-ae.conf
/usr/lib/systemd/user/wireplumber.service.d/oneplus-erhai-camera-ae.conf
/usr/share/doc/oneplus-erhai-camera-ae/{copyright,PROVENANCE.md}
```

`postinst` runs `systemctl --global enable erhai-camera-ae.service` and writes
the libcamera fingerprint; `postrm` removes the enable symlink (and, on purge,
`/var/lib/oneplus-erhai-camera-ae`). I2C and lens access for the session user
come from `oneplus-erhai-camera`'s uaccess rules.

## Use

- Check: `python3 -I /usr/lib/oneplus-erhai-camera-ae/camera-ae.py doctor`
- Switch off for one user: `systemctl --user edit erhai-camera-ae.service`,
  add `Environment=CAMERA_AE=off` (or `CAMERA_AF=off`), then
  `systemctl --user restart erhai-camera-ae.service`. With the override
  active and the engine off, PipeWire camera exposure stays frozen.
- `CAMERA_AE_TARGET` (default 0.45, clamped 0.2-0.7) sets the target luma.
- Full rollback: remove the package, then log out and back in.
