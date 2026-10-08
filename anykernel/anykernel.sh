### AnyKernel3 Ramdisk Mod Script
## osm0sis @ xda-developers

### AnyKernel setup
# global properties
properties() { '
kernel.string=LazyExec by xd (iput-object)
do.devicecheck=1
do.modules=0
do.systemless=1
do.cleanup=1
do.cleanuponabort=0
device.name1=miatoll
device.name2=curtana
device.name3=excalibur
device.name4=gram
device.name5=joyeuse
supported.versions=
supported.patchlevels=
supported.vendorpatchlevels=
'; } # end properties

### AnyKernel install
# boot shell variables
block=/dev/block/bootdevice/by-name/boot;
is_slot_device=auto;
ramdisk_compression=auto;
patch_vbmeta_flag=auto;

# import functions/variables and setup patching - see for reference (DO NOT REMOVE)
. tools/ak3-core.sh;

## boot files attributes
boot_attributes() {
set_perm_recursive 0 0 755 644 $ramdisk/*;
set_perm_recursive 0 0 750 750 $ramdisk/init* $ramdisk/sbin;
} # end attributes

## AnyKernel boot install
split_boot;

flash_boot;
flash_dtbo;

## cache cleanup: everything here is rebuilt on the next boot
# /cache (recovery logs and commands are kept), dalvik-cache, and the
# framework's parsed package cache, so nothing from the previous kernel's
# boot is reused
ui_print " " "Cleaning cache, dalvik-cache and package cache...";
for f in /cache/* /cache/.[!.]*; do
  [ -e "$f" ] && [ "${f##*/}" != recovery ] && rm -rf "$f";
done;
rm -rf /data/dalvik-cache/* /data/system/package_cache/* 2>/dev/null;

ui_print " " "First boot may take a few minutes while caches are rebuilt.";
## end boot install
