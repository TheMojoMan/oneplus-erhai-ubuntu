# oneplus-erhai-wcn

Host-side driver stack for the **WCN7880** Wi-Fi/Bluetooth combo on the
OnePlus Pad 3 (`erhai`, SM8750), for use with the erhai kernel
(<https://github.com/TheMojoMan/oneplus-erhai-linux>).

This is the Qualcomm CNSS/qcacld stack — the same "Android-style" path the
device uses in its stock firmware. It is **vendored into this repository** as a
plain source tree (see [Vendoring / provenance](#vendoring--provenance)); it is
not part of the mainline-shaped kernel tree, which only receives the driver
copies and support headers that `install.sh` puts there.

`ath12k` is **not** used here. It does not reach mission mode on this chip; it
manages only a one-shot cold-boot calibration. The netdev you actually get
(`wlan0`) comes from `wlan.ko` below.

## What is in here

| Path | Builds as | Notes |
|---|---|---|
| `drivers/net/wireless/cnss2` | `cnss2.ko` | CNSS platform driver, PCIe/QMI/MHI bring-up |
| `drivers/net/wireless/cnss_genl` | `cnss_nl.ko` | netlink transport |
| `drivers/net/wireless/cnss_prealloc` | `cnss_prealloc.ko` | memory pools |
| `drivers/net/wireless/cnss_utils` | `cnss_utils.ko`, `wlan_firmware_service.ko`, `cnss_plat_ipc_qmi_svc.ko` | QMI services |
| `drivers/staging/qcacld-3.0` | `wlan.ko` | the WLAN driver itself; builds out-of-tree |
| `drivers/staging/qca-wifi-host-cmn` | — | shared QDF/UMAC/DP code, reached via the `cmn` symlink |
| `drivers/staging/fw-api` | — | firmware interface headers |
| `drivers/staging/platform/inc` | — | platform headers qcacld includes |
| `include/` | — | headers the port needs, installed into the kernel tree |

`fw-api` carries only `fw/` and `hw/peach/`. The upstream drop has register
headers for eighteen chips and is 152 MB; `peach` is the one this device uses.

A note on `include/`: some of those headers are not vendor code but things
mainline has removed. `linux/of_gpio.h` went away in commit `b6420bd5aa0c`
("gpio: remove of_get_named_gpio() and `<linux/of_gpio.h>`") and cnss2 still
uses that interface, so it is carried here rather than patched out of a large
body of vendor source.

## Kernel requirements

The erhai kernel must already carry the WCN host support — the PCIe link
bring-up, the WCN7880 power sequence, the PCIe PHY clock release and the MHI
bandwidth-scaling handshake. Those are in the kernel repository; you do not
need to do anything beyond using a recent build of it.

Two configuration points matter:

- `CONFIG_PCI_PWRCTRL_PWRSEQ=y` — **must be built in**, not a module. The PCIe
  host controller is built in and scans the bus at boot; if the pwrctrl
  `->power_on` hook is not present by then the endpoint is never powered and
  the link fails with `qcom-pcie 1c00000.pcie: Device not found`.
- `CONFIG_POWER_SEQUENCING_QCOM_WCN=y`.

## Building

    ./install.sh /path/to/erhai/linux

That copies the cnss drivers and support headers into the kernel tree and wires
them into `drivers/net/wireless/{Kconfig,Makefile}`. It is idempotent.

Then enable them and build the kernel:

    cd /path/to/erhai/linux
    ./scripts/config --module CNSS2 --enable CNSS2_QMI --enable CNSS2_DEBUG \
                     --module CNSS_GENL --module CNSS_UTILS \
                     --module CNSS_QMI_SVC --module CNSS_PLAT_IPC_QMI_SVC \
                     --module WCNSS_MEM_PRE_ALLOC
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j$(nproc) Image.gz modules

Finally build `wlan.ko` out-of-tree against that kernel:

    cd drivers/staging/qcacld-3.0
    make KERNEL_SRC=/path/to/erhai/linux \
         ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
         KBUILD_EXTRA="WLAN_PROFILE=sun_gki_peach" -j$(nproc)

`sun_gki_peach` is the right profile for this device: `sun` is the SoC and
`peach` is the WCN7880. It must be passed through `KBUILD_EXTRA`; passing
`WLAN_PROFILE=` directly to the outer make does not reach the inner one, and
the build then fails looking for `qdf_types.h`.

The kernel has to be built before `wlan.ko`, because `wlan.ko` resolves around
seventy symbols out of `cnss2.ko`.

## Installing on the device

`wlan.ko` is ~100 MB with debug symbols; strip it on install.

    # from the kernel tree, for the cnss modules
    make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- \
         INSTALL_MOD_PATH=/tmp/stage modules_install

    # wlan.ko
    make -C /path/to/erhai/linux M=$PWD INSTALL_MOD_STRIP=1 \
         INSTALL_MOD_PATH=/tmp/stage modules_install

Copy `/tmp/stage/lib/modules/<kver>/` to the device, then `depmod -a <kver>`.

Load order is `cnss2` first (it pulls the rest in through its dependencies),
then `wlan`. Blacklist `ath12k` — both drivers match the same PCI ID and
whichever binds first wins.

Firmware lives in the [firmware repository](../oneplus-erhai-firmware). One
file is easy to miss: cnss asks for `qdss_trace_config_v2.cfg` and
`qdss_trace_config_perf_v2.cfg` by bare name from the firmware base path, but
the vendor drop ships them only under `.../erhai/image/peach/`. If they are not
found, cnss skips the QDSS download without complaining and the firmware never
signals init-done, so bring-up hangs. Copy them to the base path.

## Licensing

The Qualcomm sources here are GPL-2.0-only, as marked in the files themselves.

## Vendoring / provenance

Until 2026-10-19 this directory was a **separate git repository** (branch
`master`, HEAD `f0b37cec` — *"cnss2: select WIRELESS_EXT, which qcacld needs and
nothing else provides"*) that the parent repository referenced only as a
**gitlink without a `.gitmodules` entry**.  A clone of `oneplus-erhai-ubuntu`
therefore got an EMPTY `oneplus-erhai-wcn/` directory and the whole WiFi driver
stack was missing from the mirror; the tree is vendored here as plain files now
(3117 files, build output excluded by the `.gitignore` files below it).  The
separate repository's history is not carried along — this README, `patches/`,
`install.sh` and the source tree are the record.

