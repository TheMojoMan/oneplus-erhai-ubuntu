# holdout — built but NOT published

`update-ppa.sh --mv` moves every `<suite>/debs/*.deb` into the PPA suite and
does **not** descend into subdirectories. Debs parked here are therefore out of
reach of a publish run.

Contents (2026-11-22, parked while publishing release kernel `+b6`):

- `oneplus-erhai-video_1.12|1.13|1.14` — release-numbered builds newer than the
  published 1.11, never owner-published.

Move a deb back to `<suite>/debs/` when the owner orders its publish.
