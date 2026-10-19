# steam (arm64) — packages README

`apt install steam` on an aarch64 machine gives Valve's **native 64-bit ARM
Steam client**. This is not Canonical's snap, not the i386 client under an
emulator, and not a distribution repack of the x86_64 client.

The package is a **10 MB bootstrap**: it installs Valve's own bootstrap binary
(`steamrtarm64/steam`, from the `steam_client_publicbeta_linuxarm64` channel)
and the client then **downloads the rest of itself** on the first start — that
is exactly how Valve's own packages work, so shipping the client tree would only
duplicate what the CDN already delivers to every user.

## Where things live

| Path | What it is |
|---|---|
| `/usr/lib/steam` | the read-only **bootstrap** (dpkg-owned, ~10 MB) |
| `/usr/lib/steam/VERSION` | stamp: package version + client build |
| `/usr/bin/steam` | the launcher |
| `/etc/steam/launcher.conf` | root-owned, all-commented override file |
| `~/.local/share/Steam` | the **writable** Steam root (seeded on first start) |
| `~/.local/state/erhai-steam` | launcher state |
| `~/.steam` | Valve's bootstrap symlinks (`root`, `steam`, `sdk*`, `bin*`) |

## Why `/usr/lib/steam` is not where the client runs from

Steam needs a root directory it can **write** to: the client updates itself and
stores `config/`, `appcache/`, `logs/`, `steamapps/` and `userdata/` inside its
own tree. A dpkg-owned directory cannot be that place (a self-update there
would corrupt the package and break for every other account).

So `/usr/lib/steam` holds the **bootstrap only** and `/usr/bin/steam` seeds it
into `~/.local/share/Steam` — Valve's own default Steam root. Seeding only ever
**fills in** missing files: once the client has updated itself, its copy is
newer than the packaged one and is never overwritten, and nothing is ever
deleted. `rsync --ignore-existing` is used when available, `cp -an` otherwise.

The seed also carries the usual (empty) `config/`, `steamapps/`, `userdata/`,
`logs/`, `appcache/`, `depotcache/`, `friends/`, `package/` and `steam/cached/`
directories, the `.steam-enable-steamrt64-client` sentinel and the beta
selector.

## Where the data lives (chosen on the first start)

The client downloads ~3.1 GB and keeps every game next to it, so **the first
start asks where that data should live** instead of quietly filling the root
filesystem:

* it lists your home folder (the default) plus every **writable, persistent**
  partition that has room — so on a machine with a data partition you can put
  Steam there, the way the reference setup on this tablet keeps its root on
  `/media/data`;
* `choose another folder…` opens a folder picker (you can create one);
* tmpfs/ramfs are never offered (data there would not survive a reboot), and a
  location with less than 8 GB free asks for confirmation;
* the answer is remembered in `~/.config/erhai-steam.conf`. **Delete that file**
  (or start with `STEAM_ASK_HOME=1 steam`) to be asked again;
* the question is asked **once** — a location that is already set, or a Steam
  root that already exists, is never questioned again.

Precedence, highest first: `STEAM_HOME` in the environment → `STEAM_HOME` in
`/etc/steam/launcher.conf` (root-owned, machine-wide, no dialog) → the saved
answer → the dialog → the default `~/.local/share/Steam`.

Changing the location later does **not** move an existing installation: pick the
new place in the dialog and Steam downloads itself there again (the old folder
stays until you delete it). No desktop, no `zenity`? Then the default is used
silently — set `STEAM_HOME=/path steam` instead.

## First start

1. `steam` (from the menu, or the shell) asks where the data should live (first
   start only), seeds the bootstrap and says so on stderr.
2. The client then downloads itself: **~654 MB downloaded, ~3.1 GB on disk**
   (measured on a scratch HOME from exactly this bootstrap, 2026-10-17),
   including the **steamrt3c container runtime** it now runs inside, the UI, and
   the x86 compatibility trees it still ships. Give it time — this is a one-off.
3. When it is done, Steam shows its **sign-in window and asks for Steam
   credentials** — no account data is packaged.

Verification stays ON until that download has finished (with `-noverifyfiles`
the client refuses to fetch anything and dies with *"Failed to load
steamui.so"*); afterwards it is skipped for a fast start, which is what a normal
Steam start does. `STEAM_FORCE_VERIFY=1` / `STEAM_NO_VERIFY=1` override the
choice for troubleshooting.

## Games

* **Windows titles** — Settings → Compatibility → *Enable Steam Play for all
  other titles*, then pick an **ARM** Proton (Proton ARM / Proton
  Experimental ARM64; FEX is inside Proton). x86 Proton builds will not run.
* **Native x86-64 Linux titles** — need a host FEX rootfs; set `FEX_ROOTFS`
  in `/etc/steam/launcher.conf`.
* Free space: ~3.1 GB in the user's Steam root once the client has downloaded
  itself, plus whatever the games need. Nothing of that is in `/usr`.

## Removing

`apt purge steam` removes the packaged bootstrap and launcher only. Every
account's `~/.local/share/Steam` (games, saves, settings) is deliberately left
alone — delete it by hand if you really want it gone.

## Notes

* Not affiliated with or endorsed by Valve Corporation. Steam is a trademark
  of Valve Corporation; the client is distributed by Valve under the Steam
  Subscriber Agreement.
* The launcher honours `STEAM_PAYLOAD_DIR` and `STEAM_HOME` from the
  environment, which is how a rebuild can be tested without installing it.
