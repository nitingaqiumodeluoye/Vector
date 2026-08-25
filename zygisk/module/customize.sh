SKIPUNZIP=1

# =========================================================
# Utils functions to extract and verify installation package

TMPDIR_FOR_VERIFY="/data/local/tmp/.vector-vunzip.$$"
rm -rf "$TMPDIR_FOR_VERIFY"
mkdir -p "$TMPDIR_FOR_VERIFY/files"
trap 'rm -rf "$TMPDIR_FOR_VERIFY"' EXIT

abort_verify() {
  ui_print "*********************************************************"
  ui_print "! $1"
  ui_print "! This zip may be corrupted, please try downloading again"
  abort    "*********************************************************"
}

# Usage: extract <zip> <file> <target dir> [junk_paths: true|false]
extract() {
  local zip="$1"
  local file="$2"
  local dir="$3"
  local junk_paths="${4:-false}" # Defaults to false if not provided
  local opts="-o"
  local file_path hash_path tmp_file_path file_basename

  file_basename=$(basename "$file")

  if [ "$junk_paths" = "true" ]; then
    opts="-oj"
    file_path="$dir/$file_basename"
    hash_path="${TMPDIR_FOR_VERIFY}/$file_basename.sha256"
    tmp_file_path="${TMPDIR_FOR_VERIFY}/files/$file_basename"
  else
    file_path="$dir/$file"
    hash_path="${TMPDIR_FOR_VERIFY}/$file.sha256"
    tmp_file_path="${TMPDIR_FOR_VERIFY}/files/$file"
  fi

  # Stream to a neutral temp location first. Some APatch install contexts have produced
  # same-sized zero-filled files when unzip writes directly into the module target.
  mkdir -p "$(dirname "$tmp_file_path")"
  rm -f "$tmp_file_path"
  if ! unzip -p "$zip" "$file" >"$tmp_file_path" 2>/dev/null; then
    rm -f "$tmp_file_path"
    abort_verify "Failed to extract $file"
  fi
  [ -s "$tmp_file_path" ] || abort_verify "Extracted $file is empty"

  mkdir -p "$(dirname "$hash_path")"
  rm -f "$hash_path"
  if ! unzip -p "$zip" "$file.sha256" >"$hash_path" 2>/dev/null; then
    rm -f "$hash_path"
    abort_verify "Failed to extract hash file $file.sha256"
  fi
  [ -s "$hash_path" ] || abort_verify "Hash file $file.sha256 is empty"

  # Read the expected hash and verify it
  local expected_hash
  read -r expected_hash < "$hash_path"
  expected_hash="${expected_hash%% *}" # Strip anything after the actual hash string
  if ! echo "$expected_hash" | grep -Eq '^[0-9a-fA-F]{64}$'; then
    abort_verify "Invalid hash for $file"
  fi

  if ! echo "$expected_hash  $tmp_file_path" | sha256sum -c -s -; then
    abort_verify "Failed to verify $file"
  fi

  mkdir -p "$(dirname "$file_path")"
  rm -f "$file_path"
  # APatch can return from installation before a normal cp-backed file is durable.  On the next
  # boot the file keeps its size but reads as zeroes, which makes module.prop and the Zygisk ELF
  # unusable.  Use fsync-capable dd for the final copy so the verified bytes reach storage before
  # the installer returns.
  if ! dd if="$tmp_file_path" of="$file_path" bs=65536 conv=fsync 2>/dev/null; then
    rm -f "$file_path"
    abort_verify "Failed to copy $file into module path"
  fi
  if ! echo "$expected_hash  $file_path" | sha256sum -c -s -; then
    abort_verify "Failed to verify installed $file"
  fi

  ui_print "- Verified $file"
}

# Extract a file only when this build contains the requested ABI.  arm64-only local builds are
# valid on 64-bit devices even though the installer reports both the 32-bit and 64-bit ABIs.
extract_if_present() {
  local zip="$1"
  local file="$2"
  local dir="$3"
  local junk_paths="${4:-false}"

  # BusyBox unzip may return success for `-p` even when no member matched.
  # Check the archive directory explicitly so an arm64-only package does not
  # fall through to extract() and abort on its intentionally missing 32-bit file.
  if unzip -l "$zip" "$file" 2>/dev/null | grep -Fv 'caution:' | grep -Fq "$file"; then
    extract "$zip" "$file" "$dir" "$junk_paths"
  else
    ui_print "- Skipped unavailable $file"
  fi
}
# =========================================================

VERSION=$(grep_prop version "${TMPDIR}/module.prop")
ui_print "- Vector version ${VERSION}"

# Disable existing LSPosed installation
LSPOSED_DIR="/data/adb/modules/zygisk_lsposed"
if [ -d "$LSPOSED_DIR" ]; then
    ui_print "*********************************************************"
    ui_print "LSPosed installation detected, disabling it for Vector"
    touch "$LSPOSED_DIR/disable"
    ui_print "*********************************************************"
fi

# 1. Map architecture to standard ABI paths, eliminating duplicate logic
case "$ARCH" in
    arm|arm64)
        ABI32="armeabi-v7a"
        ABI64="arm64-v8a"
        ;;
    x86|x64)
        ABI32="x86"
        ABI64="x86_64"
        ;;
    *)
        abort "! Unsupported platform: $ARCH"
        ;;
esac
ui_print "- Device platform: $ARCH ($ABI32 / $ABI64)"

ui_print "- Extracting root module files"
for file in module.prop action.sh service.sh uninstall.sh sepolicy.rule framework/vector.dex cli daemon.apk daemon manager.apk; do
    extract "$ZIPFILE" "$file" "$MODPATH"
done

# Remove artifacts from older Vector releases that shipped the dex2oat wrapper.
rm -rf "$MODPATH/bin"
rm -f "$MODPATH/system.prop"

ui_print "- Extracting Zygisk libraries"
mkdir -p "$MODPATH/zygisk"

# Extract 32-bit lib when present. A 64-bit-only package is valid on a 64-bit device.
if [ "$IS64BIT" = true ]; then
    extract_if_present "$ZIPFILE" "lib/$ABI32/libzygisk.so" "$MODPATH/zygisk" true
else
    extract "$ZIPFILE" "lib/$ABI32/libzygisk.so" "$MODPATH/zygisk" true
fi
[ -f "$MODPATH/zygisk/libzygisk.so" ] && mv "$MODPATH/zygisk/libzygisk.so" "$MODPATH/zygisk/${ABI32}.so"

# Extract 64-bit lib if supported
if [ "$IS64BIT" = true ]; then
    extract "$ZIPFILE" "lib/$ABI64/libzygisk.so" "$MODPATH/zygisk" true
    mv "$MODPATH/zygisk/libzygisk.so" "$MODPATH/zygisk/${ABI64}.so"
fi

DEV_PATH=$(tr -dc 'a-z0-9' </dev/urandom | head -c 32)
[ -f "$MODPATH/daemon.apk" ] && sed -i "s/5291374ceda0aef7c5d86cd2a4f6a3ac/$DEV_PATH/g" "$MODPATH/daemon.apk"

# APatch/NeoZygisk may relabel module files as system_file, which root app_process cannot always
# open as a classpath. Keep a readable mirror for the daemon and CLI; both wrappers prefer it.
if [ -f "$MODPATH/daemon.apk" ]; then
    rm -f /data/local/tmp/daemon.apk
    dd if="$MODPATH/daemon.apk" of=/data/local/tmp/daemon.apk bs=65536 conv=fsync 2>/dev/null ||
        unzip -op "$ZIPFILE" daemon.apk >/data/local/tmp/daemon.apk
    sed -i "s/5291374ceda0aef7c5d86cd2a4f6a3ac/$DEV_PATH/g" /data/local/tmp/daemon.apk 2>/dev/null || true
    chown shell shell /data/local/tmp/daemon.apk 2>/dev/null || true
    chmod 0644 /data/local/tmp/daemon.apk
    chcon u:object_r:shell_data_file:s0 /data/local/tmp/daemon.apk 2>/dev/null || true
fi

ui_print "- Setting permissions"
set_perm_recursive "$MODPATH" 0 0 0755 0644
[ -d "$MODPATH/bin" ] && set_perm_recursive "$MODPATH/bin" 0 2000 0755 0755 u:object_r:xposed_file:s0

set_perm "$MODPATH/action.sh" 0 0 0755
set_perm "$MODPATH/daemon" 0 0 0744
set_perm "$MODPATH/cli" 0 0 0744

# Make the module tree durable before APatch returns control to the caller.
sync

if [ "$(grep_prop ro.maple.enable)" = "1" ]; then
    ui_print "- Add ro.maple.enable=0"
    echo "ro.maple.enable=0" >>"$MODPATH/system.prop"
fi

ui_print "- Welcome to Vector!"
