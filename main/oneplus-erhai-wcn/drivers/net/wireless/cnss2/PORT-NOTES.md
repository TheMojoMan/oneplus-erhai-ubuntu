# CNSS2 literal downstream port — provenance & deviation log

**Goal:** As-literal-as-possible port of the OnePlus/Qualcomm downstream CNSS2
platform driver to mainline 7.2, as a "last measure" test of whether the native
Android host bring-up completes WCN7880 cold-boot calibration (or asserts
identically at the L1ss/PHY-off `line#178`), settling the host-vs-firmware
question for the long-running WiFi-internal hunt.

## Source provenance (pristine)
- Repo: `github.com/OnePlusOSS/android_kernel_modules_and_devicetree_oneplus_sm8750`
- Branch: `oneplus/sm8750_b_16.0.0_pad_3` (HEAD `b61b35f`, QCOM WLAN tag
  `AU_TECHPACK_WLAN.LA.1.0.R1.00.00.00.000.075`)
- Path: `vendor/qcom/opensource/wlan/platform/{cnss2,cnss_utils,cnss_genl,cnss_prealloc,inc}`
- Local checkout: `/mnt/data/Develop/erhai/downstream-ref/wlan-platform-pad3`
- The driver `.c/.h` are copied **verbatim**. Build glue (Kconfig/Makefile)
  is written fresh (legitimate — not driver logic).

## Tree layout
- `drivers/net/wireless/cnss2/`         — main/bus/debug/pci/power/genl/qmi/coex/oplus_wifi
- `drivers/net/wireless/cnss_utils/`    — cnss_utils + WLFW/DMS/IMS QMI IDL + plat_ipc QMI + inc headers
- `drivers/net/wireless/cnss_genl/`     — cnss_nl
- `drivers/net/wireless/cnss_prealloc/` — cnss_prealloc
- `include/net/{cnss2,cnss_utils,cnss_prealloc,cnss_nl}.h` — public API headers

## Substrate strategy (why this can build against mainline)
The downstream source is multi-platform via CONFIG guards; we select its own
"generic/standard" path rather than rewriting logic:
- `CONFIG_PCI_MSM=n`      → `pci_platform.h` `#else` branch → mainline PCIe RC (drops `pci_qcom.o`, all `msm_pcie_*`)
- `CONFIG_MHI_BUS_MISC=n` → `#else` stubs in pci.c → mainline MHI bus only
- `CONFIG_ESOC`/`MSM_SUBSYSTEM_RESTART` off → esoc/subsys compiled out
- PIL/ramdump/smem/smcinvoke(TZ): 0 refs in the cnss2 core

## Deliberate deviations from pristine (kept minimal, listed for audit)
1. **Build glue**: fresh Kconfig/Makefile per module; `CNSS2` depends on plain
   `PCI` (downstream `PCI_MSM`) so the standard-PCI path is selected.
2. **oplus vendor headers stubbed** — `include/soc/oplus/system/{boot_mode,oplus_project}.h`
   (real ones live in the OnePlus *kernel* tree, different repo). `get_boot_mode()`
   returns `NORMAL`; version getters return 0. Peripheral to cal (NV-MAC quirks).

## Compat shims added (mainline has no equivalent header)
- `include/linux/of_gpio.h` — reimplements `of_get_named_gpio()` (removed ~v6.15)
  via descriptor get+put+`desc_to_gpio()`; also re-includes `<linux/gpio.h>`.
  **VERIFY on boot: SOL/WAKE/WLAN_EN gpios (load-bearing for cal).**
- `include/linux/qcom-iommu-util.h` — no-op SMMU fault-handler stubs (debug aid).
- `include/soc/oplus/system/{boot_mode,oplus_project}.h` — oplus vendor stubs
  (`get_boot_mode`=NORMAL, `get_eng_version`=RELEASE, version getters=0).

## API-drift fixes (6.6 → 7.2) — DONE
- `cnss_utils/cnss_plat_ipc_qmi.c`, `cnss2/debug.h`: `in_irq()` → `in_hardirq()`.
- public headers → `include/net/{cnss2,cnss_utils,cnss_prealloc,cnss_nl}.h`.
- `cnss2/qmi.c` (×4): `kernel_connect()` cast `(struct sockaddr *)` →
  `(struct sockaddr_unsized *)` (arg-2 type changed in 7.2).
- `cnss2/debug.h`: `#include <soc/oplus/system/oplus_project.h>` (transitive).

## STATUS (2026-07-03): CLEAN LINK ✅
All six modules build with `make ARCH=arm64 modules` (modpost EXIT=0):
`cnss2.ko` (depends: wlan_firmware_service, cnss_plat_ipc_qmi_svc, **mhi**,
cnss_prealloc, cnss_utils), `cnss_utils.ko`, `wlan_firmware_service.ko` (WLFW/
DMS/IMS QMI IDL), `cnss_plat_ipc_qmi_svc.ko`, `cnss_nl.ko`, `cnss_prealloc.ko`.
Uses the **mainline** MHI bus + PCIe RC. NOT yet boot-tested (Task #5).

### API-drift fixes applied (6.6 → 7.2)
- `debug.h`, `cnss_plat_ipc_qmi.c`: `in_irq()` → `in_hardirq()`.
- `qmi.c` (×4): `kernel_connect()` arg-2 cast → `struct sockaddr_unsized *`.
- `debug.h`: `#include <soc/oplus/system/oplus_project.h>` (transitive).
- `main.c`: `thermal_of_cooling_device_register()` gained a `u32 cdev_id` arg
  (pass 0).
- `pci.c` timers: `from_timer`→`timer_container_of`, `del_timer`→`timer_delete`,
  `del_timer_sync`→`timer_delete_sync`.
- `pci.c`: `mhi_device_get()` → `mhi_device_get_sync()` (only variant upstream).
- `pci.c`: `struct mhi_channel_config.auto_queue` removed (×24 — no mainline
  field; 3 were `true` DL channels that used MHI auto-queue → now host-queued.
  Irrelevant to cal; revisit for datapath).
- `pci.c` `__cnss_get_mhi_soc_info()`: parse SoC HW-version reg straight into
  `plat_priv->device_version` (mainline `mhi_controller` lacks the version
  fields).
- `pci.c`: `mhi_controller.tme_supported_image` assignment dropped (no mainline
  field; TME image not driven via mainline MHI). ⚠ **functional gap** — revisit
  if WCN7880 needs a TME segment during bring-up.
- `pci.c`: `cnss_pci_{of_reserved_mem_device_init,wake_gpio_init,wake_gpio_deinit}`
  re-gated from `!ARCH_QCOM` to `!PCI_MSM` and made non-static — on ARCH_QCOM +
  mainline PCIe RC these real GPIO/IRQ wake fallbacks are exactly what's needed
  (pci_qcom.c provides them only when PCI_MSM=y). **WAKE gpio is load-bearing
  for cal — verify on boot.**

### Compat shims (mainline lacks the header)
- `include/linux/of_gpio.h` (of_get_named_gpio reimpl — VERIFY gpios on boot),
  `include/linux/qcom-iommu-util.h` (no-op SMMU fault stubs),
  `include/linux/pinctrl/qcom-pinctrl.h` (msm_gpio_mpm_wake_set no-op),
  `include/soc/oplus/system/{boot_mode,oplus_project}.h`.
- `ipc_logging` (38 refs) needed NO action — all behind `CONFIG_IPC_LOGGING`
  guards (off).

### NEXT (Task #5): DT node for cnss2/mainline-PCIe + ath12k unbind + firmware
staging, then the decisive boot test — does native cnss2 cold-boot-cal complete
(QMI FW_READY) or assert identically at the L1ss/PHY-off `line#178`.

## STATUS (2026-07-03 PM): boot-tested — fw drives the FULL QMI preamble, then
## stalls at "Timeout waiting for FW ready indication" (fw-init-done never fires).
The literal cnss binds the endpoint, sets up MSI/MHI, powers the fw, and the fw
ACCEPTS the entire cold-boot-cal preamble: target-cap, BDF, regdb, M3 (and AUX).
It then goes silent — no fw_init_done → host times out ("Ignore FW ready timeout
for calibration mode"). Captures: `captures/cnss-native-port/cnss-cal-trace-*.txt`.
So the `line#178` PHY-off assert does NOT reproduce here — the fw dies EARLIER, at
init, before mode=CAL is ever sent.

### ⚠️ Course-correction (user, 2026-07-03): STOP single-field patching host_cap —
### go fully LITERAL, replicate missing *inputs/packages*, then prune.
I had drifted into drip-patching host_cap outputs (5 PEACH-specific deviations) to
chase the stall. That violates FULL-REPLAY-THEN-PRUNE. **All 5 reverted to the
literal Android bring-up** (2026-07-03); qmi.c/main.c now differ from pristine ONLY
by the 6.6→7.2 API-drift fixes. The reverted deviations + why each was wrong:
1. `fw_ready_enable=1` (ind_register) — literal FW_V2 uses fw_init_done only.
2. `ddr_range` hardcoded `0x18000000/0x98010000` — **the real input is the SMMU-S1
   fastmap** (qmi.c host_cap sends ddr_range only `if cnss_bus_is_smmu_s1_enabled &&
   cnss_bus_get_iova(...)`; values come from the live IOVA domain). We STUBBED
   SMMU-S1 (`cnss_pci_init_smmu`→0), so the hardcode was *lying to the fw* about
   where DMA buffers live while the default iommu domain places them elsewhere.
   ⇒ literal now sends NO ddr_range. **If the fw still stalls at init, port the
   SMMU-S1 fastmap (the genuinely-missing "package") — do NOT re-hardcode.**
3. suppress `host_build_type`/MLO — literal sends them; no reason to omit.
4. `feature_list=0x18` (drop AUX_UC_SUPPORT bit6) + 5. `cnss_is_aux_support_enabled
   →false` — these two go together and were the biggest mistake: they *disable the
   full Android AUX handshake*. The literal cnss, when the fw reports AUX_UC_SUPPORT,
   `dma_alloc_coherent`s `aux_mem`, copies `aux_ucode20.elf`, and sends
   `AUX_UC_INFO_REQ` with the DMA addr (qmi.c:1535, pci.c:5717). The ath12k
   "AUX bit wedges the fw" finding was on ath12k, which sets the bit but never does
   that handshake — NOT transferable. Testing the literal AUX flow IS the experiment.

### Ruled OUT this session (evidence, not theory):
- **Userspace CNSS daemon / plat_ipc QMI file plane is NOT load-bearing for cal.**
  BDF response gives `Host BDF config: HW_XPA:1 CalDB:0` → `cbc_file_download=0` →
  `cnss_cal_db_mem_update` returns early ("CAL DB file not required as per BDF"),
  so the `wait_for_completion(&daemon_connected)` + `CNSS_ASSERT` is never reached.
  And `CONFIG_CNSS2_DEBUG` is off → CNSS_ASSERT is WARN_ON, not BUG(). The missing
  Android userspace cnss-daemon cannot be our stall or our reboot.
- The earlier **reboot-on-poke** correlated with the `rmmod cnss2; modprobe` RELOAD
  (cnss teardown leans on stubbed subsys/ramdump paths). Test protocol changed to a
  SINGLE modprobe on a fresh boot (cnss2 blacklisted from auto-load to dodge the
  8.6s-load vs 16.5s-endpoint race) + one `echo 1 > fs_ready`. No rmmod.

### NEXT: deploy the literal `cnss2.ko` + boot + capture the FULL literal trace.
Does the literal Android AUX-on bring-up reach fw_init_done (→ mode=CAL → line#178),
or stall at init? If it stalls, the next missing package to replicate is SMMU-S1.
