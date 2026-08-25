MANAGER_PACKAGE_NAME="@MANAGER_PACKAGE_NAME@"
INJECTED_PACKAGE_NAME="@INJECTED_PACKAGE_NAME@"
MODDIR="${0%/*}"

if [ "$MODDIR" = "$0" ]; then
  MODDIR="/data/adb/modules/zygisk_vector"
fi

# The manager APK is a parasitic payload. Starting an installed copy directly creates a normal
# application process, which is injected as a regular target and never receives the manager Binder
# handoff. Route through the framework's launch category so the ActivityThread/system hooks create
# the manager inside the designated host process and Constants.setBinder() is called.
am start -c "${MANAGER_PACKAGE_NAME}.LAUNCH_MANAGER" "${INJECTED_PACKAGE_NAME}/.BugreportWarningActivity"
