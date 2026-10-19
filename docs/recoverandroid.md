# Delete Linux, Recover Android

## Full restore: give the whole disk back to Android

1. Connect PC via USB-cable.
2. Get tablet in **recovery mode**.
3. Run the partitioning script:
```bash
adb push erhai-setup_partitions.sh /tmp
adb shell "chmod +x /tmp/erhai-setup_partitions.sh && /tmp/erhai-setup_partitions.sh"
```
4. Choose **Dry-run=No** and then **2) Restore the Android layout**. It deletes `esp`, `linux` and `data` and extends `userdata` back to the end of the disk.
5. **Wipe → Format Data → `yes`** in TWRP. This is what actually makes `userdata` mountable again — the script only fixes the table.
6. Reboot to **fastboot** mode.
7. Put the stock boot image back:

	From your PC (whichever slot is active):
	```bash
	fastboot flash boot_a erhai-boot_a-stock.img     # or boot_b
	fastboot reboot
	```
	
	<details>
	<summary> No backup of the boot image?</summary>

	> ⚠️  Beware of **Anti-Rollback** protection. See [here](https://danielspringer.at/index.php?view=arb). Only flash matching boot loader! You might brick your device!!!
	
	* Find OxygenOS ROMS/files for the Pad 3 [here](https://danielspringer.at/index.php?dir=Oneplus+Pad+3%2FSuper+Flashers#browser).
	* Choose region.
	* Browse ZIP.
	* Download boot.img and flash it to tablet as described above.

	If you cannot find a suitable boot.img then ask in forums or telegram.

	</details>

---

Back to [README](../README.md)
