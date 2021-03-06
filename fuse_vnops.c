/*
 * Copyright (C) 2006-2008 Google. All Rights Reserved.
 * Copyright (C) 2010 Tuxera. All Rights Reserved.
 * Copyright (C) 2011 Anatol Pomozov. All Rights Reserved.
 */

#include "fuse.h"
#include "fuse_file.h"
#include "fuse_internal.h"
#include "fuse_ipc.h"
#include "fuse_locking.h"
#include "fuse_node.h"
#include <fuse_param.h>
#include "fuse_sysctl.h"
#include "fuse_vnops.h"
#include "compat/tree.h"

#ifdef FUSE4X_ENABLE_BIGLOCK
#include "fuse_biglock_vnops.h"
#endif

#include <kern/assert.h>
#include <libkern/libkern.h>
#include <libkern/OSMalloc.h>
#include <libkern/locks.h>
#include <mach/mach_types.h>
#include <sys/dirent.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/kernel_types.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/proc.h>
#include <sys/stat.h>
#include <sys/ubc.h>
#include <sys/unistd.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/xattr.h>
#include <sys/buf.h>
#include <sys/namei.h>
#include <sys/mman.h>
#include <vfs/vfs_support.h>


#define COM_APPLE_ "com.apple."

/* xattr */
static __inline__
bool
fuse_skip_apple_xattr_mp(mount_t mp, const char *name)
{
    return name &&
        (fuse_get_mpdata(mp)->dataflags & FSESS_NO_APPLEXATTR) &&
        (bcmp(name, COM_APPLE_, sizeof(COM_APPLE_) - 1) == 0);
}


/*
    struct vnop_access_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        int                  a_action;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_access(struct vnop_access_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    int           action  = ap->a_action;
    vfs_context_t context = ap->a_context;

    struct fuse_data *data = fuse_get_mpdata(vnode_mount(vp));

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        if (vnode_isvroot(vp)) {
            return 0;
        } else {
            return ENXIO;
        }
    }

    if (!data->inited) {
        if (vnode_isvroot(vp)) {
            if (fuse_vfs_context_issuser(context) ||
               (fuse_match_cred(data->daemoncred,
                                vfs_context_ucred(context)) == 0)) {
                return 0;
            }
        }
        return EBADF;
    }

    if (vnode_islnk(vp)) {
        return 0;
    }

    return fuse_internal_access(vp, action, context);
}

/*
    struct vnop_blktooff_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        daddr64_t            a_lblkno;
        off_t               *a_offset;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_blktooff(struct vnop_blktooff_args *ap)
{
    vnode_t    vp        = ap->a_vp;
    daddr64_t  lblkno    = ap->a_lblkno;
    off_t     *offsetPtr = ap->a_offset;

    struct fuse_data *data;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    data = fuse_get_mpdata(vnode_mount(vp));

    *offsetPtr = lblkno * (off_t)(data->blocksize);

    return 0;
}

/*
    struct vnop_blockmap_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        off_t                a_foffset;
        size_t               a_size;
        daddr64_t           *a_bpn;
        size_t              *a_run;
        void                *a_poff;
        int                  a_flags;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_blockmap(struct vnop_blockmap_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    off_t         foffset = ap->a_foffset;
    size_t        size    = ap->a_size;
    daddr64_t    *bpnPtr  = ap->a_bpn;
    size_t       *runPtr  = ap->a_run;
    int          *poffPtr = (int *)ap->a_poff;

    /* Ignoring flags and context */

    struct fuse_vnode_data *fvdat;
    struct fuse_data       *data;

    off_t contiguousPhysicalBytes;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    if (vnode_isdir(vp)) {
        return ENOTSUP;
    }

    if (ap->a_bpn == NULL) {
        return 0;
    }

    fvdat = VTOFUD(vp);
    data = fuse_get_mpdata(vnode_mount(vp));

    /*
     * We could assert that:
     *
     * (foffset % data->blocksize) == 0
     * (foffset < fvdat->filesize)
     * (size % data->blocksize) == 0)
     */

    *bpnPtr = foffset / data->blocksize;

    contiguousPhysicalBytes = \
        fvdat->filesize - (off_t)(*bpnPtr * data->blocksize);

    /* contiguousPhysicalBytes cannot really be negative (could assert) */

    if (contiguousPhysicalBytes > (off_t)size) {
        contiguousPhysicalBytes = (off_t)size;
    }

    if (runPtr != NULL) {
        *runPtr = (size_t)contiguousPhysicalBytes;
    }

    if (poffPtr != NULL) {
        *poffPtr = 0;
    }

    return 0;
}

/*
    struct vnop_close_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        int                  a_fflag;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_close(struct vnop_close_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    int           fflag   = ap->a_fflag;
    vfs_context_t context = ap->a_context;

    int err   = 0;
    int isdir = (vnode_isdir(vp)) ? 1 : 0;

    fufh_type_t fufh_type;
    struct fuse_data *data;

    struct fuse_vnode_data *fvdat = VTOFUD(vp);
    struct fuse_filehandle *fufh  = NULL;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return 0;
    }

    /* vclean() calls VNOP_CLOSE with fflag set to IO_NDELAY. */
    if (fflag == IO_NDELAY) {
        return 0;
    }

    if (isdir) {
        fufh_type = FUFH_RDONLY;
    } else {
        fufh_type = fuse_filehandle_xlate_from_fflags(fflag);
    }

    fufh = &(fvdat->fufh[fufh_type]);

    if (!FUFH_IS_VALID(fufh)) {
        log("fuse4x: fufh invalid in close [type=%d oc=%d vtype=%d cf=%d]\n",
              fufh_type, fufh->open_count, vnode_vtype(vp), fflag);
        return 0;
    }

    if (isdir) {
        goto skipdir;
    }

    /*
     * Enforce sync-on-close unless explicitly told not to.
     *
     * We do this to maintain correct semantics in the not so common case when
     * you create a file with O_RDWR but without write permissions--you /are/
     * supposed to be able to write to such a file given the descriptor you
     * you got from open()/create(). Therefore, if we don't finish all our
     * writing before we close this precious writable descriptor, we might
     * be doomed.
     */
    if (vnode_hasdirtyblks(vp) && !fuse_isnosynconclose(vp)) {
        (void)cluster_push(vp, IO_SYNC | IO_CLOSE);
    }

    data = fuse_get_mpdata(vnode_mount(vp));
    if (fuse_implemented(data, FSESS_NOIMPLBIT(FLUSH))) {

        struct fuse_dispatcher  fdi;
        struct fuse_flush_in   *ffi;

        fuse_dispatcher_init(&fdi, sizeof(*ffi));
        fuse_dispatcher_make_vp(&fdi, FUSE_FLUSH, vp, context);

        ffi = fdi.indata;
        ffi->fh = fufh->fh_id;
        ffi->unused = 0;
        ffi->padding = 0;
        ffi->lock_owner = 0;

        err = fuse_dispatcher_wait_answer(&fdi);

        if (!err) {
            fuse_ticket_drop(fdi.ticket);
        } else {
            if (err == ENOSYS) {
                fuse_clear_implemented(data, FSESS_NOIMPLBIT(FLUSH));
                err = 0;
            }
        }
    }

skipdir:

    /* This must be done after we have flushed any pending I/O. */
    FUFH_USE_DEC(fufh);

    if (!FUFH_IS_VALID(fufh)) {
        (void)fuse_filehandle_put(vp, context, fufh_type);
    }

    return err;
}

/*
    struct vnop_create_args {
        struct vnodeop_desc  *a_desc;
        vnode_t               a_dvp;
        vnode_t              *a_vpp;
        struct componentname *a_cnp;
        struct vnode_attr    *a_vap;
        vfs_context_t         a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_create(struct vnop_create_args *ap)
{
    vnode_t               dvp     = ap->a_dvp;
    vnode_t              *vpp     = ap->a_vpp;
    struct componentname *cnp     = ap->a_cnp;
    struct vnode_attr    *vap     = ap->a_vap;
    vfs_context_t         context = ap->a_context;

    struct fuse_create_in  *fci;
    struct fuse_mknod_in    fmni;
    struct fuse_entry_out  *feo;
    struct fuse_dispatcher  fdi;
    struct fuse_dispatcher *dispatcher = &fdi;

    int err;
    bool gone_good_old = false;

    struct fuse_data *data = NULL;

    mount_t mp = vnode_mount(dvp);
    uint64_t parent_nodeid = VTOFUD(dvp)->nodeid;
    mode_t mode = MAKEIMODE(vap->va_type, vap->va_mode);

    fuse_trace_printf_vnop_novp();

    if (fuse_isdeadfs(dvp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(dvp, context, EPERM);

    if (fuse_skip_apple_double_mp(mp, cnp->cn_nameptr, cnp->cn_namelen)) {
        return EPERM;
    }

    bzero(&fdi, sizeof(fdi));

    data = fuse_get_mpdata(mp);

    if (!fuse_implemented(data, FSESS_NOIMPLBIT(CREATE)) ||
        (vap->va_type != VREG)) {

        /* User-space file system does not implement CREATE */

        goto good_old;
    }

    fuse_dispatcher_init(dispatcher, sizeof(*fci) + cnp->cn_namelen + 1);
    fuse_dispatcher_make(dispatcher, FUSE_CREATE, mp, parent_nodeid, context);

    fci = dispatcher->indata;
    fci->mode = mode;

    /* XXX: We /always/ creat() like this. Wish we were on Linux. */
    fci->flags = O_CREAT | O_RDWR;

    memcpy((char *)dispatcher->indata + sizeof(*fci), cnp->cn_nameptr,
           cnp->cn_namelen);
    ((char *)dispatcher->indata)[sizeof(*fci) + cnp->cn_namelen] = '\0';

    err = fuse_dispatcher_wait_answer(dispatcher);

    if (err == ENOSYS) {
        fuse_clear_implemented(data, FSESS_NOIMPLBIT(CREATE));
        dispatcher->ticket = NULL;
        goto good_old;
    } else if (err) {
        goto undo;
    }

    goto bringup;

good_old:
    gone_good_old = true;
    fmni.mode = mode; /* fvdat->flags; */
    fmni.rdev = 0;
    fuse_internal_newentry_makerequest(mp, parent_nodeid, cnp,
                                       FUSE_MKNOD, &fmni, sizeof(fmni),
                                       dispatcher, context);
    err = fuse_dispatcher_wait_answer(dispatcher);
    if (err) {
        goto undo;
    }

bringup:
    feo = dispatcher->answer;

    if ((err = fuse_internal_checkentry(feo, VREG))) { // VBLK/VCHR not allowed
        fuse_ticket_drop(dispatcher->ticket);
        goto undo;
    }

    err = FSNodeGetOrCreateFileVNodeByID(vpp, false, feo, mp, dvp, context, NULL /* oflags */);
    if (err) {
       if (gone_good_old) {
           fuse_internal_forget_send(mp, context, feo->nodeid, 1, dispatcher);
       } else {
           struct fuse_release_in *fri;
           uint64_t nodeid = feo->nodeid;
           uint64_t fh_id = ((struct fuse_open_out *)(feo + 1))->fh;

           fuse_dispatcher_init(dispatcher, sizeof(*fri));
           fuse_dispatcher_make(dispatcher, FUSE_RELEASE, mp, nodeid, context);
           fri = dispatcher->indata;
           fri->fh = fh_id;
           fri->flags = OFLAGS(mode);
           fuse_insert_callback(dispatcher->ticket, fuse_internal_forget_callback);
           fuse_insert_message(dispatcher->ticket);
       }
       return err;
    }

    dispatcher->answer = gone_good_old ? NULL : feo + 1;

    if (!gone_good_old) {
        struct fuse_open_out *foo = (struct fuse_open_out *)(feo + 1);
        struct fuse_vnode_data *fvdat = VTOFUD(*vpp);
        struct fuse_filehandle *fufh = &(fvdat->fufh[FUFH_RDWR]);

        fufh->fh_id = foo->fh;
        fufh->open_flags = foo->open_flags;

        /*
         * We're stashing this to be picked up by open. Meanwhile, we set
         * the use count to 1 because that's what it is. The use count will
         * later transfer to the slot that this handle ends up falling in.
         */
        fufh->open_count = 1;

        OSIncrementAtomic((SInt32 *)&fuse_fh_current);
    }

    cache_purge_negatives(dvp);

    fuse_ticket_drop(dispatcher->ticket);

    return 0;

undo:
    return err;
}

/*
    struct vnop_exchange_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_fvp;
        vnode_t              a_tvp;
        int                  a_options;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_exchange(struct vnop_exchange_args *ap)
{

#ifdef FUSE4X_ENABLE_EXCHANGE

    vnode_t       fvp     = ap->a_fvp;
    vnode_t       tvp     = ap->a_tvp;
    int           options = ap->a_options;
    vfs_context_t context = ap->a_context;

    const char *fname = NULL;
    const char *tname = NULL;
    size_t flen = 0;
    size_t tlen = 0;

    struct fuse_data *data = fuse_get_mpdata(vnode_mount(fvp));

    int err = 0;

    fuse_trace_printf_vnop_novp();

    if (vnode_mount(fvp) != vnode_mount(tvp)) {
        return EXDEV;
    }

    /* We now know f and t are on the same volume. */

    if (!fuse_implemented(data, FSESS_NOIMPLBIT(EXCHANGE))) {
        return ENOTSUP;
    }

    if (fuse_isnovncache(fvp)) {
        return ENOTSUP;
    }

    if (fvp == tvp) {
        return EINVAL;
    }

    if (!vnode_isreg(fvp) || !vnode_isreg(tvp)) {
        return EINVAL;
    }

    if (fuse_isdeadfs(fvp)) {
        return ENXIO;
    }

    fname = vnode_getname(fvp);
    if (!fname) {
        return EIO;
    }

    tname = vnode_getname(tvp);
    if (!tname) {
        vnode_putname(fname);
        return EIO;
    }

    flen = strlen(fname);
    tlen = strlen(tname);

    if ((flen > 2) && (*fname == '.') && (*(fname + 1) == '_')) {
        err = EINVAL;
        goto out;
    }

    if ((tlen > 2) && (*tname == '.') && (*(tname + 1) == '_')) {
        err = EINVAL;
        goto out;
    }

    err = fuse_internal_exchange(fvp, fname, flen, tvp, tname, tlen, options,
                                 context);

    if (err == ENOSYS) {
        fuse_clear_implemented(data, FSESS_NOIMPLBIT(EXCHANGE));
        err = ENOTSUP;
    }

out:

    vnode_putname(fname);
    vnode_putname(tname);

    return err;

#else /* !FUSE4X_ENABLE_EXCHANGE */

    (void)ap;

    return ENOTSUP;

#endif /* FUSE4X_ENABLE_EXCHANGE */

}

/*
 * Our vnop_fsync roughly corresponds to the FUSE_FSYNC method. The Linux
 * version of FUSE also has a FUSE_FLUSH method.
 *
 * On Linux, fsync() synchronizes a file's complete in-core state with that
 * on disk. The call is not supposed to return until the system has completed
 * that action or until an error is detected.
 *
 * Linux also has an fdatasync() call that is similar to fsync() but is not
 * required to update the metadata such as access time and modification time.
 */

/*
    struct vnop_fsync_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        int                  a_waitfor;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_fsync(struct vnop_fsync_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    int           waitfor = ap->a_waitfor;
    vfs_context_t context = ap->a_context;

    struct fuse_dispatcher  fdi;
    struct fuse_filehandle *fufh;
    struct fuse_vnode_data *fvdat = VTOFUD(vp);

    int type, err = 0, tmp_err = 0;
    (void)waitfor;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return 0;
    }

    cluster_push(vp, 0);

    /*
     * struct timeval tv;
     * int wait = (waitfor == MNT_WAIT)
     *
     * In another world, we could be doing something like:
     *
     * buf_flushdirtyblks(vp, wait, 0, (char *)"fuse_fsync");
     * microtime(&tv);
     * ...
     */

    /*
     * - UBC and vnode are in lock-step.
     * - Can call vnode_isinuse().
     * - Can call ubc_msync().
     */

    mount_t mp = vnode_mount(vp);

    if (!fuse_implemented(fuse_get_mpdata(mp), ((vnode_isdir(vp)) ?
                FSESS_NOIMPLBIT(FSYNCDIR) : FSESS_NOIMPLBIT(FSYNC)))) {
        err = ENOSYS;
        goto out;
    }

    fuse_dispatcher_init(&fdi, 0);
    for (type = 0; type < FUFH_MAXTYPE; type++) {
        fufh = &(fvdat->fufh[type]);
        if (FUFH_IS_VALID(fufh)) {
            tmp_err = fuse_internal_fsync(vp, context, fufh, &fdi);
            if (tmp_err) {
                err = tmp_err;
            }
        }
    }

out:
    if ((err == ENOSYS) && !fuse_isnosyncwrites_mp(mp)) {
        err = 0;
    }

    return err;
}

/*
    struct vnop_getattr_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        struct vnode_attr   *a_vap;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_getattr(struct vnop_getattr_args *ap)
{
    vnode_t            vp      = ap->a_vp;
    struct vnode_attr *vap     = ap->a_vap;
    vfs_context_t      context = ap->a_context;

    int err = 0;
    struct timespec uptsp;
    struct fuse_dispatcher fdi;
    struct fuse_data *data;

    data = fuse_get_mpdata(vnode_mount(vp));

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        if (vnode_isvroot(vp)) {
            goto fake;
        } else {
            return ENXIO;
        }
    }

    if (!vnode_isvroot(vp) || !fuse_vfs_context_issuser(context)) {
        CHECK_BLANKET_DENIAL(vp, context, ENOENT);
    }

    /* Note that we are not bailing out on a dead file system just yet. */

    /* look for cached attributes */
    nanouptime(&uptsp);
    if (fuse_timespec_cmp(&uptsp, &VTOFUD(vp)->attr_valid, <=)) {
        if (vap != VTOVA(vp)) {
            fuse_internal_attr_loadvap(vp, vap, context);
        }
        return 0;
    }

    if (!data->inited) {
        if (!vnode_isvroot(vp)) {
            fuse_data_kill(data);
            return ENOTCONN;
        } else {
            goto fake;
        }
    }

    fuse_dispatcher_init(&fdi, sizeof(struct fuse_getattr_in));
    fuse_dispatcher_make_vp(&fdi, FUSE_GETATTR, vp, context);
    bzero(fdi.indata, sizeof(struct fuse_getattr_in));

    if ((err = fuse_dispatcher_wait_answer(&fdi))) {
        if ((err == ENOTCONN) && vnode_isvroot(vp)) {
            /* see comment at similar place in fuse_statfs() */
            goto fake;
        }
        if (err == ENOENT) {
#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_unlock(data->biglock);
#endif
            fuse_vncache_purge(vp); 
#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_lock(data->biglock);
#endif
        }
        return err;
    }

    struct fuse_attr_out *attr_out = fdi.answer;
    /* XXX: Could check the sanity/volatility of va_mode here. */

    if ((attr_out->attr.mode & S_IFMT) == 0) {
        return EIO;
    }

    cache_attrs(vp, attr_out);

    VTOFUD(vp)->c_flag &= ~C_XTIMES_VALID;

    fuse_internal_attr_loadvap(vp, vap, context);

    /* ATTR_FUDGE_CASE */
    if (vnode_isreg(vp) && fuse_isdirectio(vp)) {
        /*
         * This is for those cases when the file size changed without us
         * knowing, and we want to catch up.
         *
         * For the sake of sanity, we don't want to do it with UBC.
         * We also don't want to do it when we have asynchronous writes
         * enabled because we might have pending writes on *our* side.
         * We're not researching distributed file systems here!
         *
         */

        VTOFUD(vp)->filesize = attr_out->attr.size;
    }

    fuse_ticket_drop(fdi.ticket);

    if (vnode_vtype(vp) != vap->va_type) {
        if ((vnode_vtype(vp) == VNON) && (vap->va_type != VNON)) {
            /*
             * We should be doing the following:
             *
             * vp->vtype = vap->v_type
             */
        } else {

            /*
             * STALE vnode, ditch
             *
             * The vnode has changed its type "behind our back". There's
             * nothing really we can do, so let us just force an internal
             * revocation.
             */

#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_unlock(data->biglock);
#endif
           fuse_vncache_purge(vp); 
#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_lock(data->biglock);
#endif
            return EIO;
        }
    }

    return 0;

fake:
    bzero(vap, sizeof(*vap));
    VATTR_RETURN(vap, va_type, vnode_vtype(vp));
    VATTR_RETURN(vap, va_uid, kauth_cred_getuid(data->daemoncred));
    VATTR_RETURN(vap, va_gid, kauth_cred_getgid(data->daemoncred));
    VATTR_RETURN(vap, va_mode, S_IRWXU);

    return 0;
}

/*
    struct vnop_getxattr_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        char                *a_name;
        uio_t                a_uio;
        size_t              *a_size;
        int                  a_options;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_getxattr(struct vnop_getxattr_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    const char   *name    = ap->a_name;
    uio_t         uio     = ap->a_uio;
    vfs_context_t context = ap->a_context;

    struct fuse_dispatcher    fdi;
    struct fuse_getxattr_in  *fgxi;
    struct fuse_getxattr_out *fgxo;
    struct fuse_data         *data;
    mount_t mp;

    int err = 0;
    size_t namelen;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(vp, context, ENOENT);

    if (name == NULL || name[0] == '\0') {
        return EINVAL;
    }

    mp = vnode_mount(vp);
    data = fuse_get_mpdata(mp);

    if (fuse_skip_apple_xattr_mp(mp, name)) {
        return EPERM;
    }

    if (data->dataflags & FSESS_AUTO_XATTR) {
        return ENOTSUP;
    }

    if (!fuse_implemented(data, FSESS_NOIMPLBIT(GETXATTR))) {
        return ENOTSUP;
    }

    namelen = strlen(name);

    fuse_dispatcher_init(&fdi, sizeof(*fgxi) + namelen + 1);
    fuse_dispatcher_make_vp(&fdi, FUSE_GETXATTR, vp, context);
    fgxi = fdi.indata;

    if (uio) {
        fgxi->size = (uint32_t)uio_resid(uio);
    } else {
        fgxi->size = 0;
    }

    fgxi->position = (uint32_t)uio_offset(uio);

    memcpy((char *)fdi.indata + sizeof(*fgxi), name, namelen);
    ((char *)fdi.indata)[sizeof(*fgxi) + namelen] = '\0';

    if (fgxi->size > FUSE_REASONABLE_XATTRSIZE) {
        fdi.ticket->killed = true;
    }

    err = fuse_dispatcher_wait_answer(&fdi);
    if (err) {
        if (err == ENOSYS) {
            fuse_clear_implemented(data, FSESS_NOIMPLBIT(GETXATTR));
            return ENOTSUP;
        }
        return err;
    }

    if (uio) {
        *ap->a_size = fdi.iosize;
        if ((user_ssize_t)fdi.iosize > uio_resid(uio)) {
            err = ERANGE;
        } else {
            err = uiomove((char *)fdi.answer, (int)fdi.iosize, uio);
        }
    } else {
        fgxo = (struct fuse_getxattr_out *)fdi.answer;
        *ap->a_size = fgxo->size;
    }

    fuse_ticket_drop(fdi.ticket);

    return err;
}

/*
    struct vnop_inactive_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_inactive(struct vnop_inactive_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    vfs_context_t context = ap->a_context;

    struct fuse_vnode_data *fvdat = VTOFUD(vp);
    struct fuse_filehandle *fufh = NULL;

    fuse_trace_printf_vnop();

    /*
     * Cannot do early bail out on a dead file system in this case.
     */

    for (int fufh_type = 0; fufh_type < FUFH_MAXTYPE; fufh_type++) {

        fufh = &(fvdat->fufh[fufh_type]);
        //TOTHINK: should we just check that all fuse_fh are zero??

        if (FUFH_IS_VALID(fufh)) {
            FUFH_USE_RESET(fufh);
            (void)fuse_filehandle_put(vp, context, fufh_type);
        }
    }

    return 0;
}

/*
    struct vnop_link_args {
        struct vnodeop_desc  *a_desc;
        vnode_t               a_vp;
        vnode_t               a_tdvp;
        struct componentname *a_cnp;
        vfs_context_t         a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_link(struct vnop_link_args *ap)
{
    vnode_t               vp      = ap->a_vp;
    vnode_t               tdvp    = ap->a_tdvp;
    struct componentname *cnp     = ap->a_cnp;
    vfs_context_t         context = ap->a_context;

    struct vnode_attr *vap = VTOVA(vp);

    struct fuse_dispatcher fdi;
    struct fuse_entry_out *feo;
    struct fuse_link_in    fli;

    int err;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    if (vnode_mount(tdvp) != vnode_mount(vp)) {
        return EXDEV;
    }

    if (vap->va_nlink >= FUSE_LINK_MAX) {
        return EMLINK;
    }

    CHECK_BLANKET_DENIAL(vp, context, EPERM);

    fli.oldnodeid = VTOI(vp);

    fuse_dispatcher_init(&fdi, 0);
    fuse_internal_newentry_makerequest(vnode_mount(tdvp), VTOI(tdvp), cnp,
                                       FUSE_LINK, &fli, sizeof(fli), &fdi,
                                       context);
    if ((err = fuse_dispatcher_wait_answer(&fdi))) {
        return err;
    }

    feo = fdi.answer;

    err = fuse_internal_checkentry(feo, vnode_vtype(vp));
    fuse_ticket_drop(fdi.ticket);
    fuse_invalidate_attr(tdvp);
    fuse_invalidate_attr(vp);

    if (err == 0) {
        VTOFUD(vp)->nlookup++;
    }

    return err;
}

/*
    struct vnop_listxattr_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        uio_t                a_uio;
        size_t              *a_size;
        int                  a_options;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_listxattr(struct vnop_listxattr_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    uio_t         uio     = ap->a_uio;
    vfs_context_t context = ap->a_context;

    struct fuse_dispatcher    fdi;
    struct fuse_getxattr_in  *fgxi;
    struct fuse_getxattr_out *fgxo;
    struct fuse_data         *data;

    int err = 0;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(vp, context, ENOENT);

    data = fuse_get_mpdata(vnode_mount(vp));

    if (data->dataflags & FSESS_AUTO_XATTR) {
        return ENOTSUP;
    }

    if (!fuse_implemented(data, FSESS_NOIMPLBIT(LISTXATTR))) {
        return ENOTSUP;
    }

    fuse_dispatcher_init(&fdi, sizeof(*fgxi));
    fuse_dispatcher_make_vp(&fdi, FUSE_LISTXATTR, vp, context);
    fgxi = fdi.indata;
    if (uio) {
        fgxi->size = (uint32_t)uio_resid(uio);
    } else {
        fgxi->size = 0;
    }

    err = fuse_dispatcher_wait_answer(&fdi);
    if (err) {
        if (err == ENOSYS) {
            fuse_clear_implemented(data, FSESS_NOIMPLBIT(LISTXATTR));
            return ENOTSUP;
        }
        return err;
    }

    if (uio) {
        *ap->a_size = fdi.iosize;
        if ((user_ssize_t)fdi.iosize > uio_resid(uio)) {
            err = ERANGE;
        } else {
            err = uiomove((char *)fdi.answer, (int)fdi.iosize, uio);
        }
    } else {
        fgxo = (struct fuse_getxattr_out *)fdi.answer;
        *ap->a_size = fgxo->size;
    }

    fuse_ticket_drop(fdi.ticket);

    return err;
}

/*
    struct vnop_lookup_args {
        struct vnodeop_desc  *a_desc;
        vnode_t               a_dvp;
        vnode_t              *a_vpp;
        struct componentname *a_cnp;
        vfs_context_t         a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_lookup(struct vnop_lookup_args *ap)
{
    vnode_t dvp               = ap->a_dvp;
    vnode_t *vpp              = ap->a_vpp;
    struct componentname *cnp = ap->a_cnp;
    vfs_context_t context     = ap->a_context;

    int nameiop               = cnp->cn_nameiop;
    int flags                 = cnp->cn_flags;
    int wantparent            = flags & (LOCKPARENT|WANTPARENT);
    int islastcn              = flags & ISLASTCN;
    bool isdot                = false;
    bool isdotdot             = false;
    mount_t mp                = vnode_mount(dvp);

    int err                   = 0;
    int lookup_err            = 0;
    vnode_t vp                = NULL;
    vnode_t pdp               = (vnode_t)NULL;
    uint64_t size             = FUSE_ZERO_SIZE;

    struct fuse_dispatcher fdi;
    enum   fuse_opcode     op;

    uint64_t nodeid;
    uint64_t parent_nodeid;

    *vpp = NULLVP;

    fuse_trace_printf_vnop_novp();

    if (fuse_isdeadfs(dvp)) {
        return ENXIO;
    }

    if (fuse_skip_apple_double_mp(mp, cnp->cn_nameptr, cnp->cn_namelen)) {
        return ENOENT;
    }

    if (!vnode_isdir(dvp)) {
        return ENOTDIR;
    }

    if (islastcn && vfs_isrdonly(mp) && (nameiop != LOOKUP)) {
        return EROFS;
    }

    if (cnp->cn_namelen > FUSE_MAXNAMLEN) {
        return ENAMETOOLONG;
    }

    if (flags & ISDOTDOT) {
        isdotdot = true;
    } else if ((cnp->cn_nameptr[0] == '.') && (cnp->cn_namelen == 1)) {
        isdot = true;
    }

    if (isdotdot) {
        pdp = VTOFUD(dvp)->parentvp;
        nodeid = VTOI(pdp);
        parent_nodeid = VTOFUD(dvp)->parent_nodeid;
        fuse_dispatcher_init(&fdi, sizeof(struct fuse_getattr_in));
        op = FUSE_GETATTR;
        goto calldaemon;
    } else if (isdot) {
        nodeid = VTOI(dvp);
        parent_nodeid = VTOFUD(dvp)->parent_nodeid;
        fuse_dispatcher_init(&fdi, sizeof(struct fuse_getattr_in));
        op = FUSE_GETATTR;
        goto calldaemon;
    } else if (fuse_isnovncache_mp(mp)) {
        /* pretend it's a vncache miss */
        OSIncrementAtomic((SInt32 *)&fuse_lookup_cache_overrides);
        err = 0;
    } else {
#ifdef FUSE4X_ENABLE_BIGLOCK
        struct fuse_data *data = fuse_get_mpdata(mp);
        fuse_biglock_unlock(data->biglock);
#endif
        err = fuse_vncache_lookup(dvp, vpp, cnp);
#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_lock(data->biglock);
#endif
        switch (err) {

        case -1: /* positive match */
            OSIncrementAtomic((SInt32 *)&fuse_lookup_cache_hits);
            return 0;

        case 0: /* no match in cache (or aged out) */
            OSIncrementAtomic((SInt32 *)&fuse_lookup_cache_misses);
            break;

        case ENOENT: /* negative match */
            /* fall through */
        default:
            return err;
        }
    }

    nodeid = VTOI(dvp);
    parent_nodeid = VTOI(dvp);
    fuse_dispatcher_init(&fdi, cnp->cn_namelen + 1);
    op = FUSE_LOOKUP;

calldaemon:
    fuse_dispatcher_make(&fdi, op, mp, nodeid, context);

    if (op == FUSE_LOOKUP) {
        memcpy(fdi.indata, cnp->cn_nameptr, cnp->cn_namelen);
        ((char *)fdi.indata)[cnp->cn_namelen] = '\0';
    } else if (op == FUSE_GETATTR) {
        bzero(fdi.indata, sizeof(struct fuse_getattr_in));
    }

    lookup_err = fuse_dispatcher_wait_answer(&fdi);

    if ((op == FUSE_LOOKUP) && !lookup_err) { /* lookup call succeeded */
        nodeid = ((struct fuse_entry_out *)fdi.answer)->nodeid;
        size = ((struct fuse_entry_out *)fdi.answer)->attr.size;
        if (!nodeid) {
            fdi.answer_errno = ENOENT; /* XXX: negative_timeout case */
            lookup_err = ENOENT;
        } else if (nodeid == FUSE_ROOT_ID) {
            lookup_err = EINVAL;
        }
    }

    /*
     * If we get (lookup_err != 0), that means we didn't find what we were
     * looking for. This can still be OK if we're creating or renaming and
     * are at the end of the pathname.
     */

    if (lookup_err &&
        (!fdi.answer_errno || lookup_err != ENOENT || op != FUSE_LOOKUP)) {
        return lookup_err;
    }

    /* lookup_err, if non-zero, must be ENOENT at this point */

    if (lookup_err) {

        if ((nameiop == CREATE || nameiop == RENAME) && islastcn
            /* && directory dvp has not been removed */) {

            /*
             * EROFS case has already been covered.
             *
             * if (vfs_isrdonly(mp)) {
             *     err = EROFS;
             *     goto out;
             * }
             */

            err = EJUSTRETURN;
            goto out;
        }

        if ((cnp->cn_flags & MAKEENTRY) && (nameiop != CREATE)) {
            fuse_vncache_enter(dvp, NULLVP, cnp);
        }

        err = ENOENT;
        goto out;

    } else {

        /* !lookup_err */

        struct fuse_entry_out *feo   = NULL;
        struct fuse_attr      *fattr = NULL;

        if (op == FUSE_GETATTR) {
            fattr = &((struct fuse_attr_out *)fdi.answer)->attr;
        } else {
            feo = (struct fuse_entry_out *)fdi.answer;
            fattr = &(feo->attr);
        }

        /* Sanity check(s) */

        if ((fattr->mode & S_IFMT) == 0) {
            err = EIO;
            goto out;
        }

        if ((nameiop == DELETE) && islastcn) {

            if (isdot) {
                err = vnode_get(dvp);
                if (err == 0) {
                    *vpp = dvp;
                }
                goto out;
            }

            if ((err  = fuse_vget_i(&vp, feo, cnp, dvp,
                                    mp, context))) {
                goto out;
            }

            *vpp = vp;

            goto out;

        }

        if ((nameiop == RENAME) && islastcn && wantparent) {

            if (isdot) {
                err = EISDIR;
                goto out;
            }

            if ((err  = fuse_vget_i(&vp, feo, cnp, dvp,
                                    mp, context))) {
                goto out;
            }

            *vpp = vp;

            goto out;
        }

        if (isdotdot) {
            err = vnode_get(pdp);
            if (err == 0) {
                *vpp = pdp;
            }
        } else if (isdot) { /* nodeid == VTOI(dvp) */
            err = vnode_get(dvp);
            if (err == 0) {
                *vpp = dvp;
            }
        } else {
            if ((err  = fuse_vget_i(&vp, feo, cnp, dvp,
                                    mp, context))) {
                goto out;
            }
            *vpp = vp;
        }

        if (op == FUSE_GETATTR) {

            /* ATTR_FUDGE_CASE */
            if (vnode_isreg(*vpp) && fuse_isdirectio(vp)) {
                VTOFUD(*vpp)->filesize =
                    ((struct fuse_attr_out *)fdi.answer)->attr.size;
            }

            cache_attrs(*vpp, (struct fuse_attr_out *)fdi.answer);
        } else {

            /* ATTR_FUDGE_CASE */
            if (vnode_isreg(*vpp) && fuse_isdirectio(vp)) {
                VTOFUD(*vpp)->filesize =
                    ((struct fuse_entry_out *)fdi.answer)->attr.size;
            }

            cache_attrs(*vpp, (struct fuse_entry_out *)fdi.answer);
        }

        /*
         * We do this elsewhere...
         *
         * if (cnp->cn_flags & MAKEENTRY) {
         *     fuse_vncache_enter(dvp, *vpp, cnp);
         * }
         */
    }

out:
    if (!lookup_err) {

        /* No lookup error; need to clean up. */

        if (err) { /* Found inode; exit with no vnode. */
            if (op == FUSE_LOOKUP) {
                fuse_internal_forget_send(vnode_mount(dvp), context,
                                          nodeid, 1, &fdi);
            }
            return err;
        } else {

            if (!islastcn) {

                int tmpvtype = vnode_vtype(*vpp);

                if ((tmpvtype != VDIR) && (tmpvtype != VLNK)) {
                    err = ENOTDIR;
                }

                /* if (!err && !vnode_mountedhere(*vpp)) { ... */

                if (err) {
                    vnode_put(*vpp);
                    *vpp = NULL;
                }
            }
        }

        fuse_ticket_drop(fdi.ticket);
    }

    return err;
}

/*
    struct vnop_mkdir_args {
        struct vnodeop_desc  *a_desc;
        vnode_t               a_dvp;
        vnode_t              *a_vpp;
        struct componentname *a_cnp;
        struct vnode_attr    *a_vap;
        vfs_context_t         a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_mkdir(struct vnop_mkdir_args *ap)
{
    vnode_t               dvp     = ap->a_dvp;
    vnode_t              *vpp     = ap->a_vpp;
    struct componentname *cnp     = ap->a_cnp;
    struct vnode_attr    *vap     = ap->a_vap;
    vfs_context_t         context = ap->a_context;

    int err = 0;

    struct fuse_mkdir_in fmdi;

    fuse_trace_printf_vnop_novp();

    if (fuse_isdeadfs(dvp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(dvp, context, EPERM);

    fmdi.mode = MAKEIMODE(vap->va_type, vap->va_mode);

    err = fuse_internal_newentry(dvp, vpp, cnp, FUSE_MKDIR, &fmdi,
                                 sizeof(fmdi), VDIR, context);

    if (err == 0) {
        fuse_invalidate_attr(dvp);
    }

    return err;
}

/*
    struct vnop_mknod_args {
        struct vnodeop_desc  *a_desc;
        vnode_t               a_dvp;
        vnode_t              *a_vpp;
        struct componentname *a_cnp;
        struct vnode_attr    *a_vap;
        vfs_context_t         a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_mknod(struct vnop_mknod_args *ap)
{
    vnode_t               dvp     = ap->a_dvp;
    vnode_t              *vpp     = ap->a_vpp;
    struct componentname *cnp     = ap->a_cnp;
    struct vnode_attr    *vap     = ap->a_vap;
    vfs_context_t         context = ap->a_context;

    struct fuse_mknod_in fmni;

    int err;

    fuse_trace_printf_vnop_novp();

    if (fuse_isdeadfs(dvp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(dvp, context, EPERM);

    fmni.mode = MAKEIMODE(vap->va_type, vap->va_mode);
    fmni.rdev = vap->va_rdev;

    err = fuse_internal_newentry(dvp, vpp, cnp, FUSE_MKNOD, &fmni,
                                 sizeof(fmni), vap->va_type, context);

    if (err== 0) {
        fuse_invalidate_attr(dvp);
    }

    return err;
}

/*
    struct vnop_mmap_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        int                  a_fflags;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_mmap(struct vnop_mmap_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    int           fflags  = ap->a_fflags;
    vfs_context_t context = ap->a_context;

    struct fuse_vnode_data *fvdat = VTOFUD(vp);
    struct fuse_filehandle *fufh = NULL;

    int err = 0;
    int deleted = 0;
    int retried = 0;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    if (fuse_isdirectio(vp)) {
        /*
         * We should be returning ENODEV here, but ubc_map() translates
         * all errors except ENOPERM to 0. Even then, this is not going
         * to prevent the mmap()!
         */
        return EPERM;
    }

    CHECK_BLANKET_DENIAL(vp, context, ENOENT);

    if (fflags & (PROT_READ | PROT_WRITE | PROT_EXEC)) { /* nothing to do */
        return 0;
    }

    /* XXX: For PROT_WRITE, we should only care if file is mapped MAP_SHARED. */
    fufh_type_t fufh_type = fuse_filehandle_xlate_from_mmap(fflags);

retry:
    fufh = &(fvdat->fufh[fufh_type]);

    if (FUFH_IS_VALID(fufh)) {
        FUFH_USE_INC(fufh);
        OSIncrementAtomic((SInt32 *)&fuse_fh_reuse_count);
        goto out;
    }

    if (!deleted) {
#ifdef FUSE4X_ENABLE_BIGLOCK
        struct fuse_data *data = fuse_get_mpdata(vnode_mount(vp));
        fuse_biglock_unlock(data->biglock);
#endif
        err = fuse_filehandle_preflight_status(vp, fvdat->parentvp,
                                               context, fufh_type);
#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_lock(data->biglock);
#endif
        if (err == ENOENT) {
            deleted = 1;
            err = 0;
        }
    }

#ifdef FUSE4X_DEBUG
    fuse_preflight_log(vp, fufh_type, err, "mmap");
#endif /* FUSE4X_DEBUG */

    if (!err) {
        err = fuse_filehandle_get(vp, context, fufh_type, 0 /* mode */);
    }

    if (err) {
        /*
         * XXX: This is a kludge because xnu doesn't tell us whether this
         *      is a MAP_SHARED or MAP_PRIVATE mapping. If we want shared
         *      library mapping to go well, we need to do this.
         */
        if (!retried && (err == EACCES) &&
            ((fufh_type == FUFH_RDWR) || (fufh_type == FUFH_WRONLY))) {
            log("fuse4x: filehandle_get retrying (type=%d, err=%d)\n",
                  fufh_type, err);
            fufh_type = FUFH_RDONLY;
            retried = 1;
            goto retry;
        } else {
            log("fuse4x: filehandle_get failed in mmap (type=%d, err=%d)\n",
                  fufh_type, err);
        }
        return EPERM;
    }

out:

    return 0;
}

/*
    struct vnop_mnomap_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_mnomap(struct vnop_mnomap_args *ap)
{
    vnode_t vp = ap->a_vp;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return 0;
    }

    if (fuse_isdirectio(vp)) {
        /*
         * ubc_unmap() doesn't care about the return value.
         */
        return ENODEV;
    }

    /*
     * XXX
     *
     * What behavior do we want here?
     *
     * I once noted that sync() is not going to help here, but I think
     * I've forgotten the context. Need to think about this again.
     *
     * ubc_msync(vp, (off_t)0, ubc_getsize(vp), NULL, UBC_PUSHDIRTY);
     */

    /*
     * Earlier, we used to go through our vnode's fufh list here, doing
     * something like the following:
     *
     * for (type = 0; type < FUFH_MAXTYPE; type++) {
     *     fufh = &(fvdat->fufh[type]);
     *     if ((fufh->fufh_flags & FUFH_VALID) &&
     *         (fufh->fufh_flags & FUFH_MAPPED)) {
     *         fufh->fufh_flags &= ~FUFH_MAPPED;
     *         if (fufh->open_count == 0) {
     *             (void)fuse_filehandle_put(vp, context, type,
     *                                       wait_for_completion = false);
     *         }
     *     }
     * }
     *
     * Now, cleanup is all taken care of in vnop_inactive/reclaim.
     *
     */

    return 0;
}

/*
    struct vnop_offtoblk_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        off_t                a_offset;
        daddr64_t           *a_lblkno;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_offtoblk(struct vnop_offtoblk_args *ap)
{
    vnode_t    vp        = ap->a_vp;
    off_t      offset    = ap->a_offset;
    daddr64_t *lblknoPtr = ap->a_lblkno;

    struct fuse_data *data;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    data = fuse_get_mpdata(vnode_mount(vp));

    *lblknoPtr = offset / data->blocksize;

    return 0;
}

/*
    struct vnop_open_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        int                  a_mode;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_open(struct vnop_open_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    int           mode    = ap->a_mode;
    vfs_context_t context = ap->a_context;

    fufh_type_t             fufh_type;
    struct fuse_vnode_data *fvdat;
    struct fuse_filehandle *fufh = NULL;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    if (vnode_isfifo(vp)) {
        return EPERM;
    }

    CHECK_BLANKET_DENIAL(vp, context, ENOENT);

    fvdat = VTOFUD(vp);
    fufh_type = vnode_isdir(vp) ? FUFH_RDONLY : fuse_filehandle_xlate_from_fflags(mode);
    fufh = &(fvdat->fufh[fufh_type]);

    if (FUFH_IS_VALID(fufh)) {
        FUFH_USE_INC(fufh);
        OSIncrementAtomic((SInt32 *)&fuse_fh_reuse_count);
        goto ok; /* return 0 */
    }

    int error = fuse_filehandle_get(vp, context, fufh_type, mode);
    if (error) {
        log("fuse4x: filehandle_get failed in open (type=%d, err=%d)\n",
              fufh_type, error);
        if (error == ENOENT) {
            cache_purge(vp);
        }
        return error;
    }

ok:
    /*
     * Doing this here because when a vnode goes inactive, things like
     * no-cache and no-readahead are cleared by the kernel.
     */

    if ((fufh->fuse_open_flags & FOPEN_DIRECT_IO) || fuse_isdirectio(vp)) {
        /*
         * direct_io for a vnode implies:
         * - no ubc for the vnode
         * - no readahead for the vnode
         * - nosyncwrites disabled FOR THE ENTIRE MOUNT
         * - no vncache for the vnode (handled in lookup)
         */
        ubc_msync(vp, (off_t)0, ubc_getsize(vp), NULL,
                  UBC_PUSHALL | UBC_INVALIDATE);
        vnode_setnocache(vp);
        vnode_setnoreadahead(vp);
        fuse_clearnosyncwrites_mp(vnode_mount(vp));
        fvdat->flag |= FN_DIRECT_IO;
        goto out;
    } else if (fufh->fuse_open_flags & FOPEN_PURGE_UBC) {
        ubc_msync(vp, (off_t)0, ubc_getsize(vp), NULL,
                  UBC_PUSHALL | UBC_INVALIDATE);
        fufh->fuse_open_flags &= ~FOPEN_PURGE_UBC;
        if (fufh->fuse_open_flags & FOPEN_PURGE_ATTR) {
            struct fuse_dispatcher fdi;
            fuse_invalidate_attr(vp);

            fuse_dispatcher_init(&fdi, sizeof(struct fuse_getattr_in));
            fuse_dispatcher_make_vp(&fdi, FUSE_GETATTR, vp, context);
            bzero(fdi.indata, sizeof(struct fuse_getattr_in));

            int serr = fuse_dispatcher_wait_answer(&fdi);
            if (!serr) {
                /* XXX: Could check the sanity/volatility of va_mode here. */
                if ((((struct fuse_attr_out*)fdi.answer)->attr.mode & S_IFMT)) {
                    cache_attrs(vp, (struct fuse_attr_out *)fdi.answer);
                    off_t new_filesize =
                        ((struct fuse_attr_out *)fdi.answer)->attr.size;
                    VTOFUD(vp)->filesize = new_filesize;
                    ubc_setsize(vp, (off_t)new_filesize);
                }
                fuse_ticket_drop(fdi.ticket);
            }
            fufh->fuse_open_flags &= ~FOPEN_PURGE_ATTR;
        }
    }

    if (fuse_isnoreadahead(vp)) {
        vnode_setnoreadahead(vp);
    }

out:
    return 0;
}

/*
    struct vnop_pagein_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        upl_t                a_pl;
        vm_offset_t          a_pl_offset;
        off_t                a_f_offset;
        size_t               a_size;
        int                  a_flags;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_pagein(struct vnop_pagein_args *ap)
{
    vnode_t       vp        = ap->a_vp;
    upl_t         pl        = ap->a_pl;
    vm_offset_t   pl_offset = ap->a_pl_offset;
    off_t         f_offset  = ap->a_f_offset;
    size_t        size      = ap->a_size;
    int           flags     = ap->a_flags;

    struct fuse_vnode_data *fvdat;
    int err;

#ifdef FUSE4X_ENABLE_BIGLOCK
    struct fuse_data *data = fuse_get_mpdata(vnode_mount(vp));
#endif

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp) || fuse_isdirectio(vp)) {
        if (!(flags & UPL_NOCOMMIT)) {
            ubc_upl_abort_range(pl, (upl_offset_t)pl_offset, (int)size,
                                UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_ERROR);
        }
        /*
         * Will cause PAGER_ERROR (pager unable to read or write page).
         */
        return ENOTSUP;
    }

    fvdat = VTOFUD(vp);
    if (!fvdat) {
        return EIO;
    }

#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_unlock(data->biglock);
#endif
    err = cluster_pagein(vp, pl, (upl_offset_t)pl_offset, f_offset, (int)size,
                         fvdat->filesize, flags);
#ifdef FUSE4X_ENABLE_BIGLOCK
   fuse_biglock_lock(data->biglock);
#endif

    return err;
}

/*
    struct vnop_pageout_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        upl_t                a_pl;
        vm_offset_t          a_pl_offset;
        off_t                a_f_offset;
        size_t               a_size;
        int                  a_flags;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_pageout(struct vnop_pageout_args *ap)
{
    vnode_t       vp        = ap->a_vp;
    upl_t         pl        = ap->a_pl;
    vm_offset_t   pl_offset = ap->a_pl_offset;
    off_t         f_offset  = ap->a_f_offset;
    size_t        size      = ap->a_size;
    int           flags     = ap->a_flags;

    struct fuse_vnode_data *fvdat = VTOFUD(vp);
    int error;

#ifdef FUSE4X_ENABLE_BIGLOCK
    struct fuse_data *data = fuse_get_mpdata(vnode_mount(vp));
#endif

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp) || fuse_isdirectio(vp)) {
        if (!(flags & UPL_NOCOMMIT)) {
            ubc_upl_abort_range(pl, (upl_offset_t)pl_offset, (upl_size_t)size,
                                UPL_ABORT_FREE_ON_EMPTY | UPL_ABORT_ERROR);
        }
        /*
         * Will cause PAGER_ERROR (pager unable to read or write page).
         */
        return ENOTSUP;
    }

#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_unlock(data->biglock);
#endif
    error = cluster_pageout(vp, pl, (upl_offset_t)pl_offset, f_offset,
                            (int)size, (off_t)fvdat->filesize, flags);
#ifdef FUSE4X_ENABLE_BIGLOCK
   fuse_biglock_lock(data->biglock);
#endif

    return error;
}

/*
    struct vnop_pathconf_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        int                  a_name;
        int                 *a_retval;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_pathconf(struct vnop_pathconf_args *ap)
{
    vnode_t        vp        = ap->a_vp;
    int            name      = ap->a_name;
    int           *retvalPtr = ap->a_retval;
    vfs_context_t  context   = ap->a_context;

    int err;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(vp, context, ENOENT);

    err = 0;
    switch (name) {
        case _PC_LINK_MAX:
            *retvalPtr = FUSE_LINK_MAX;
            break;
        case _PC_NAME_MAX:
            *retvalPtr = FUSE_MAXNAMLEN;
            break;
        case _PC_PATH_MAX:
            *retvalPtr = MAXPATHLEN;
            break;
        case _PC_PIPE_BUF:
            *retvalPtr = PIPE_BUF;
            break;
        case _PC_CHOWN_RESTRICTED:
            *retvalPtr = 1;
            break;
        case _PC_NO_TRUNC:
            *retvalPtr = 0;
            break;
        case _PC_NAME_CHARS_MAX:
            *retvalPtr = 255;   // chars as opposed to bytes
            break;
        case _PC_CASE_SENSITIVE:
            *retvalPtr = 1;
            break;
        case _PC_CASE_PRESERVING:
            *retvalPtr = 1;
            break;

        /*
         * _PC_EXTENDED_SECURITY_NP and _PC_AUTH_OPAQUE_NP are handled
         * by the VFS.
         */

        // The following are terminal device stuff that we don't support:

        case _PC_MAX_CANON:
        case _PC_MAX_INPUT:
        case _PC_VDISABLE:
        default:
            err = EINVAL;
            break;
    }

    return err;
}

/*
    struct vnop_read_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        struct uio          *a_uio;
        int                  a_ioflag;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_read(struct vnop_read_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    uio_t         uio     = ap->a_uio;
    int           ioflag  = ap->a_ioflag;
    vfs_context_t context = ap->a_context;

    /*
     * XXX: Locking
     *
     * lock_shared(truncatelock)
     * call the cluster layer (note that we are always block-aligned)
     * lock(nodelock)
     * do cleanup
     * unlock(nodelock)
     * unlock(truncatelock)
     */

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return vnode_ischr(vp) ? 0 : ENXIO;
    }

    if (!vnode_isreg(vp)) {
        return vnode_isdir(vp) ? EISDIR : EPERM;
    }

    /*
     * if (uio_offset(uio) > SOME_MAXIMUM_SIZE) {
     *     return 0;
     * }
     */

    off_t orig_resid = uio_resid(uio);
    if (orig_resid == 0) {
        return 0;
    }

    off_t orig_offset = uio_offset(uio);
    if (orig_offset < 0) {
        return EINVAL;
    }

    struct fuse_vnode_data *fvdat = VTOFUD(vp);
    if (!fvdat) {
        return EINVAL;
    }

    /* Protect against size change here. */

    struct fuse_data *data = fuse_get_mpdata(vnode_mount(vp));

    if (fuse_isdirectio(vp)) {
        fufh_type_t             fufh_type = FUFH_RDONLY;
        struct fuse_dispatcher  fdi;
        struct fuse_filehandle *fufh = NULL;
        struct fuse_read_in    *fri = NULL;
        int err = 0;

        fufh = &(fvdat->fufh[fufh_type]);

        if (!FUFH_IS_VALID(fufh)) {
            fufh_type = FUFH_RDWR;
            fufh = &(fvdat->fufh[fufh_type]);
            if (!FUFH_IS_VALID(fufh)) {
                fufh = NULL;
            } else {
                /* Read falling back to FUFH_RDWR. */
            }
        }

        if (!fufh) {
            /* Failing direct I/O because of no fufh. */
            return EIO;
        } else {
            /* Using existing fufh of type fufh_type. */
        }

        fuse_dispatcher_init(&fdi, 0);

        while (uio_resid(uio) > 0) {

            fdi.iosize = sizeof(*fri);
            fuse_dispatcher_make_vp(&fdi, FUSE_READ, vp, context);
            fri = fdi.indata;
            fri->fh = fufh->fh_id;
            fri->offset = uio_offset(uio);
            fri->size = (uint32_t)min((size_t)uio_resid(uio), data->iosize);

            if ((err = fuse_dispatcher_wait_answer(&fdi))) {
                return err;
            }

#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_unlock(data->biglock);
#endif
            err = uiomove(fdi.answer, (int)min(fri->size, fdi.iosize), uio);
#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_lock(data->biglock);
#endif
            if (err || fdi.iosize < fri->size) {
                break;
            }
        }
        fuse_ticket_drop(fdi.ticket);

        return err;

    } else {  /* direct_io */
#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_unlock(data->biglock);
#endif
        int res = cluster_read(vp, uio, fvdat->filesize, ioflag);
#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_lock(data->biglock);
#endif
        return res;

    }
}

/*
    struct vnop_readdir_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        struct uio          *a_uio;
        int                  a_flags;
        int                 *a_eofflag;
        int                 *a_numdirent;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_readdir(struct vnop_readdir_args *ap)
{
    vnode_t        vp           = ap->a_vp;
    uio_t          uio          = ap->a_uio;
    int            flags        = ap->a_flags;
    int           *numdirentPtr = ap->a_numdirent;
    vfs_context_t  context      = ap->a_context;

    struct fuse_filehandle *fufh = NULL;
    struct fuse_vnode_data *fvdat;
    struct fuse_iov         cookediov;

    int err = 0;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(vp, context, EPERM);

    /* No cookies yet. */
    if (flags & (VNODE_READDIR_REQSEEKOFF | VNODE_READDIR_EXTENDED)) {
        return EINVAL;
    }

    const user_ssize_t dirent_size = (user_ssize_t)sizeof(struct fuse_dirent);
    if ((uio_iovcnt(uio) > 1) || (uio_resid(uio) < dirent_size)) {
        return EINVAL;
    }

    /*
     *  if ((uio_offset(uio) % dirent_size) != 0) { ...
     */

    fvdat = VTOFUD(vp);

    fufh = &(fvdat->fufh[FUFH_RDONLY]);

    if (FUFH_IS_VALID(fufh)) {
        FUFH_USE_INC(fufh);
        OSIncrementAtomic((SInt32 *)&fuse_fh_reuse_count);
    } else {
        err = fuse_filehandle_get(vp, context, FUFH_RDONLY, 0 /* mode */);
        if (err) {
            log("fuse4x: filehandle_get failed in readdir (err=%d)\n", err);
            return err;
        }
    }

    size_t dircookedsize = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + MAXNAMLEN + 1);
    fiov_init(&cookediov, dircookedsize);

    err = fuse_internal_readdir(vp, uio, context, fufh, &cookediov,
                                numdirentPtr);

    fiov_teardown(&cookediov);

    FUFH_USE_DEC(fufh);
    if (!FUFH_IS_VALID(fufh)) {
        (void)fuse_filehandle_put(vp, context, FUFH_RDONLY);
    }

    fuse_invalidate_attr(vp);

    return err;
}

/*
    struct vnop_readlink_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        struct uio          *a_uio;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_readlink(struct vnop_readlink_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    uio_t         uio     = ap->a_uio;
    vfs_context_t context = ap->a_context;

    struct fuse_dispatcher fdi;
    int err;

#ifdef FUSE4X_ENABLE_BIGLOCK
    struct fuse_data *data = fuse_get_mpdata(vnode_mount(vp));
#endif

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(vp, context, ENOENT);

    if (!vnode_islnk(vp)) {
        return EINVAL;
    }

    if ((err = fuse_dispatcher_simple_putget_vp(&fdi, FUSE_READLINK, vp, context))) {
        return err;
    }

    if (((char *)fdi.answer)[0] == '/' &&
        fuse_get_mpdata(vnode_mount(vp))->dataflags & FSESS_JAIL_SYMLINKS) {
            char *mpth = vfs_statfs(vnode_mount(vp))->f_mntonname;
            err = uiomove(mpth, (int)strlen(mpth), uio);
    }

    if (!err) {
#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_unlock(data->biglock);
#endif
        err = uiomove(fdi.answer, (int)fdi.iosize, uio);
#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_lock(data->biglock);
#endif
    }

    fuse_ticket_drop(fdi.ticket);
    fuse_invalidate_attr(vp);

    return err;
}

/*
    struct vnop_reclaim_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_reclaim(struct vnop_reclaim_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    vfs_context_t context = ap->a_context;

    struct fuse_vnode_data *fvdat = VTOFUD(vp);
    struct fuse_filehandle *fufh = NULL;
    struct fuse_data *data = fuse_get_mpdata(vnode_mount(vp));

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        goto out;
    }

    if (!fvdat) {
        panic("fuse4x: no vnode data during recycling");
    }

    /*
     * Cannot do early bail out on a dead file system in this case.
     */

    for (int type = 0; type < FUFH_MAXTYPE; type++) {
        fufh = &(fvdat->fufh[type]);
        if (FUFH_IS_VALID(fufh)) {
            FUFH_USE_RESET(fufh);
            if (vfs_isforce(vnode_mount(vp))) {
                (void)fuse_filehandle_put(vp, context, type);
            } else {

                /*
                 * This is not a forced unmount. So why is the vnode being
                 * reclaimed if a fufh is valid? Well...
                 *
                 * One reason is that we are dead.
                 *
                 * Another reason is an unmount-time vflush race with ongoing
                 * vnops. Typically happens for a VDIR here.
                 *
                 * More often, the following happened:
                 *
                 *     open()
                 *     mmap()
                 *     close()
                 *     pagein... read... strategy
                 *     done... reclaim
                 */

                if (!fuse_isdeadfs(vp)) {
                    OSIncrementAtomic((SInt32 *)&fuse_fh_zombies);
                } /* !deadfs */

                (void)fuse_filehandle_put(vp, context, type);

            } /* !forced unmount */
        } /* valid fufh */
    } /* fufh loop */

    if (fvdat->nlookup) {
        struct fuse_dispatcher fdi;
        fdi.ticket = NULL;
        fuse_internal_forget_send(vnode_mount(vp), context, VTOI(vp),
                                  fvdat->nlookup, &fdi);
    }

out:
    fuse_vncache_purge(vp);

    fuse_lck_mtx_lock(data->node_mtx);
    RB_REMOVE(fuse_data_nodes, &data->nodes_head, fvdat);
    fuse_lck_mtx_unlock(data->node_mtx);
    vnode_removefsref(vp);

    fuse_vnode_data_destroy(fvdat);
    vnode_clearfsnode(vp);
    OSDecrementAtomic((SInt32 *)&fuse_vnodes_current);

    return 0;
}

/*
    struct vnop_remove_args {
        struct vnodeop_desc  *a_desc;
        vnode_t               a_dvp;
        vnode_t               a_vp;
        struct componentname *a_cnp;
        int                   a_flags;
        vfs_context_t         a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_remove(struct vnop_remove_args *ap)
{
    vnode_t               dvp     = ap->a_dvp;
    vnode_t               vp      = ap->a_vp;
    struct componentname *cnp     = ap->a_cnp;
    int                   flags   = ap->a_flags;
    vfs_context_t         context = ap->a_context;

    int err;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(vp, context, ENOENT);

    if (vnode_isdir(vp)) {
        return EPERM;
    }

    /* Check for Carbon delete semantics. */
    if ((flags & VNODE_REMOVE_NODELETEBUSY) && vnode_isinuse(vp, 0)) {
        return EBUSY;
    }

    fuse_vncache_purge(vp);

    err = fuse_internal_remove(dvp, vp, cnp, FUSE_UNLINK, context);

    if (err == 0) {
        fuse_vncache_purge(vp);
        fuse_invalidate_attr(dvp);
        /*
         * If we really want, we could...
         * if (!vnode_isinuse(vp, 0)) {
         *     vnode_recycle(vp);
         * }
         */
    }

    return err;
}

/*
    struct vnop_removexattr_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        char                *a_name;
        int                  a_options;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_removexattr(struct vnop_removexattr_args *ap)
{
    vnode_t        vp      = ap->a_vp;
    const char    *name    = ap->a_name;
    vfs_context_t  context = ap->a_context;

    struct fuse_dispatcher fdi;
    struct fuse_data      *data;

    mount_t mp;
    size_t  namelen;

    int err = 0;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(vp, context, ENOENT);

    if (name == NULL || name[0] == '\0') {
        return EINVAL;  /* invalid name */
    }

    mp = vnode_mount(vp);
    data = fuse_get_mpdata(mp);

    if (fuse_skip_apple_xattr_mp(mp, name)) {
        return EPERM;
    }

    if (data->dataflags & FSESS_AUTO_XATTR) {
        return ENOTSUP;
    }

    if (!fuse_implemented(data, FSESS_NOIMPLBIT(REMOVEXATTR))) {
        return ENOTSUP;
    }

    namelen = strlen(name);

    fuse_dispatcher_init(&fdi, namelen + 1);
    fuse_dispatcher_make_vp(&fdi, FUSE_REMOVEXATTR, vp, context);

    memcpy((char *)fdi.indata, name, namelen);
    ((char *)fdi.indata)[namelen] = '\0';

    err = fuse_dispatcher_wait_answer(&fdi);
    if (!err) {
        fuse_ticket_drop(fdi.ticket);
        VTOFUD(vp)->c_flag |= C_TOUCH_CHGTIME;
        fuse_invalidate_attr(vp);
    } else {
        if (err == ENOSYS) {
            fuse_clear_implemented(data, FSESS_NOIMPLBIT(REMOVEXATTR));
            return ENOTSUP;
        }
    }

    return err;
}

/*
    struct vnop_rename_args {
        struct vnodeop_desc  *a_desc;
        vnode_t               a_fdvp;
        vnode_t               a_fvp;
        struct componentname *a_fcnp;
        vnode_t               a_tdvp;
        vnode_t               a_tvp;
        struct componentname *a_tcnp;
        vfs_context_t         a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_rename(struct vnop_rename_args *ap)
{
    vnode_t fdvp               = ap->a_fdvp;
    vnode_t fvp                = ap->a_fvp;
    struct componentname *fcnp = ap->a_fcnp;
    vnode_t tdvp               = ap->a_tdvp;
    vnode_t tvp                = ap->a_tvp;
    struct componentname *tcnp = ap->a_tcnp;
    vfs_context_t context      = ap->a_context;

    int err = 0;

    fuse_trace_printf_vnop_novp();

    if (fuse_isdeadfs(fdvp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(fdvp, context, ENOENT);

    fuse_vncache_purge(fvp);

    err = fuse_internal_rename(fdvp, fvp, fcnp, tdvp, tvp, tcnp, ap->a_context);

    if (err == 0) {
        fuse_invalidate_attr(fdvp);
        if (tdvp != fdvp) {
            fuse_invalidate_attr(tdvp);
        }
    }

    if (tvp != NULLVP) {
        if (tvp != fvp) {
            fuse_vncache_purge(tvp);
        }
        if (err == 0) {

            /*
             * If we want the file to just "disappear" from the standpoint
             * of those who might have it open, we can do a revoke/recycle
             * here. Otherwise, don't do anything. Only doing a recycle will
             * make our fufh-checking code in reclaim unhappy, leading us to
             * proactively panic.
             */

            /*
             * 1. revoke
             * 2. recycle
             */
        }
    }

    if (vnode_isdir(fvp)) {
        if ((tvp != NULLVP) && vnode_isdir(tvp)) {
            fuse_vncache_purge(tdvp);
        }
        fuse_vncache_purge(fdvp);
    }

    return err;
}

/*
    struct vnop_rmdir_args {
        struct vnodeop_desc  *a_desc;
        vnode_t               a_dvp;
        vnode_t               a_vp;
        struct componentname *a_cnp;
        vfs_context_t         a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_rmdir(struct vnop_rmdir_args *ap)
{
    vnode_t               dvp     = ap->a_dvp;
    vnode_t               vp      = ap->a_vp;
    struct componentname *cnp     = ap->a_cnp;
    vfs_context_t         context = ap->a_context;

    int err;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(vp, context, ENOENT);

    if (VTOFUD(vp) == VTOFUD(dvp)) {
        return EINVAL;
    }

    fuse_vncache_purge(vp);

    err = fuse_internal_remove(dvp, vp, cnp, FUSE_RMDIR, context);

    if (err == 0) {
        fuse_invalidate_attr(dvp);
    }

    return err;
}

/*
struct vnop_select_args {
    struct vnodeop_desc *a_desc;
    vnode_t              a_vp;
    int                  a_which;
    int                  a_fflags;
    void                *a_wql;
    vfs_context_t        a_context;
};
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_select(struct vnop_select_args *ap)
{
    fuse_trace_printf_vnop_novp();

    return 1;
}

/*
    struct vnop_setattr_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        struct vnode_attr   *a_vap;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_setattr(struct vnop_setattr_args *ap)
{
    vnode_t            vp      = ap->a_vp;
    struct vnode_attr *vap     = ap->a_vap;
    vfs_context_t      context = ap->a_context;

    struct fuse_dispatcher  fdi;
    struct fuse_setattr_in *fsai;

    int err = 0;
    enum vtype vtyp;
    int sizechanged = 0;
    uint64_t newsize = 0;

#ifdef FUSE4X_ENABLE_BIGLOCK
    struct fuse_data *data = fuse_get_mpdata(vnode_mount(vp));
#endif

    fuse_trace_printf_vnop();

    /*
     * XXX: Locking
     *
     * We need to worry about the file size changing in setattr(). If the call
     * is indeed altering the size, then:
     *
     * lock_exclusive(truncatelock)
     *   lock(nodelock)
     *     set the new size
     *   unlock(nodelock)
     *   adjust ubc
     *   lock(nodelock)
     *     do cleanup
     *   unlock(nodelock)
     * unlock(truncatelock)
     * ...
     */

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(vp, context, ENOENT);

    fuse_dispatcher_init(&fdi, sizeof(*fsai));
    fuse_dispatcher_make_vp(&fdi, FUSE_SETATTR, vp, context);
    fsai = fdi.indata;

    sizechanged = fuse_internal_attr_vat2fsai(vnode_mount(vp), vp, vap,
                                              fsai, &newsize);

    if (!fsai->valid) {
        goto out;
    }

    vtyp = vnode_vtype(vp);

    if (fsai->valid & FATTR_SIZE && vtyp == VDIR) {
        err = EISDIR;
        goto out;
    }

    if (vnode_vfsisrdonly(vp) && (fsai->valid & ~FATTR_SIZE || vtyp == VREG)) {
        err = EROFS;
        goto out;
    }

    if ((err = fuse_dispatcher_wait_answer(&fdi))) {
        fuse_invalidate_attr(vp);
        return err;
    }

    vtyp = IFTOVT(((struct fuse_attr_out *)fdi.answer)->attr.mode);

    if (vnode_vtype(vp) != vtyp) {
        if ((vnode_vtype(vp) == VNON) && (vtyp != VNON)) {
            /* What just happened here? */
        } else {

            /*
             * STALE vnode, ditch
             *
             * The vnode has changed its type "behind our back". There's
             * nothing really we can do, so let us just force an internal
             * revocation and tell the caller to try again, if interested.
             */

#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_unlock(data->biglock);
#endif
            fuse_vncache_purge(vp); 
#ifdef FUSE4X_ENABLE_BIGLOCK
            fuse_biglock_lock(data->biglock);
#endif

            err = EAGAIN;
        }
    }

    if (!err) {
        if (sizechanged) {
            fuse_invalidate_attr(vp);
        } else {
            cache_attrs(vp, (struct fuse_attr_out *)fdi.answer);
            if (fsai->valid & FATTR_BKUPTIME || fsai->valid & FATTR_CRTIME) {
                VTOFUD(vp)->c_flag &= ~C_XTIMES_VALID;
            }
        }
    }

out:
    fuse_ticket_drop(fdi.ticket);
    if (!err && sizechanged) {
        VTOFUD(vp)->filesize = newsize;
        ubc_setsize(vp, (off_t)newsize);
    }

    return err;
}

/*
    struct vnop_setxattr_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        char                *a_name;
        uio_t                a_uio;
        int                  a_options;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_setxattr(struct vnop_setxattr_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    const char   *name    = ap->a_name;
    uio_t         uio     = ap->a_uio;
    vfs_context_t context = ap->a_context;

    struct fuse_dispatcher   fdi;
    struct fuse_setxattr_in *fsxi;
    struct fuse_data        *data;

    user_addr_t a_baseaddr[FUSE_UIO_BACKUP_MAX];
    user_size_t a_length[FUSE_UIO_BACKUP_MAX];

    mount_t mp;

    int err = 0;
    int iov_err = 0;
    int i, iov_cnt;
    size_t namelen;
    size_t attrsize;
    off_t  saved_offset;

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(vp, context, ENOENT);

    if (name == NULL || name[0] == '\0') {
        return EINVAL;
    }

    mp = vnode_mount(vp);
    data = fuse_get_mpdata(mp);

    if (fuse_skip_apple_xattr_mp(mp, name)) {
        return EPERM;
    }

    if (data->dataflags & FSESS_AUTO_XATTR) {
        return ENOTSUP;
    }

    if (!fuse_implemented(data, FSESS_NOIMPLBIT(SETXATTR))) {
        return ENOTSUP;
    }

    attrsize = (size_t)uio_resid(uio);
    saved_offset = uio_offset(uio);

    iov_cnt = uio_iovcnt(uio);
    if (iov_cnt > FUSE_UIO_BACKUP_MAX) {
        /* no need to make it more complicated */
        iov_cnt = FUSE_UIO_BACKUP_MAX;
    }

    for (i = 0; i < iov_cnt; i++) {
        iov_err = uio_getiov(uio, i, &(a_baseaddr[i]), &(a_length[i]));
    }

    /*
     * Check attrsize for some sane maximum: otherwise, we can fail malloc()
     * in fuse_dispatcher_make_vp().
     */
    if (attrsize > data->userkernel_bufsize) {
        return E2BIG;
    }

    namelen = strlen(name);

    fuse_dispatcher_init(&fdi, sizeof(*fsxi) + namelen + 1 + attrsize);
    err = fuse_dispatcher_make_vp_canfail(&fdi, FUSE_SETXATTR, vp, ap->a_context);
    if (err) {
        log("fuse4x: setxattr failed for too large attribute (%lu)\n",
              attrsize);
        return ERANGE;
    }
    fsxi = fdi.indata;

    fsxi->size = (uint32_t)attrsize;
    fsxi->flags = ap->a_options;
    fsxi->position = (uint32_t)saved_offset;

    if (attrsize > FUSE_REASONABLE_XATTRSIZE) {
        fdi.ticket->killed = true;
    }

    memcpy((char *)fdi.indata + sizeof(*fsxi), name, namelen);
    ((char *)fdi.indata)[sizeof(*fsxi) + namelen] = '\0';

#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_unlock(data->biglock);
#endif
    err = uiomove((char *)fdi.indata + sizeof(*fsxi) + namelen + 1,
                  (int)attrsize, uio);
#ifdef FUSE4X_ENABLE_BIGLOCK
    fuse_biglock_lock(data->biglock);
#endif
    if (!err) {
        err = fuse_dispatcher_wait_answer(&fdi);
    }

    if (!err) {
        fuse_ticket_drop(fdi.ticket);
        fuse_invalidate_attr(vp);
        VTOFUD(vp)->c_flag |= C_TOUCH_CHGTIME;
    } else {
        if ((err == ENOSYS) || (err == ENOTSUP)) {

            int a_spacetype = UIO_USERSPACE;

            if (err == ENOSYS) {
                fuse_clear_implemented(data, FSESS_NOIMPLBIT(SETXATTR));
            }

            if (iov_err) {
                return EAGAIN;
            }

            if (!uio_isuserspace(uio)) {
                a_spacetype = UIO_SYSSPACE;
            }

            uio_reset(uio, saved_offset, a_spacetype, uio_rw(uio));
            for (i = 0; i < iov_cnt; i++) {
                uio_addiov(uio, CAST_USER_ADDR_T(a_baseaddr[i]), a_length[i]);
            }

            return ENOTSUP;
        }
    }

    return err;
}

/*
    struct vnop_strategy_args {
        struct vnodeop_desc *a_desc;
        struct buf          *a_bp;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_strategy(struct vnop_strategy_args *ap)
{
    buf_t   bp = ap->a_bp;
    vnode_t vp = buf_vnode(bp);

    fuse_trace_printf_vnop();

    if (!vp || fuse_isdeadfs(vp)) {
        buf_seterror(bp, EIO);
        buf_biodone(bp);
        return ENXIO;
    }

    return fuse_internal_strategy_buf(ap);
}

/*
    struct vnop_symlink_args {
        struct vnodeop_desc  *a_desc;
        vnode_t               a_dvp;
        vnode_t              *a_vpp;
        struct componentname *a_cnp;
        struct vnode_attr    *a_vap;
        char                 *a_target;
        vfs_context_t         a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_symlink(struct vnop_symlink_args *ap)
{
    vnode_t               dvp     = ap->a_dvp;
    vnode_t              *vpp     = ap->a_vpp;
    struct componentname *cnp     = ap->a_cnp;
    char                 *target  = ap->a_target;
    vfs_context_t         context = ap->a_context;

    struct fuse_dispatcher fdi;

    int err;
    size_t len;

    fuse_trace_printf_vnop_novp();

    if (fuse_isdeadfs(dvp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(dvp, context, EPERM);

    len = strlen(target) + 1;
    fuse_dispatcher_init(&fdi, len + cnp->cn_namelen + 1);
    fuse_dispatcher_make_vp(&fdi, FUSE_SYMLINK, dvp, context);

    memcpy(fdi.indata, cnp->cn_nameptr, cnp->cn_namelen);
    ((char *)fdi.indata)[cnp->cn_namelen] = '\0';
    memcpy((char *)fdi.indata + cnp->cn_namelen + 1, target, len);

    /* XXX: Need to take vap into account. */

    err = fuse_internal_newentry_core(dvp, vpp, cnp, VLNK, &fdi, context);

    if (err == 0) {
        fuse_invalidate_attr(dvp);
    }

    return err;
}

/*
    struct vnop_write_args {
        struct vnodeop_desc *a_desc;
        vnode_t              a_vp;
        struct uio          *a_uio;
        int                  a_ioflag;
        vfs_context_t        a_context;
    };
*/
FUSE_VNOP_EXPORT
int
fuse_vnop_write(struct vnop_write_args *ap)
{
    vnode_t       vp      = ap->a_vp;
    uio_t         uio     = ap->a_uio;
    int           ioflag  = ap->a_ioflag;
    vfs_context_t context = ap->a_context;

    int          error = 0;
    int          lflag;
    off_t        offset;
    off_t        zero_off;
    off_t        filesize;
    off_t        original_offset;
    off_t        original_size;
    user_ssize_t original_resid;

    /*
     * XXX: Locking
     *
     * lock_shared(truncatelock)
     * lock(nodelock)
     * if (file is being extended) {
     *     unlock(nodelock)
     *     unlock(truncatelock)
     *     lock_exclusive(truncatelock)
     *     lock(nodelock)
     *     current_size = the file's current size
     * }
     * if (file is being extended) { // check again
     *     // do whatever needs to be done to allocate storage
     * }
     * // We are always block-aligned
     * unlock(nodelock)
     * call the cluster layer
     * adjust ubc
     * lock(nodelock)
     * do cleanup
     * unlock(nodelock)
     * unlock(truncatelock)
     */

    fuse_trace_printf_vnop();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    struct fuse_vnode_data *fvdat = VTOFUD(vp);

    switch (vnode_vtype(vp)) {
    case VREG:
        break;

    case VDIR:
        return EISDIR;

    default:
        return EPERM; /* or EINVAL? panic? */
    }

    original_resid = uio_resid(uio);
    original_offset = uio_offset(uio);
    offset = original_offset;

    if (original_resid == 0) {
        return 0;
    }

    if (original_offset < 0) {
        return EINVAL;
    }

    if (fuse_isdirectio(vp)) {
        fufh_type_t             fufh_type = FUFH_WRONLY;
        struct fuse_dispatcher  fdi;
        struct fuse_filehandle *fufh = NULL;
        struct fuse_write_in   *fwi  = NULL;
        struct fuse_write_out  *fwo  = NULL;
        struct fuse_data       *data = fuse_get_mpdata(vnode_mount(vp));

        size_t chunksize;
        off_t  diff;

        fufh = &(fvdat->fufh[fufh_type]);

        if (!FUFH_IS_VALID(fufh)) {
            fufh_type = FUFH_RDWR;
            fufh = &(fvdat->fufh[fufh_type]);
            if (!FUFH_IS_VALID(fufh)) {
                fufh = NULL;
            } else {
                /* Write falling back to FUFH_RDWR. */
            }
        }

        if (!fufh) {
            /* Failing direct I/O because of no fufh. */
            return EIO;
        } else {
            /* Using existing fufh of type fufh_type. */
        }

        fuse_dispatcher_init(&fdi, 0);

        while (uio_resid(uio) > 0) {
            chunksize = min((size_t)uio_resid(uio), data->iosize);
            fdi.iosize = sizeof(*fwi) + chunksize;
            fuse_dispatcher_make_vp(&fdi, FUSE_WRITE, vp, context);
            fwi = fdi.indata;
            fwi->fh = fufh->fh_id;
            fwi->offset = uio_offset(uio);
            fwi->size = (uint32_t)chunksize;

            error = uiomove((char *)fdi.indata + sizeof(*fwi), (int)chunksize,
                            uio);
            if (error) {
                break;
            }

            error = fuse_dispatcher_wait_answer(&fdi);
            if (error) {
                return error;
            }

            fwo = (struct fuse_write_out *)fdi.answer;

            diff = chunksize - fwo->size;
            if (diff < 0) {
                error = EINVAL;
                break;
            }

            uio_setresid(uio, (uio_resid(uio) + diff));
            uio_setoffset(uio, (uio_offset(uio) - diff));

        } /* while */

        if (!error) {
            fuse_invalidate_attr(vp);
        }

        fuse_ticket_drop(fdi.ticket);

        return error;

    } else { /* !direct_io */

        /* Be wary of a size change here. */

        original_size = fvdat->filesize;

        if (ioflag & IO_APPEND) {
            /* Arrange for append */
            uio_setoffset(uio, fvdat->filesize);
            offset = fvdat->filesize;
        }

        if (offset < 0) {
            return EFBIG;
        }

        if (offset + original_resid > original_size) {
            /* Need to extend the file. */
            filesize = offset + original_resid;
            fvdat->filesize = filesize;
        } else {
            /* Original size OK. */
            filesize = original_size;
        }

        lflag = (ioflag & (IO_SYNC | IO_NOCACHE));

        if (vfs_issynchronous(vnode_mount(vp))) {
            lflag |= IO_SYNC;
        }

        if (offset > original_size) {
            zero_off = original_size;
            lflag |= IO_HEADZEROFILL;
            /* Zero-filling enabled. */
        } else {
            zero_off = 0;
        }

#ifdef FUSE4X_ENABLE_BIGLOCK
        struct fuse_data *data = fuse_get_mpdata(vnode_mount(vp));
        fuse_biglock_unlock(data->biglock);
#endif
        error = cluster_write(vp, uio, (off_t)original_size, (off_t)filesize,
                          (off_t)zero_off, (off_t)0, lflag);
#ifdef FUSE4X_ENABLE_BIGLOCK
        fuse_biglock_lock(data->biglock);
#endif

        if (!error) {
            if (uio_offset(uio) > original_size) {
                /* Updating to new size. */
                fvdat->filesize = uio_offset(uio);
                ubc_setsize(vp, (off_t)fvdat->filesize);
            } else {
                fvdat->filesize = original_size;
            }
            fuse_invalidate_attr(vp);
        }

        /*
         * If original_resid > uio_resid(uio), we could set an internal
         * flag bit to "update" (e.g., dep->de_flag |= DE_UPDATE).
         */

        /*
         * If the write failed and they want us to, truncate the file back
         * to the size it was before the write was attempted.
         */

        if (error) {
            if (ioflag & IO_UNIT) {
                /*
                 * e.g.: detrunc(dep, original_size, ioflag & IO_SYNC, context);
                 */
                uio_setoffset(uio, original_offset);
                uio_setresid(uio, original_resid);
            } else {
                /*
                 * e.g.: detrunc(dep, dep->de_FileSize, ioflag & IO_SYNC, context);
                 */
                if (uio_resid(uio) != original_resid) {
                    error = 0;
                }
            }
        }

        /*
         if ((original_resid > uio_resid(uio)) &&
         !fuse_vfs_context_issuser(context)) {
            // clear setuid/setgid here
         }
         */

    return error;
    }
}

/*
    struct vnop_ioctl_args {
        struct vnodeop_desc *a_desc;
        vnode_t a_vp;
        u_long a_command;
        caddr_t a_data;
        int a_fflag;
        vfs_context_t a_context;
    };
 */
FUSE_VNOP_EXPORT
int
fuse_vnop_ioctl(struct vnop_ioctl_args *ap)
{
    vnode_t vp            = ap->a_vp;
    vfs_context_t context = ap->a_context;

    struct fuse_dispatcher fdi;
    struct fuse_ioctl_in *fioi;
    struct fuse_data *data;
    mount_t mp;

    fuse_trace_printf_vnop_novp();

    if (fuse_isdeadfs(vp)) {
        return ENXIO;
    }

    CHECK_BLANKET_DENIAL(vp, context, EPERM);

    mp = vnode_mount(vp);
    data = fuse_get_mpdata(mp);

    if (!fuse_implemented(data, FSESS_NOIMPLBIT(IOCTL))) {
        return ENOTSUP;
    }

    fufh_type_t fufh_type = fuse_filehandle_xlate_from_fflags(ap->a_fflag);
    struct fuse_filehandle *fufh = &(VTOFUD(vp)->fufh[fufh_type]);

    if (!FUFH_IS_VALID(fufh)) {
        return EIO;
    }

    const int iodata_size = (int)IOCPARM_LEN(ap->a_command);
    fuse_dispatcher_init(&fdi, sizeof(*fioi) + iodata_size);
    fuse_dispatcher_make_vp(&fdi, FUSE_IOCTL, vp, context);

    fioi = fdi.indata;
    fioi->fh = fufh->fh_id;
    fioi->cmd = (uint32_t)ap->a_command;
    if (ap->a_command | IOC_IN) {
        fioi->in_size = iodata_size;
        memcpy((char *)fdi.indata + sizeof(*fioi), ap->a_data, iodata_size);
    }
    if (ap->a_command | IOC_OUT) {
        fioi->out_size = iodata_size;
    }

    int err = fuse_dispatcher_wait_answer(&fdi);

    if (!err) {
        if (ap->a_command | IOC_OUT) {
            memcpy(ap->a_data, (char *)fdi.answer + sizeof(struct fuse_ioctl_out), iodata_size);
        }
        fuse_ticket_drop(fdi.ticket);
    } else {
        if (err == ENOSYS) {
            fuse_clear_implemented(data, FSESS_NOIMPLBIT(IOCTL));
            err = 0;
        }
    }

    return err;
}


struct vnodeopv_entry_desc fuse_vnode_operation_entries[] = {
    { &vnop_access_desc,        (fuse_vnode_op_t) fuse_vnop_access        },
    { &vnop_allocate_desc,      (fuse_vnode_op_t) nop_allocate            }, // vnop stub until FUSE_FALLOCATE is implemented
    { &vnop_blktooff_desc,      (fuse_vnode_op_t) fuse_vnop_blktooff      },
    { &vnop_blockmap_desc,      (fuse_vnode_op_t) fuse_vnop_blockmap      },
    { &vnop_close_desc,         (fuse_vnode_op_t) fuse_vnop_close         },
    { &vnop_create_desc,        (fuse_vnode_op_t) fuse_vnop_create        },
    { &vnop_exchange_desc,      (fuse_vnode_op_t) fuse_vnop_exchange      },
    { &vnop_fsync_desc,         (fuse_vnode_op_t) fuse_vnop_fsync         },
    { &vnop_getattr_desc,       (fuse_vnode_op_t) fuse_vnop_getattr       },
    { &vnop_getxattr_desc,      (fuse_vnode_op_t) fuse_vnop_getxattr      },
    { &vnop_inactive_desc,      (fuse_vnode_op_t) fuse_vnop_inactive      },
    { &vnop_ioctl_desc,         (fuse_vnode_op_t) fuse_vnop_ioctl         },
    { &vnop_link_desc,          (fuse_vnode_op_t) fuse_vnop_link          },
    { &vnop_listxattr_desc,     (fuse_vnode_op_t) fuse_vnop_listxattr     },
    { &vnop_lookup_desc,        (fuse_vnode_op_t) fuse_vnop_lookup        },
    { &vnop_mkdir_desc,         (fuse_vnode_op_t) fuse_vnop_mkdir         },
    { &vnop_mknod_desc,         (fuse_vnode_op_t) fuse_vnop_mknod         },
    { &vnop_mmap_desc,          (fuse_vnode_op_t) fuse_vnop_mmap          },
    { &vnop_mnomap_desc,        (fuse_vnode_op_t) fuse_vnop_mnomap        },
    { &vnop_offtoblk_desc,      (fuse_vnode_op_t) fuse_vnop_offtoblk      },
    { &vnop_open_desc,          (fuse_vnode_op_t) fuse_vnop_open          },
    { &vnop_pagein_desc,        (fuse_vnode_op_t) fuse_vnop_pagein        },
    { &vnop_pageout_desc,       (fuse_vnode_op_t) fuse_vnop_pageout       },
    { &vnop_pathconf_desc,      (fuse_vnode_op_t) fuse_vnop_pathconf      },
    { &vnop_read_desc,          (fuse_vnode_op_t) fuse_vnop_read          },
    { &vnop_readdir_desc,       (fuse_vnode_op_t) fuse_vnop_readdir       },
    { &vnop_readlink_desc,      (fuse_vnode_op_t) fuse_vnop_readlink      },
    { &vnop_reclaim_desc,       (fuse_vnode_op_t) fuse_vnop_reclaim       },
    { &vnop_remove_desc,        (fuse_vnode_op_t) fuse_vnop_remove        },
    { &vnop_removexattr_desc,   (fuse_vnode_op_t) fuse_vnop_removexattr   },
    { &vnop_rename_desc,        (fuse_vnode_op_t) fuse_vnop_rename        },
    { &vnop_revoke_desc,        (fuse_vnode_op_t) nop_revoke              },
    { &vnop_rmdir_desc,         (fuse_vnode_op_t) fuse_vnop_rmdir         },
    { &vnop_select_desc,        (fuse_vnode_op_t) fuse_vnop_select        },
    { &vnop_setattr_desc,       (fuse_vnode_op_t) fuse_vnop_setattr       },
    { &vnop_setxattr_desc,      (fuse_vnode_op_t) fuse_vnop_setxattr      },
    { &vnop_strategy_desc,      (fuse_vnode_op_t) fuse_vnop_strategy      },
    { &vnop_symlink_desc,       (fuse_vnode_op_t) fuse_vnop_symlink       },
    { &vnop_write_desc,         (fuse_vnode_op_t) fuse_vnop_write         },
    { &vnop_default_desc,       (fuse_vnode_op_t) vn_default_error        },
    { NULL, NULL }
};
