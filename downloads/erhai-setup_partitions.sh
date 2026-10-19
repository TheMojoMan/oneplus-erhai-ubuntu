#!/sbin/sh
#
# setup_erhai_partitions.sh — partition the OnePlus Pad 3 (erhai) for Linux,
# and put the disk back the way Android wants it.
#
# Run it from TWRP (Advanced -> Terminal, or `adb shell`):
#
#     adb push setup_erhai_partitions.sh /tmp
#     adb shell "chmod +x /tmp/setup_erhai_partitions.sh && /tmp/setup_erhai_partitions.sh"
#
# It only touches the GPT. It writes no file systems and installs no files —
# erhai-ubuntu-installer.zip does that afterwards.
#
# What it can do:
#   1) Partition for Ubuntu — shrink, delete, or leave Android's `userdata`
#      alone, and create `esp` + `linux` (+ optional `data`) in the free space.
#      Existing `esp`/`linux`/`data` partitions can be deleted or kept one by
#      one, so re-partitioning works too (e.g. recreate esp + linux while
#      keeping data).
#      `esp` is created at a FIXED size — no size is asked for (see
#      ESP_SIZE_MB at the top of the file, including the minimum this hardware
#      allows for it).
#   2) Restore the Android layout — delete `esp`/`linux`/`data` and give every
#      byte back to `userdata`.
#
# NOTE: the sgdisk shipped by the SM87XX TWRP build accepts LONG options only
#       (--print, --info=N, --move-second-header, ...). Short options like -p
#       fail with "invalid option", so this script uses long options throughout.
#
# Dev note: run with --dev to also manage 'linux1' (the developer install).
# Normal end-user runs never touch linux1. Partition numbers are dynamic:
# userdata keeps its number x (whichever slot it is on the tablet), esp gets
# x+1, linux x+2, linux1 x+3 (--dev only) and data the next free number —
# shifting up past every kept partition.
#
# Dry-run is the default. Nothing is written until you explicitly turn it off.
#
# by TheMojoMan
# Version 1.1   (1.1: `esp` is created at a fixed 300 MB — the ESP size prompt
#                is gone, see ESP_SIZE_MB)

DRY_RUN=1
DISK=""
BYNAME=""
UD_NUM=""
UD_START=""

# Size of the 'esp' partition this script creates, in MB.  FIXED on purpose:
# the tool no longer asks (owner decision 2026-10-18), because the value is not
# cosmetic — it decides whether the volume can legally be a FAT32 at all.
#   * this disk has 4096-byte logical sectors (blockdev --getss), and mkfs.fat
#     never goes below a device's sector size, so the smallest possible cluster
#     here is 4096 B;
#   * a FAT32 needs >= 65525 clusters => ~256.0 MB of clusters + FAT overhead,
#     i.e. ~257 MB minimum.  Measured on erhai 2026-10-18 with the flags used
#     below (-F 32 -s 1): 256 MB -> 65376 clusters (TOO FEW), 268 MB -> 68442,
#     300 MB -> 76618, 512 MB -> 130784 (those three are valid, no fsck warning);
#   * the firmware we boot with (Mu-Silicium = EDK2) REJECTS a too-small
#     "FAT32" with EFI_VOLUME_CORRUPTED (FatPkg/EnhancedFatDxe/Init.c returns
#     that when MaxCluster < FAT_MAX_FAT16_CLUSTER = 0xFFF5 = 65525), so a
#     128/256 MB ESP would mean rEFInd is never loaded.  mkfs.fat itself only
#     warns — hence the explicit guard in the esp section below.
# 300 MB is the chosen size: the smallest round value that sits comfortably
# above that floor.  The ESP only holds rEFInd + theme + AndroidBootPkg
# (~5.5 MB); kernel UKIs live in the rootfs /boot, so there is no reason to go
# bigger.
ESP_SIZE_MB=300

# --------------------------------------------------------------- primitives --

say()  { echo "$@"; }
die()  { echo ""; echo "!! $1"; echo "!! ABORTED."; exit 1; }

confirm() {
	# confirm "question" -> 0 on y/Y
	printf "%s [y/n, default N]: " "$1"
	read _a
	[ "$_a" = "y" ] || [ "$_a" = "Y" ]
}

refresh_disk() {
	[ "$DRY_RUN" = 1 ] && return 0
	sync
	blockdev --rereadpt "$DISK" >/dev/null 2>&1
	partprobe "$DISK" >/dev/null 2>&1
	mdev -s >/dev/null 2>&1
	sleep 2
}

run_cmd() {
	if [ "$DRY_RUN" = 1 ]; then
		echo "   [DRY-RUN] $*"
	else
		echo "   + $*"
		"$@" || die "command failed: $*"
		refresh_disk
	fi
}

need_tool() {
	command -v "$1" >/dev/null 2>&1 && return 0
	# allow a static binary next to the script (e.g. the bundled parted)
	if [ -x "$(dirname "$0")/$1" ]; then
		PATH="$(dirname "$0"):$PATH"
		export PATH
		return 0
	fi
	return 1
}

# ------------------------------------------------------------------ discover --

find_disk() {
	for d in /dev/block/by-name /dev/block/bootdevice/by-name \
	         /dev/block/platform/soc/*/by-name; do
		[ -d "$d" ] && BYNAME="$d" && break
	done
	[ -n "$BYNAME" ] || die "No by-name partition directory found. Are you in TWRP?"

	[ -e "$BYNAME/userdata" ] || die "No 'userdata' partition in $BYNAME."
	UD=$(readlink -f "$BYNAME/userdata")

	# /dev/block/sda23 -> sda, 23 ;  /dev/block/mmcblk0p23 -> mmcblk0, 23
	UD_NUM=$(echo "$UD" | sed -e 's/.*[^0-9]\([0-9][0-9]*\)$/\1/')
	BASE=$(echo "$UD" | sed -e 's/[0-9][0-9]*$//')
	case "$BASE" in
		*p) DISK=${BASE%p} ;;
		*)  DISK=$BASE ;;
	esac
	[ -b "$DISK" ] || die "Cannot work out the disk that holds userdata (guessed $DISK)."

	SECSZ=$(blockdev --getss "$DISK" 2>/dev/null)
	[ -n "$SECSZ" ] || SECSZ=4096
}

# The partition table is snapshotted ONCE (load_table, called from main right
# after the disk is found). All planning works from these initial findings —
# after writing partitions the kernel/by-name view may lag or not refresh at
# all, so nothing re-reads the table mid-run. show_layout() is the only live
# view (for the human).
TABLE=""
load_table() {
	TABLE=$(sgdisk --print "$DISK" 2>/dev/null)
}

table_rows() {
	printf '%s\n' "$TABLE" | awk '
		$1 ~ /^[0-9]+$/ && $2 ~ /^[0-9]+$/ && $3 ~ /^[0-9]+$/ { print $1, $2, $3 }'
}

part_num_by_name() {
	# part_num_by_name esp -> partition number, empty if absent
	printf '%s\n' "$TABLE" | awk -v n="$1" '
		$1 ~ /^[0-9]+$/ && tolower($NF) == tolower(n) { print $1; exit }'
}

part_first_sector() {
	table_rows | awk -v p="$1" '$1+0 == p+0 { print $2; exit }'
}

part_last_sector() {
	table_rows | awk -v p="$1" '$1+0 == p+0 { print $3; exit }'
}

last_usable_sector() {
	printf '%s\n' "$TABLE" | awk '/last usable sector/ { print $NF; exit }'
}

first_usable_sector() {
	printf '%s\n' "$TABLE" | awk '/First usable sector/ { print $5+0; exit }'
}

# name of partition $1 from the snapshot, or empty if it doesn't exist or its
# number is in the $2 deleted list (it will be gone when new partitions are
# created, so its number counts as free)
part_name_by_num_excl() {
	printf '%s\n' "$TABLE" | awk -v p="$1" -v del="$2" '
		BEGIN { nd = split(del, D, " "); for (i = 1; i <= nd; i++) skip[D[i]] = 1 }
		$1 ~ /^[0-9]+$/ && $1+0 == p+0 {
			if ($1 in skip) exit
			if ($7 != "") print $7
			exit
		}'
}

# how many partition entries the GPT holds (sgdisk: "Partition table holds
# up to N entries")
gpt_max_entries() {
	printf '%s\n' "$TABLE" | awk '/Partition table holds up to/ { print $6; exit }'
}

# next free partition number after the cursor $2. Numbers held by partitions
# the user is deleting ($1 list) count as free; every other number present in
# the snapshot counts as occupied, so kept partitions shift the slot up.
next_slot() {
	_max=$(gpt_max_entries)
	[ -n "$_max" ] || _max=128
	_n=$(($2 + 1))
	while :; do
		[ "$_n" -le "$_max" ] || die \
			"no free partition number left (GPT holds only $_max entries)."
		if [ -z "$(part_name_by_num_excl "$_n" "$1")" ]; then
			echo "$_n"
			return
		fi
		_n=$((_n + 1))
	done
}

# lowest partition number not used by any kept partition; numbers in $1
# (space-separated, e.g. the ones being deleted) count as free
lowest_free_num() {
	table_rows | awk -v del="$1" '
		BEGIN { nd = split(del, D, " "); for (i = 1; i <= nd; i++) free[D[i]] = 1; n = 1 }
		{ if ($1 in free) next; used[$1] = 1 }
		END { while (n in used) n++; print n }'
}

# ---- planning ----------------------------------------------------------------
# rows "num start end" of the snapshot are provided by table_rows(); the
# helpers below derive free space and slot numbers from them.

# first start sector of a kept partition after sector $3
# ($2 = space-separated list of partition numbers that are being deleted)
next_start_after() {
	table_rows | awk -v me="$1" -v del="$2" -v after="$3" '
		BEGIN { nd = split(del, D, " "); for (i = 1; i <= nd; i++) skip[D[i]] = 1; best = 0 }
		{
			if ($1 == me || ($1 in skip)) next
			s = $2 + 0
			if (s > after + 0 && (best == 0 || s < best)) best = s
		}
		END { print best }'
}

# largest free gap on the disk, given the planned deletions and the planned
# userdata fate. Prints "start end" of the gap (in sectors).
# args: 1=deleted list  2=userdata num  3=userdata action (keep|shrink|delete)
#       4=new userdata end sector (shrink)  5=first usable  6=last usable
plan_gap() {
	table_rows | sort -k2,2n | awk -v del="$1" -v ud="$2" -v act="$3" -v udend="$4" \
		-v first="$5" -v last="$6" '
		BEGIN {
			nd = split(del, D, " "); for (i = 1; i <= nd; i++) skip[D[i]] = 1
			cur = first + 0; best = 0; bs = 0; be = 0
		}
		{
			n = $1; s = $2 + 0; e = $3 + 0
			if (n == ud) {
				if (act == "delete") next
				if (act == "shrink") e = udend + 0
			}
			if (n in skip) next
			if (s - 1 >= cur) { g = s - cur; if (g > best) { best = g; bs = cur; be = s - 1 } }
			if (e + 1 > cur) cur = e + 1
		}
		END {
			g = last - cur + 1
			if (g > best) { best = g; bs = cur; be = last }
			if (best > 0) print bs, be
		}'
}

# 32-bit-safe size math: mksh in TWRP does 32-bit arithmetic, so any product
# involving bytes (sector * 4096, GB * 2^30) overflows — do it in awk (doubles)
sectors_for_gb() { awk -v g="$1" -v s="$SECSZ" 'BEGIN { printf "%d", g * 1073741824 / s }'; }
sectors_for_mb() { awk -v m="$1" -v s="$SECSZ" 'BEGIN { printf "%d", (m * 1048576 + s - 1) / s }'; }
gb_of_sectors()  { awk -v n="$1" -v s="$SECSZ" 'BEGIN { printf "%d", n * s / 1073741824 }'; }

# decimal-safe size input helpers ("48", "48.8", "48,8" are all valid)
clean_size() { printf '%s' "$1" | tr -d ' \t' | tr ',' '.'; }
is_size() {
	awk -v v="$1" 'BEGIN { exit !(v ~ /^[0-9]+(\.[0-9]+)?$/ && v + 0 > 0) }'
}
# decimal compare a <= b (shell -gt/-le breaks on "48.8")
num_le() {
	awk -v a="$1" -v b="$2" 'BEGIN { exit !(a + 0 <= b + 0) }'
}

show_layout() {
	say "========================================================================="
	say "Current disk layout:"
	say "--------------------"
	# only the table itself, from userdata to the last partition — no disk
	# info header, and the stock firmware partitions are not interesting here
	sgdisk --print "$DISK" 2>/dev/null | awk -v min="${UD_NUM:-1}" '
		$1 == "Number" && $2 == "Start" { header = 1; print; next }
		header && $1 ~ /^[0-9]+$/ && $1+0 < min+0 { next }
		header { print }
	'
	[ -n "$UD_NUM" ] && [ "$UD_NUM" -gt 1 ] 2>/dev/null && \
		say "   (stock firmware partitions 1..$((UD_NUM-1)) hidden)"
	say "========================================================================="
	say ""
}

# ------------------------------------------------------------------ actions --

partition_for_ubuntu() {
	say ""
	say "=== Partition disk for LINUX ============================================"

	# keep / delete existing Linux partitions, one by one — like the nabu
	# script, keeping one is fine: no new partition of that name is created.
	# 'linux1' is the developer install — only managed with --dev.
	CLEAN_LIST="esp linux data"
	[ "$DEV_MODE" = 1 ] && CLEAN_LIST="esp linux linux1 data"

	DELETED=""
	KEPT_ESP=0
	KEPT_LINUX=0
	KEPT_LINUX1=0
	KEPT_DATA=0

	for p in $CLEAN_LIST; do
		n=$(part_num_by_name "$p")
		if [ -n "$n" ]; then
			say ""
			say "A partition named '$p' already exists (number $n)."
			if confirm "Delete it"; then
				run_cmd sgdisk --delete="$n" "$DISK"
				DELETED="$DELETED $n"
			else
				say "   keeping '$p' (partition $n) — no new '$p' will be created."
				case "$p" in
				esp)    KEPT_ESP=1 ;;
				linux)  KEPT_LINUX=1 ;;
				linux1) KEPT_LINUX1=1 ;;
				data)   KEPT_DATA=1 ;;
				esac
			fi
		fi
	done

	refresh_disk

	# ---- userdata --------------------------------------------------------------
	UD_START=$(part_first_sector "$UD_NUM")
	UD_END=$(part_last_sector "$UD_NUM")
	[ -n "$UD_START" ] && [ -n "$UD_END" ] || die "Cannot read userdata geometry."
	UD_SIZE_GB=$(gb_of_sectors $((UD_END - UD_START + 1)))

	UD_ACTION="keep"
	WIPED_ANDROID=0
	UD_END_NEW=0

	say ""
	say "========================================================================="
	say "userdata is partition $UD_NUM, ${UD_SIZE_GB} GB, sectors $UD_START..$UD_END"
	say "(sector size ${SECSZ} bytes)"
	say ""
	say "Please choose:"
	say "=============="
	say "  1) Shrink userdata, keep Android         [dual boot]"
	say "  2) Delete userdata, remove Android       [Linux only]"
	say "  3) Don't touch userdata                  [re-partition / testing]"
	printf "Choice [1/2/3]: "
	read c

	case "$c" in
	1)
		# shrinking happens in place: userdata keeps its start sector, only
		# the end moves — the freed space simply becomes a gap between
		# userdata and the next partition, wherever that is

		# the new end must not run into the next kept partition
		NEXT=$(next_start_after "$UD_NUM" "$DELETED" "$UD_END")
		[ "$NEXT" -gt 0 ] || NEXT=$(last_usable_sector)
		[ "$NEXT" -gt 0 ] || NEXT=$((UD_END + 1))
		MAX_GB=$(gb_of_sectors $((NEXT - UD_START)))
		say ""
		printf "New userdata size in GB [default 64, max %s]: " "$MAX_GB"
		read nsz
		[ -z "$nsz" ] && nsz=64
		nsz=$(clean_size "$nsz")
		is_size "$nsz" || die "Not a size: $nsz"
		num_le "$nsz" "$MAX_GB" || die \
			"${nsz} GB does not fit — the next kept partition starts right after userdata (max ${MAX_GB} GB)."

		_USECT=$(sectors_for_gb "$nsz")
		UD_END_NEW=$(( UD_START + _USECT - 1 ))
		UD_END_NEW=$(( UD_END_NEW - UD_END_NEW % 8 ))

		# recreate userdata at the same start, with the new size
		say ""
		say ">> shrinking userdata to ${nsz} GB"
		run_cmd sgdisk --delete="$UD_NUM" "$DISK"
		run_cmd sgdisk --new="$UD_NUM:$UD_START:$UD_END_NEW" \
		                --change-name="$UD_NUM:userdata" \
		                --typecode="$UD_NUM:8300" "$DISK"
		UD_ACTION="shrink"
		;;
	2)
		say ""
		say ">> deleting userdata (Android is gone after this)"
		confirm "Really delete Android's userdata" || return 0
		run_cmd sgdisk --delete="$UD_NUM" "$DISK"
		DELETED="$DELETED $UD_NUM"
		UD_ACTION="delete"
		WIPED_ANDROID=1
		;;
	*)
		say ""
		say "   userdata left untouched."
		;;
	esac

	refresh_disk

	# ---- where can the new partitions go? --------------------------------------
	# largest free gap, counting the deletions (and a userdata shrink) as done,
	# so planning also works in dry-run
	FIRST=$(first_usable_sector)
	LAST=$(last_usable_sector)
	[ -n "$FIRST" ] && [ -n "$LAST" ] || die "Cannot read disk geometry."

	set -- $(plan_gap "$DELETED" "$UD_NUM" "$UD_ACTION" "$UD_END_NEW" "$FIRST" "$LAST")
	[ "$#" -eq 2 ] || die "No usable free space on $DISK — cannot create new partitions."
	GAP_START=$1
	GAP_END=$2
	# align the start the way sgdisk does (here: 8-sector / 32 KiB steps)
	_R=$(( GAP_START % 8 ))
	[ "$_R" -ne 0 ] && GAP_START=$(( GAP_START + 8 - _R ))
	GAP_GB=$(gb_of_sectors $((GAP_END - GAP_START + 1)))
	[ "$GAP_GB" -gt 0 ] || die \
		"No usable free space on $DISK — shrink or delete userdata, or delete existing esp/linux/data first."
	say ""
	say "Free space for new partitions: ${GAP_GB} GB (sectors $GAP_START..$GAP_END)"

	# dynamic slot allocation: userdata keeps its number, everything else
	# follows on the next free numbers (shifting up past kept partitions)
	CUR=$UD_NUM

	# ---- esp -----------------------------------------------------------------
	# Size is FIXED (ESP_SIZE_MB, no prompt) — and always validated, because a
	# "FAT32" that is too small is silently created by mkfs.fat but REFUSED by
	# the firmware's FAT driver (see ESP_SIZE_MB at the top).
	if [ "$KEPT_ESP" = 1 ]; then
		say ""
		say "   'esp' kept — skipping its creation."
	else
		say ""
		ESP_SECT=$(sectors_for_mb "$ESP_SIZE_MB")
		# minimum sectors for a legal FAT32 on this disk: 65525 clusters
		# (x 1 sector each) + reserved sectors + 2 copies of the 32-bit FAT
		ESP_MIN_SECT=$(awk -v s="$SECSZ" 'BEGIN {
			f = (65525 + 2) * 4; f = int((f + s - 1) / s)
			printf "%d", 65525 + 32 + 2 * f }')
		[ "$ESP_SECT" -ge "$ESP_MIN_SECT" ] || die \
			"ESP_SIZE_MB=$ESP_SIZE_MB cannot hold a FAT32 on this disk (${SECSZ} B sectors): a FAT32 needs at least 65525 clusters, i.e. $(( (ESP_MIN_SECT * SECSZ + 1048575) / 1048576 )) MB here (and the boot firmware will not mount a smaller one)."
		ESP_END=$(( GAP_START + ESP_SECT - 1 ))
		[ "$ESP_END" -le "$GAP_END" ] || die \
			"ESP (${ESP_SIZE_MB} MB) does not fit into the ${GAP_GB} GB of free space."
		ESP_N=$(next_slot "$DELETED" "$CUR")
		CUR=$ESP_N
		say ">> creating 'esp' as partition $ESP_N, sectors $GAP_START..$ESP_END (fixed ${ESP_SIZE_MB} MB)"
		run_cmd sgdisk --new="$ESP_N:$GAP_START:$ESP_END" \
		                --change-name="$ESP_N:esp" --typecode="$ESP_N:ef00" "$DISK"
		ESP_MADE=1
		GAP_START=$(( ESP_END + 1 ))
	fi

	# ---- linux ---------------------------------------------------------------
	if [ "$KEPT_LINUX" = 1 ]; then
		say ""
		say "   'linux' kept — skipping its creation."
	else
		say ""
		printf "Linux root size in GB, or 'all' for the rest [default 40]: "
		read lsz
		[ -z "$lsz" ] && lsz=40
		LIN_N=$(next_slot "$DELETED" "$CUR")
		CUR=$LIN_N
		if [ "$lsz" = "all" ]; then
			LIN_END=$GAP_END
			say ">> creating 'linux' as partition $LIN_N, sectors $GAP_START..$LIN_END (rest of the free space)"
		else
			lsz=$(clean_size "$lsz")
			is_size "$lsz" || die "Not a size."
			_LSECT=$(sectors_for_gb "$lsz")
			LIN_END=$(( GAP_START + _LSECT - 1 ))
			[ "$LIN_END" -le "$GAP_END" ] || die \
				"linux (${lsz} GB) does not fit — only $(gb_of_sectors $((GAP_END - GAP_START + 1))) GB of free space remain."
			say ">> creating 'linux' as partition $LIN_N, sectors $GAP_START..$LIN_END"
		fi
		run_cmd sgdisk --new="$LIN_N:$GAP_START:$LIN_END" \
		                --change-name="$LIN_N:linux" --typecode="$LIN_N:8300" "$DISK"
		GAP_START=$(( LIN_END + 1 ))
	fi

	# ---- linux1 (developer install, only with --dev) ---------------------------
	if [ "$DEV_MODE" = 1 ]; then
		if [ "$KEPT_LINUX1" = 1 ]; then
			say ""
			say "   'linux1' kept — skipping its creation."
		else
			say ""
			printf "linux1 size in GB, or 'all' for the rest [default 48]: "
			read l1sz
			[ -z "$l1sz" ] && l1sz=48
			L1_N=$(next_slot "$DELETED" "$CUR")
			CUR=$L1_N
			if [ "$l1sz" = "all" ]; then
				L1_END=$GAP_END
				say ">> creating 'linux1' as partition $L1_N, sectors $GAP_START..$L1_END (rest of the free space)"
			else
				l1sz=$(clean_size "$l1sz")
				is_size "$l1sz" || die "Not a size."
				_L1SECT=$(sectors_for_gb "$l1sz")
				L1_END=$(( GAP_START + _L1SECT - 1 ))
				[ "$L1_END" -le "$GAP_END" ] || die \
					"linux1 (${l1sz} GB) does not fit — only $(gb_of_sectors $((GAP_END - GAP_START + 1))) GB of free space remain."
				say ">> creating 'linux1' as partition $L1_N, sectors $GAP_START..$L1_END"
			fi
			run_cmd sgdisk --new="$L1_N:$GAP_START:$L1_END" \
			                --change-name="$L1_N:linux1" --typecode="$L1_N:8300" "$DISK"
			GAP_START=$(( L1_END + 1 ))
		fi
	fi

	# ---- data ----------------------------------------------------------------
	if [ "$KEPT_DATA" = 1 ]; then
		say ""
		say "   'data' kept — skipping its creation."
		say ""
	else
		GAP_LEFT_GB=$(gb_of_sectors $((GAP_END - GAP_START + 1)))
		if [ "$GAP_LEFT_GB" -le 0 ]; then
			say ""
			say "   no free space left for a separate 'data' partition."
			say ""
		elif confirm "Create a separate 'data' partition with the rest (${GAP_LEFT_GB} GB)"; then
			DAT_N=$(next_slot "$DELETED" "$CUR")
			CUR=$DAT_N
			say ">> creating 'data' as partition $DAT_N, sectors $GAP_START..$GAP_END"
			run_cmd sgdisk --new="$DAT_N:$GAP_START:$GAP_END" \
			                --change-name="$DAT_N:data" --typecode="$DAT_N:8300" "$DISK"
			DATA_MADE=1
		fi
	fi

	# keep the backup GPT where it belongs
	run_cmd sgdisk --move-second-header "$DISK"
	refresh_disk

	if [ "$DRY_RUN" = 1 ]; then
		say ""
		say "Dry run finished — nothing was written."
		return 0
	fi

	show_layout

	# ---- format the new ESP (the installer copies its EFI files onto it) -------
	# same flags/label the installer itself would use as a fallback — legal here
	# because the fixed 300 MB ESP gives ~77k clusters (a FAT32 needs >= 65525;
	# see ESP_SIZE_MB at the top)
	if [ "${ESP_MADE:-0}" = 1 ]; then
		if command -v mkfs.fat >/dev/null 2>&1; then
			ESP_PART="$BYNAME/esp"
			_i=0
			while [ ! -e "$ESP_PART" ] && [ "$_i" -lt 10 ]; do
				refresh_disk
				_i=$(( _i + 1 ))
			done
			if [ ! -e "$ESP_PART" ]; then
				say " !! device node for 'esp' did not appear — the installer will format it."
			else
				say ">> formatting 'esp' as FAT32 (label ERHAI_ESP)"
				if mkfs.fat -F 32 -I -s 1 -n ERHAI_ESP "$ESP_PART" >/dev/null 2>&1; then
					say "   esp formatted."
				else
					say "   !! mkfs.fat failed — the installer will format the ESP instead."
				fi
			fi
		else
			say " !! mkfs.fat not found — the installer will format the ESP instead."
		fi
	fi
	say " ('linux' is left unformatted — the installer dd's ubuntu.img onto it.)"

	if [ "${DATA_MADE:-0}" = 1 ] && command -v mkfs.ext4 >/dev/null 2>&1; then
		if confirm "Format the new 'data' partition as ext4 now"; then
			mkfs.ext4 -F -L data "$BYNAME/data" >/dev/null 2>&1 \
				&& say "   data formatted." || say "   !! mkfs.ext4 on data failed."
		fi
	fi

	say ""
	say "======================================================================="
	say " Partitioning done."
	say ""
	if [ "${WIPED_ANDROID:-0}" = 1 ]; then
		say " Android's userdata is GONE. Android will not boot until you restore"
		say " it — see 'Restoring Android' in the README (menu option 2 here plus"
		say " a Format Data in TWRP)."
	elif [ "$UD_ACTION" = "shrink" ]; then
		say " userdata was recreated, so its contents are GARBAGE now: Android"
		say " cannot mount or decrypt it and will NOT boot as-is."
		say ""
		say "   -> In TWRP: Wipe -> Format Data -> type 'yes'."
		say "      That rebuilds an empty, mountable userdata. Android boots again"
		say "      afterwards (as a factory-reset device)."
	else
		say " userdata was left untouched — Android boots as before."
	fi
	say ""
	say " Then flash erhai-ubuntu-installer.zip from TWRP."
	say "======================================================================="
}

restore_android() {
	say ""
	say "=== Restore the Android layout ========================================="
	say "Deletes 'esp', 'linux', 'linux1' and 'data' and gives the space back"
	say "to userdata."
	say "Everything in your Linux installation is lost."
	say ""
	confirm "Continue" || return 0

	RESTORE_DEL=""
	for p in data linux1 linux esp; do
		n=$(part_num_by_name "$p")
		if [ -n "$n" ]; then
			say ">> deleting '$p' (partition $n)"
			run_cmd sgdisk --delete="$n" "$DISK"
			RESTORE_DEL="$RESTORE_DEL $n"
		fi
	done
	refresh_disk

	n=$(part_num_by_name userdata)
	if [ -n "$n" ]; then
		UD_START=$(part_first_sector "$n")
		say ">> re-creating userdata from sector $UD_START to the end of the disk"
		run_cmd sgdisk --delete="$n" "$DISK"
		run_cmd sgdisk --new="$n:$UD_START:0" --change-name="$n:userdata" \
		                --typecode="$n:8300" "$DISK"
	else
		# userdata was deleted for a Linux-only install: put it back where the
		# first Linux partition used to start.
		say ""
		say "!! No userdata partition found."
		DFLT=$(lowest_free_num "$RESTORE_DEL")
		printf "Partition number to create userdata as [default %s]: " "$DFLT"
		read n
		[ -z "$n" ] && n=$DFLT
		n=$(echo "$n" | tr -dc '0-9')
		[ -n "$n" ] || die "Not a partition number."
		say ">> creating userdata as partition $n over all free space"
		run_cmd sgdisk --new="$n:0:0" --change-name="$n:userdata" \
		                --typecode="$n:8300" "$DISK"
	fi

	run_cmd sgdisk --move-second-header "$DISK"
	refresh_disk

	if [ "$DRY_RUN" = 1 ]; then
		say ""
		say "Dry run finished — nothing was written."
		return 0
	fi

	show_layout

	say ""
	say "======================================================================="
	say " The partition table is back to the Android layout."
	say ""
	say " Two things are still missing before Android boots:"
	say ""
	say "   1. TWRP: Wipe -> Format Data -> type 'yes'   (rebuilds userdata)"
	say "   2. Put the stock boot image back, from your PC:"
	say "        fastboot flash boot_a erhai-boot_a-stock.img     (or boot_b)"
	say ""
	say " If you have no backup of the boot image, reflash the stock firmware."
	say "======================================================================="
}

# --------------------------------------------------------------------- main --

main() {
	need_tool sgdisk || die "sgdisk not found. Use a TWRP build that ships it."

	find_disk
	# snapshot the table once — after writing partitions the kernel/by-name
	# view may lag, so everything plans from these initial findings
	load_table

	clear 2>/dev/null
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say ""
	say "======================================================================="
	say "        OnePlus Pad 3 (erhai) — Linux partitioning utility"
	say ""
	say "                          by TheMojoMan"
	say "======================================================================="
	say ""
	say " disk       : $DISK   (sector size ${SECSZ} B)"
	say " userdata   : partition $UD_NUM"
	say ""
	say " !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
	say " !! Android need to be reinstalled after size of userdata is changed !!"
	say " !!             Backup all yout data BEFORE proceeding               !!"
	say " !!                                                                  !! "
	say " !!         Press Ctrl + C at any time to cancel this setup          !!"
	say " !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!"
	say ""
	say "Please choose:"
	say " 1. Dry-run mode    -  simulates the setup, writes nothing yet"
	say " 2. Execution mode  -  alters the disk layout - DANGEROUS!"
	printf "Selection [1/2, default 1]: "
	read d
	if [ "$d" = "2" ]; then
		DRY_RUN=0
		say ""
		say " *** DRY RUN IS OFF. Changes will be written to $DISK. ***"
	else
		DRY_RUN=1
		say ""
		say " Dry-run is ON — commands are printed, not executed."
	fi

	show_layout

	say "  1) Partition disk for LINUX"
	say "  2) Restore ANDROID layout"
	say "  3) Quit"
	printf "Select [1/2/3]: "
	read m

	case "$m" in
		1) partition_for_ubuntu ;;
		2) restore_android ;;
		3) say ""; say "Bye."; exit 0 ;;
		*) show_layout ;;
	esac
}

# secret dev option: --dev also manages the 'linux1' developer partition
DEV_MODE=0
for _a in "$@"; do
	[ "$_a" = "--dev" ] && DEV_MODE=1
done
main
