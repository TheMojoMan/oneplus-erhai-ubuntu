# Installation Guide

This guide walks you through the download and setup process to dual-boot **Ubuntu** alongside (or instead of) **Android** using TWRP recovery.

> ⚠️ **BACKUP ALL YOUR DATA** — when `userdata` gets shrunk or wiped Android only comes back as a factory-reset device. You do this at your own risk: no bricked-device, warranty or data-loss responsibility is taken.

## Prerequisites

* A PC with [Android Platform Tools](https://developer.android.com/studio/releases/platform-tools) (adb/fastboot)
* A reliable USB-C data cable
* A **OnePlus Pad 3** with **XN242 display** and **unlocked bootloader**.

	<details>
	<summary>How to check if your OnePlus Pad 3 has a <b>XN242 display</b>?</summary>

	Tablet should be switched on and run Android.

	1. **Enable Developer Options:** Go to Settings > About device > Version > tap the Version number multiple times until you see a confirmation message.
	2. **Enable USB-Debugging:** Go to Settings > System & Update > Developer Options > turn on "USB-Debugging".
	3. Connect tablet to your PC via USB-cable. When prompted: Allow connection to your PC.
	4. Open a terminal on your PC and run this command:
	```bash
	adb shell "cat /odm/etc/PanelChaplin/panelchaplin | grep XN242 | head -n1"
	```
	It should print a line that starts with "XN242".

	</details>

	<details>
	<summary> How to unlock the bootloader?</summary>

	Tablet should be switched on and run Android.

	1. **Enable Developer Options:** Go to Settings > About device > Version > tap the Version number multiple times until you see a confirmation message.
	2. **Enable OEM Unlocking:** Go to Settings > System & Update > Developer Options > turn on OEM unlocking.
	3. **Boot into Fastboot Mode:** Power off your device > press and hold the **Volume Down + Power** keys simultaneously to boot into Fastboot mode.
	4. Connect tablet to your PC via USB-cable.
	5. Open a terminal on your PC and run this command:
	```bash
	fastboot flashing unlock
	```	
	6. Confirm on Device: Use the **Volume** keys on your device to select UNLOCK THE BOOTLOADER and confirm with the **Power** key.
	</details>

* At least 30 % battery charge
* Download these files to your PC → click the file name in the left column of the release:

| File | Description |
| :--- | :--- |
| [erhai-TWRP.img](../downloads/erhai-TWRP.img) | Recovery to access the disk, partition it and flash the installer <!-- TODO link --> |
| [erhai-setup_partitions.sh](../downloads/erhai-setup_partitions.sh) | Creates `esp` / `linux` / `data` partitions — and can put the disk back for Android only |
| [erhai-ubuntu_installer.zip](../downloads/erhai-ubuntu_installer.zip) | Everything in one flashable zip: Ubuntu rootfs, ESP/boot files, dual-boot bootloader patch |
| [erhai-bootpatch.zip](../downloads/erhai-bootpatch.zip) | **NOT NEEDED NOW!** Sideload when Ubuntu is already installed and whenever Android overwrites bootloader at update |


## 1. Partitioning

We need to shrink Android's `userdata` partition and create `esp` and `linux` partitions and optionally a `data` partition:

1. Put the tablet in **Fastboot Mode**: Long-press power button → choose **Reboot** option → keep pressing down **Volume Down** key until fastboot screen appears.
2. Connect the tablet with your PC via USB-cable.
3. Open a terminal on your PC and change into the downloads folder.
3. Flash the modified TWRP to the recovery partition:
```bash
fastboot flash recovery erhai-TWRP.img
```
4. Reboot into recovery:
```bash
fastboot reboot recovery
```
5. Press **Cancel** button when TWRP asks for password/pattern to decrypt Android userdata.
6. Push and run the partitioning script:
```bash
adb push erhai-setup_partitions.sh /tmp
adb shell "chmod +x /tmp/erhai-setup_partitions.sh && /tmp/erhai-setup_partitions.sh"
```
7. Inside the script:
* Keep **Dry-Run = Yes** for the first pass to get used to the procedure.
* Choose **1) Partition disk for Linux**.
> ⚠️ **Both options that follow erase all Android data** — resizing `userdata` breaks its encryption, Android will not boot until you format data, and it comes back factory-reset.
* **Dual-Boot:** *shrink* `userdata` to what Android should keep (e.g. 64 GB) — Android stays installed, but empty.
* **Linux only:** *delete* `userdata` — this removes Android completely.
* The `esp` partition is created automatically at a fixed **300 MB**
* Recommended sizes: `linux` **40 GB or more**, a separate `data` partition **is highly recommended** (survives a later Linux re-install).

After Dry-run:
> ⚠️ **This is the destructive step.** All of Android `userdata` is lost, and **Android will not boot** until you format data again (next step). **Do not run** if you do not have a backup of your Android data!
* Run `adb shell "/tmp/erhai-setup_partitions.sh"` again.
* Repeat above procedure with **Dry-Run = No**.

8. **Rebuild Android's data partition.** In TWRP: **Wipe** → **Format Data** → type `yes`. This is what makes Android bootable again.
9. Tap **Home** icon and then **Reboot** button → tap on your currently active slot → tap on **Reboot** → tablet reboots into Android setup → setup Android.


## 2. Install Ubuntu
1. Put the tablet in **Fastboot Mode**.
2. Flash the modified TWRP to the recovery partition:
```bash
fastboot flash recovery erhai-TWRP.img
```
3. Reboot into recovery:
```bash
fastboot reboot recovery
```
4. In TWRP: **Cancel** → **Advanced** → **ADB Sideload** → swipe, then on your PC:
```bash
adb sideload erhai-ubuntu_installer.zip
```
First time will throw an error -> **Cancel** → **Advanced** → **ADB Sideload** → swipe, then on your PC:
```bash
adb sideload erhai-ubuntu_installer.zip
```
Second time should be OK. Image is fully flashed at ~48% (don't ask me why not 100%).

> ⚠️ Your stock boot image is backed up on the tablet to `/tmp/erhai-boot_a-stock.img` (or `/tmp/erhai-boot_b-stock.img`). This is in RAM only and will be deleted when tablet is rebooted!!!

5. **!!! Save the backup of your stock boot image to your PC _NOW_ !!!**

(You will need it to boot Android if you ever decide to delete Linux completely)
```bash
adb pull /tmp/erhai-boot_a-stock.img (or: adb pull /tmp/erhai-boot_b-stock.img)
```

6. Only after backup of stock boot image: **Reboot** the tablet.

## 3. Boot process

Now, after every power-on or reboot **Mu-Silicium** loads the **rEFInd menu** which shows you options to boot **Android** or a specific **Linux kernel**. 

Use the **Volume Up** and **Volume Down** keys to choose your entry and load it by pressing the **Power button**.

**rEFInd** remembers the last choice and boots it after its timeout if you do not press a button.

> First boot might take a little longer than later ones (the rootfs grows to the partition size).

## 4. Post-Installation

Reboot after the initial GNOME setup. Then (after reboot) connect to the internet by tapping on GNOME's quick settings menu in right top corner → **Wi-Fi** → choose network and input password, then:

* **Updates:** Tap on terminal icon → type `sudo apt update && sudo apt upgrade` and press enter key.
* **Thermal profiles:** Tap the **Thermal** icon in quick settings and choose an option to limit the performance of the tablet so it does not heat up as strongly as in Android.
* **Charge limit:** charging stops near 80 % by default and switches to Bypass-charging — change with `echo <max_value> | sudo tee /sys/module/erhai_otg_glink/parameters/max_soc`. Replace <max_value> with your desired value e.g. 100 for 100%.
* **Language:** Tap **Settings** icon in quick settings → on left side: System → on right side: Region & Language → Manage Installed Languages → Install/Remove languages → mark language → hit Apply →  drag new language up to top → logout → login.

<details>
<summary> Suggestions for common apps</summary>

```bash
sudo apt install vlc blender gimp steam
sudo snap install snap-store localsend telegram-desktop
```

</details>

---

Back to [README](../README.md) · Next → [Status](status.md)
