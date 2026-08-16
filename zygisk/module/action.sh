MANAGER_PACKAGE_NAME="@MANAGER_PACKAGE_NAME@"
INJECTED_PACKAGE_NAME="@INJECTED_PACKAGE_NAME@"
MODDIR="${0%/*}"

if [ "$MODDIR" = "$0" ]; then
  MODDIR="/data/adb/modules/zygisk_vector"
fi

if pm path "$MANAGER_PACKAGE_NAME" >/dev/null 2>&1; then
  am start -n "${MANAGER_PACKAGE_NAME}/.ui.MainActivity" && exit 0
fi

if [ -f "$MODDIR/manager.apk" ]; then
  pm install -r "$MODDIR/manager.apk" >/dev/null 2>&1
  if pm path "$MANAGER_PACKAGE_NAME" >/dev/null 2>&1; then
    am start -n "${MANAGER_PACKAGE_NAME}/.ui.MainActivity" && exit 0
  fi
fi

am start -c "${MANAGER_PACKAGE_NAME}.LAUNCH_MANAGER" "${INJECTED_PACKAGE_NAME}/.BugreportWarningActivity"
