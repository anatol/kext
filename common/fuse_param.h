/*
 * 'rebel' branch modifications:
 *     Copyright (C) 2010 Tuxera. All Rights Reserved.
 */

/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Amit Singh <singh@>
 */

#ifndef _FUSE_PARAM_H_
#define _FUSE_PARAM_H_

/* Compile-time tunables (M_FUSE4X*) */

#define M_FUSE4X_ENABLE_FIFOFS            0
#define M_FUSE4X_ENABLE_INTERRUPT         1
#define M_FUSE4X_ENABLE_SPECFS            0
#define M_FUSE4X_ENABLE_TSLOCKING         1
#define M_FUSE4X_ENABLE_UNSUPPORTED       1
#define M_FUSE4X_ENABLE_XATTR             1

#if M_FUSE4X_ENABLE_UNSUPPORTED
  #define M_FUSE4X_ENABLE_DSELECT         0
  #define M_FUSE4X_ENABLE_EXCHANGE        1
  #define M_FUSE4X_ENABLE_KQUEUE          1
  #define M_FUSE4X_ENABLE_KUNC            0
  #if __LP64__
    #define M_FUSE4X_ENABLE_INTERIM_FSNODE_LOCK 1
  #endif /* __LP64__ */
#endif /* M_FUSE4X_ENABLE_UNSUPPORTED */

#if M_FUSE4X_ENABLE_INTERIM_FSNODE_LOCK
  #define M_FUSE4X_ENABLE_HUGE_LOCK 0
  #define M_FUSE4X_ENABLE_LOCK_LOGGING 0
  #define FUSE_VNOP_EXPORT __private_extern__
#else
  #define FUSE_VNOP_EXPORT static
#endif /* M_FUSE4X_ENABLE_INTERIM_FSNODE_LOCK */

/* User Control */

#define FUSE4X_POSTUNMOUNT_SIGNAL         SIGKILL

#define MACOSX_ADMIN_GROUP_NAME            "admin"

#define SYSCTL_FUSE4X_TUNABLES_ADMIN      "fuse4x.tunables.admin_group"
#define SYSCTL_FUSE4X_VERSION_NUMBER      "fuse4x.version.number"

/* Paths */

#define FUSE4X_BUNDLE_PATH "/Library/Filesystems/fuse4x.fs"
#define FUSE4X_KEXT        FUSE4X_BUNDLE_PATH "/Support/fuse4x.kext"
// Temporary use binaries from PATH
//#define FUSE4X_MOUNT_PROG  FUSE4X_BUNDLE_PATH "/Support/mount_fusefs"
#define FUSE4X_MOUNT_PROG  "/opt/local/bin/mount_fusefs"

/* Device Interface */

/*
 * This is the prefix ("fuse" by default) of the name of a FUSE device node
 * in devfs. The suffix is the device number. "/dev/fuse0" is the first FUSE
 * device by default. If you change the prefix from the default to something
 * else, the user-space FUSE library will need to know about it too.
 */
#define FUSE4X_DEVICE_BASENAME            "fuse"

/*
 * This is the number of /dev/fuse<n> nodes we will create. <n> goes from
 * 0 to (FUSE_NDEVICES - 1).
 */
#define FUSE4X_NDEVICES                   24

/*
 * This is the default block size of the virtual storage devices that are
 * implicitly implemented by the FUSE kernel extension. This can be changed
 * on a per-mount basis (there's one such virtual device for each mount).
 */
#define FUSE_DEFAULT_BLOCKSIZE             4096

#define FUSE_MIN_BLOCKSIZE                 512
#define FUSE_MAX_BLOCKSIZE                 MAXPHYS

#ifndef MAX_UPL_TRANSFER
#define MAX_UPL_TRANSFER 256
#endif

/*
 * This is default I/O size used while accessing the virtual storage devices.
 * This can be changed on a per-mount basis.
 *
 * Nevertheless, the I/O size must be at least as big as the block size.
 */
#define FUSE_DEFAULT_IOSIZE                (16 * PAGE_SIZE)

#define FUSE_MIN_IOSIZE                    512
#define FUSE_MAX_IOSIZE                    (MAX_UPL_TRANSFER * PAGE_SIZE)

#define FUSE_DEFAULT_INIT_TIMEOUT                  10     /* s  */
#define FUSE_MIN_INIT_TIMEOUT                      1      /* s  */
#define FUSE_MAX_INIT_TIMEOUT                      300    /* s  */
#define FUSE_INIT_WAIT_INTERVAL                    100000 /* us */

#define FUSE_INIT_TIMEOUT_DEFAULT_BUTTON_TITLE     "OK"
#define FUSE_INIT_TIMEOUT_NOTICE_MESSAGE                                  \
  "Timed out waiting for the file system to initialize. The volume has "  \
  "been ejected. You can use the init_timeout mount option to wait longer."

#define FUSE_DEFAULT_DAEMON_TIMEOUT                60     /* s */
#define FUSE_MIN_DAEMON_TIMEOUT                    0      /* s */
#define FUSE_MAX_DAEMON_TIMEOUT                    600    /* s */

#define FUSE_DAEMON_TIMEOUT_DEFAULT_BUTTON_TITLE   "Keep Trying"
#define FUSE_DAEMON_TIMEOUT_OTHER_BUTTON_TITLE     "Force Eject"
#define FUSE_DAEMON_TIMEOUT_ALTERNATE_BUTTON_TITLE "Don't Warn Again"
#define FUSE_DAEMON_TIMEOUT_ALERT_MESSAGE                                 \
  "There was a timeout waiting for the file system to respond. You can "  \
  "eject this volume immediately, but unsaved changes may be lost."
#define FUSE_DAEMON_TIMEOUT_ALERT_TIMEOUT          120    /* s */

#ifdef KERNEL

/*
 * This is the soft upper limit on the number of "request tickets" FUSE's
 * user-kernel IPC layer can have for a given mount. This can be modified
 * through the fuse.* sysctl interface.
 */
#define FUSE_DEFAULT_MAX_FREE_TICKETS      1024
#define FUSE_DEFAULT_IOV_PERMANENT_BUFSIZE (1 << 19)
#define FUSE_DEFAULT_IOV_CREDIT            16

/* User-Kernel IPC Buffer */

#define FUSE_MIN_USERKERNEL_BUFSIZE        (128  * 1024)
#define FUSE_MAX_USERKERNEL_BUFSIZE        (16   * 1024 * 1024)

#define FUSE_REASONABLE_XATTRSIZE          FUSE_MIN_USERKERNEL_BUFSIZE

#endif /* KERNEL */

#define FUSE_DEFAULT_USERKERNEL_BUFSIZE    (16   * 1024 * 1024)

#define FUSE_LINK_MAX                      LINK_MAX
#define FUSE_UIO_BACKUP_MAX                8

#define FUSE_MAXNAMLEN                     255

#endif /* _FUSE_PARAM_H_ */
