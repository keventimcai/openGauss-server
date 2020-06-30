/* -------------------------------------------------------------------------
 *
 * slot.cpp
 *	   Replication slot management.
 *
 *
 * Portions Copyright (c) 2020 Huawei Technologies Co.,Ltd.
 * Copyright (c) 2012-2014, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/gausskernel/storage/replication/slot.cpp
 *
 * NOTES
 *
 * Replication slots are used to keep state about replication streams
 * originating from this cluster.  Their primary purpose is to prevent the
 * premature removal of WAL or of old tuple versions in a manner that would
 * interfere with replication; they are also useful for monitoring purposes.
 * Slots need to be permanent (to allow restarts), crash-safe, and allocatable
 * on standbys (to support cascading setups).  The requirement that slots be
 * usable on standbys precludes storing them in the system catalogs.
 *
 * Each replication slot gets its own directory inside the $PGDATA/pg_replslot
 * directory. Inside that directory the state file will contain the slot's
 * own data. Additional data can be stored alongside that file if required.
 * While the server is running, the state data is also cached in memory for
 * efficiency.
 *
 * ReplicationSlotAllocationLock must be taken in exclusive mode to allocate
 * or free a slot. ReplicationSlotControlLock must be taken in shared mode
 * to iterate over the slots, and in exclusive mode to change the in_use flag
 * of a slot.  The remaining data in each slot is protected by its mutex.
 *
 * -------------------------------------------------------------------------
 */
#include "postgres.h"
#include "knl/knl_variable.h"

#include <unistd.h>
#include <sys/stat.h>

#include "access/transam.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "replication/slot.h"
#include "replication/walreceiver.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/proc.h"
#include "storage/procarray.h"
#include "postmaster/postmaster.h"

extern bool PMstateIsRun(void);

static void ReplicationSlotDropAcquired(void);

/* internal persistency functions */
static void RestoreSlotFromDisk(const char* name);
static void RecoverReplSlotFile(const ReplicationSlotOnDisk& cp, const char* name);
static void SaveSlotToPath(ReplicationSlot* slot, const char* path, int elevel);
static char* trim_str(char* str, int str_len, char sep);
static char* get_application_name(void);

/*
 * Report shared-memory space needed by ReplicationSlotShmemInit.
 */
Size ReplicationSlotsShmemSize(void)
{
    Size size = 0;

    if (g_instance.attr.attr_storage.max_replication_slots == 0)
        return size;

    size = offsetof(ReplicationSlotCtlData, replication_slots);
    size = add_size(size, mul_size((Size)g_instance.attr.attr_storage.max_replication_slots, sizeof(ReplicationSlot)));

    return size;
}

/*
 * Allocate and initialize walsender-related shared memory.
 */
void ReplicationSlotsShmemInit(void)
{
    bool found = false;

    if (g_instance.attr.attr_storage.max_replication_slots == 0)
        return;

    t_thrd.slot_cxt.ReplicationSlotCtl =
        (ReplicationSlotCtlData*)ShmemInitStruct("ReplicationSlot Ctl", ReplicationSlotsShmemSize(), &found);

    if (!found) {
        int i;
        errno_t rc = 0;

        /* First time through, so initialize */
        rc = memset_s(t_thrd.slot_cxt.ReplicationSlotCtl, ReplicationSlotsShmemSize(), 0, ReplicationSlotsShmemSize());
        securec_check(rc, "\0", "\0");

        for (i = 0; i < g_instance.attr.attr_storage.max_replication_slots; i++) {
            ReplicationSlot* slot = &t_thrd.slot_cxt.ReplicationSlotCtl->replication_slots[i];

            /* everything else is zeroed by the memset above */
            SpinLockInit(&slot->mutex);
            slot->io_in_progress_lock = LWLockAssign(LWTRANCHE_REPLICATION_SLOT);
        }
    }
}

/*
 * Check whether the passed slot name is valid and report errors at elevel.
 *
 * Slot names may consist out of [a-z0-9_]{1,NAMEDATALEN-1} which should allow
 * the name to be used as a directory name on every supported OS.
 *
 * Returns whether the directory name is valid or not if elevel < ERROR.
 */
bool ReplicationSlotValidateName(const char* name, int elevel)
{
    const char* cp = NULL;

    if (name == NULL) {
        ereport(elevel, (errcode(ERRCODE_INVALID_NAME), errmsg("replication slot name should not be NULL.")));
        return false;
    }

    if (strlen(name) == 0) {
        ereport(elevel, (errcode(ERRCODE_INVALID_NAME), errmsg("replication slot name \"%s\" is too short", name)));
        return false;
    }

    if (strlen(name) >= NAMEDATALEN) {
        ereport(elevel, (errcode(ERRCODE_NAME_TOO_LONG), errmsg("replication slot name \"%s\" is too long", name)));
        return false;
    }

    for (cp = name; *cp; cp++) {
        if (!((*cp >= 'a' && *cp <= 'z') || (*cp >= '0' && *cp <= '9') || (*cp == '_') || (*cp == '?') ||
                (*cp == '<') || (*cp == '!') || (*cp == '-') || (*cp == '.'))) {
            ereport(elevel,
                (errcode(ERRCODE_INVALID_NAME),
                    errmsg("replication slot name \"%s\" contains invalid character", name),
                    errhint("Replication slot names may only contain letters, numbers and the underscore character.")));
            return false;
        }
    }
    return true;
}
/*
 * Check whether the passed slot name is valid and report errors at elevel.
 *
 * Slot names may consist out of [a-z0-9_]{1,NAMEDATALEN-1} which should allow
 * the name to be used as a directory name on every supported OS.
 *
 * Returns whether the directory name is valid or not if elevel < ERROR.
 */
bool ValidateName(const char* name)
{
    if (name == NULL) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_NAME), errmsg("replication slot name should not be NULL.")));
    }

    if (strlen(name) == 0) {
        ereport(ERROR, (errcode(ERRCODE_INVALID_NAME), errmsg("replication slot name \"%s\" is too short", name)));
        return false;
    }

    if (strlen(name) >= NAMEDATALEN - 1) {
        ereport(ERROR, (errcode(ERRCODE_NAME_TOO_LONG), errmsg("replication slot name \"%s\" is too long", name)));
    }
    const char* danger_character_list[] = {";", "`", "\\", "'", "\"", ">", "<", "&", "|", "!", "\n", NULL};
    int i = 0;

    for (i = 0; danger_character_list[i] != NULL; i++) {
        if (strstr(name, danger_character_list[i]) != NULL) {
            ereport(ERROR,
                (errcode(ERRCODE_INVALID_NAME),
                    errmsg("replication slot name \"%s\" contains invalid character", name),
                    errhint("Replication slot names may only contain letters, numbers and the underscore character.")));
            return false;
        }
    }

    return true;
}

/*
 * Create a new replication slot and mark it as used by this backend.
 *
 * name: Name of the slot
 * db_specific: logical decoding is db specific; if the slot is going to
 *     be used for that pass true, otherwise false.
 */
void ReplicationSlotCreate(const char* name, ReplicationSlotPersistency persistency, bool isDummyStandby,
    Oid databaseId, XLogRecPtr restart_lsn)
{
    ReplicationSlot* slot = NULL;
    int i;
    errno_t rc = 0;

    Assert(t_thrd.slot_cxt.MyReplicationSlot == NULL);

    (void)ReplicationSlotValidateName(name, ERROR);

    /*
     * If some other backend ran this code currently with us, we'd likely
     * both allocate the same slot, and that would be bad.  We'd also be
     * at risk of missing a name collision.  Also, we don't want to try to
     * create a new slot while somebody's busy cleaning up an old one, because
     * we might both be monkeying with the same directory.
     */
    LWLockAcquire(ReplicationSlotAllocationLock, LW_EXCLUSIVE);

    /*
     * Check for name collision, and identify an allocatable slot.  We need
     * to hold ReplicationSlotControlLock in shared mode for this, so that
     * nobody else can change the in_use flags while we're looking at them.
     */
    LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
    for (i = 0; i < g_instance.attr.attr_storage.max_replication_slots; i++) {
        ReplicationSlot* s = &t_thrd.slot_cxt.ReplicationSlotCtl->replication_slots[i];

        if (s->in_use && strcmp(name, NameStr(s->data.name)) == 0) {
            LWLockRelease(ReplicationSlotControlLock);
            LWLockRelease(ReplicationSlotAllocationLock);
            if (databaseId == InvalidOid)
                /* For physical replication slot, report WARNING to let libpqrcv continue */
                ereport(WARNING,
                    (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("replication slot \"%s\" already exists", name)));
            else
                /* For logical replication slot, report ERROR followed PG9.4 */
                ereport(
                    ERROR, (errcode(ERRCODE_DUPLICATE_OBJECT), errmsg("replication slot \"%s\" already exists", name)));
            ReplicationSlotAcquire(name, isDummyStandby);
            return;
        }
        if (!s->in_use && slot == NULL)
            slot = s;
    }
    /* If all slots are in use, we're out of luck. */
    if (slot == NULL) {
        for (i = 0; i < g_instance.attr.attr_storage.max_replication_slots; i++) {
            ReplicationSlot* s = &t_thrd.slot_cxt.ReplicationSlotCtl->replication_slots[i];

            if (s->in_use) {
                ereport(LOG, (errmsg("Slot Name: %s", s->data.name.data)));
            }
        }

        LWLockRelease(ReplicationSlotControlLock);
        ereport(ERROR,
            (errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED),
                errmsg("all replication slots are in use"),
                errhint("Free one or increase max_replication_slots.")));
    }

    LWLockRelease(ReplicationSlotControlLock);

    /*
     * Since this slot is not in use, nobody should be looking at any
     * part of it other than the in_use field unless they're trying to allocate
     * it.  And since we hold ReplicationSlotAllocationLock, nobody except us
     * can be doing that.  So it's safe to initialize the slot.
     */
    Assert(!slot->in_use);
    slot->data.persistency = persistency;
    slot->data.xmin = InvalidTransactionId;
    slot->effective_xmin = InvalidTransactionId;
    rc = strncpy_s(NameStr(slot->data.name), NAMEDATALEN, name, NAMEDATALEN - 1);
    securec_check(rc, "\0", "\0");
    NameStr(slot->data.name)[NAMEDATALEN - 1] = '\0';
    slot->data.database = databaseId;
    slot->data.restart_lsn = restart_lsn;
    slot->data.isDummyStandby = isDummyStandby;

    /*
     * Create the slot on disk.  We haven't actually marked the slot allocated
     * yet, so no special cleanup is required if this errors out.
     */
    CreateSlotOnDisk(slot);

    /*
     * We need to briefly prevent any other backend from iterating over the
     * slots while we flip the in_use flag. We also need to set the active
     * flag while holding the ControlLock as otherwise a concurrent
     * SlotAcquire() could acquire the slot as well.
     */
    LWLockAcquire(ReplicationSlotControlLock, LW_EXCLUSIVE);

    slot->in_use = true;

    /* We can now mark the slot active, and that makes it our slot. */
    {
        volatile ReplicationSlot* vslot = slot;

        SpinLockAcquire(&slot->mutex);
        vslot->active = true;
        SpinLockRelease(&slot->mutex);
        t_thrd.slot_cxt.MyReplicationSlot = slot;
    }

    LWLockRelease(ReplicationSlotControlLock);

    /*
     * Now that the slot has been marked as in_use and in_active, it's safe to
     * let somebody else try to allocate a slot.
     */
    LWLockRelease(ReplicationSlotAllocationLock);
}

/*
 * Find a previously created slot and mark it as used by this backend.
 */
void ReplicationSlotAcquire(const char* name, bool isDummyStandby)
{
    ReplicationSlot* slot = NULL;
    int i;
    bool active = false;

    Assert(t_thrd.slot_cxt.MyReplicationSlot == NULL);

    ReplicationSlotValidateName(name, ERROR);

    /* Search for the named slot and mark it active if we find it. */
    LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
    for (i = 0; i < g_instance.attr.attr_storage.max_replication_slots; i++) {
        ReplicationSlot* s = &t_thrd.slot_cxt.ReplicationSlotCtl->replication_slots[i];

        if (s->in_use && strcmp(name, NameStr(s->data.name)) == 0) {
            volatile ReplicationSlot* vslot = s;

            SpinLockAcquire(&s->mutex);
            active = vslot->active;
            vslot->active = true;
            SpinLockRelease(&s->mutex);
            slot = s;
            break;
        }
    }
    LWLockRelease(ReplicationSlotControlLock);

    /* If we did not find the slot or it was already active, error out. */
    if (slot == NULL)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("replication slot \"%s\" does not exist", name)));
    if (active) {
        if (slot->data.database != InvalidOid || isDummyStandby != slot->data.isDummyStandby)
            ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("replication slot \"%s\" is already active", name)));
        else {
            ereport(
                WARNING, (errcode(ERRCODE_OBJECT_IN_USE), errmsg("replication slot \"%s\" is already active", name)));
        }
    }
    if (slot->data.database != InvalidOid) {
        slot->candidate_restart_lsn = InvalidXLogRecPtr;
        slot->candidate_restart_valid = InvalidXLogRecPtr;
        slot->candidate_xmin_lsn = InvalidXLogRecPtr;
        slot->candidate_catalog_xmin = InvalidTransactionId;
    }

    /* We made this slot active, so it's ours now. */
    t_thrd.slot_cxt.MyReplicationSlot = slot;
}

/*
 * Find out if we have a slot by slot name
 */
bool ReplicationSlotFind(const char* name)
{
    bool hasSlot = false;
    ReplicationSlotValidateName(name, ERROR);

    /* Search for the named slot and mark it active if we find it. */
    LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
    for (int i = 0; i < g_instance.attr.attr_storage.max_replication_slots; i++) {
        ReplicationSlot* s = &t_thrd.slot_cxt.ReplicationSlotCtl->replication_slots[i];
        if (s->in_use && strcmp(name, NameStr(s->data.name)) == 0) {
            hasSlot = true;
            break;
        }
    }
    LWLockRelease(ReplicationSlotControlLock);
    return hasSlot;
}

/*
 * Release a replication slot, this or another backend can ReAcquire it
 * later. Resources this slot requires will be preserved.
 */
void ReplicationSlotRelease(void)
{
    ReplicationSlot* slot = t_thrd.slot_cxt.MyReplicationSlot;

    if (slot == NULL || !slot->active) {
        t_thrd.slot_cxt.MyReplicationSlot = NULL;
        return;
    }

    if (slot->data.persistency == RS_EPHEMERAL) {
        /*
         * Delete the slot. There is no !PANIC case where this is allowed to
         * fail, all that may happen is an incomplete cleanup of the on-disk
         * data.
         */
        ReplicationSlotDropAcquired();
    } else {
        /* Mark slot inactive.  We're not freeing it, just disconnecting. */ volatile ReplicationSlot* vslot = slot;
        SpinLockAcquire(&slot->mutex);
        vslot->active = false;
        SpinLockRelease(&slot->mutex);
    }

    /*
     * If slot needed to temporarily restrain both data and catalog xmin to
     * create the catalog snapshot, remove that temporary constraint.
     * Snapshots can only be exported while the initial snapshot is still
     * acquired.
     */
    if (!TransactionIdIsValid(slot->data.xmin) && TransactionIdIsValid(slot->effective_xmin)) {
        SpinLockAcquire(&slot->mutex);
        slot->effective_xmin = InvalidTransactionId;
        SpinLockRelease(&slot->mutex);
        ReplicationSlotsComputeRequiredXmin(false);
    }

    t_thrd.slot_cxt.MyReplicationSlot = NULL;
    /* might not have been set when we've been a plain slot */
    LWLockAcquire(ProcArrayLock, LW_EXCLUSIVE);
    t_thrd.pgxact->vacuumFlags &= ~PROC_IN_LOGICAL_DECODING;
    LWLockRelease(ProcArrayLock);
}

/*
 * Permanently drop replication slot identified by the passed in name.
 */
void ReplicationSlotDrop(const char* name)
{
    bool isLogical = false;

    (void)ReplicationSlotValidateName(name, ERROR);

    /*
     * If some other backend ran this code currently with us, we might both
     * try to free the same slot at the same time.  Or we might try to delete
     * a slot with a certain name while someone else was trying to create a
     * slot with the same name.
     */
    Assert(t_thrd.slot_cxt.MyReplicationSlot == NULL);

    ReplicationSlotAcquire(name, false);
    isLogical = (t_thrd.slot_cxt.MyReplicationSlot->data.database != InvalidOid);
    ReplicationSlotDropAcquired();
    if (PMstateIsRun() && !RecoveryInProgress() && isLogical) {
        log_slot_drop(name);
    }
}
/*
 * Permanently drop the currently acquired replication slot which will be
 * released by the point this function returns.
 */
static void ReplicationSlotDropAcquired(void)
{
    char path[MAXPGPATH];
    char tmppath[MAXPGPATH];
    ReplicationSlot* slot = t_thrd.slot_cxt.MyReplicationSlot;

    Assert(t_thrd.slot_cxt.MyReplicationSlot != NULL);
    /* slot isn't acquired anymore */
    t_thrd.slot_cxt.MyReplicationSlot = NULL;

    /*
     * If some other backend ran this code concurrently with us, we might try
     * to delete a slot with a certain name while someone else was trying to
     * create a slot with the same name.
     */
    LWLockAcquire(ReplicationSlotAllocationLock, LW_EXCLUSIVE);

    /* Generate pathnames. */
    int nRet = snprintf_s(path, sizeof(path), MAXPGPATH - 1, "pg_replslot/%s", NameStr(slot->data.name));
    securec_check_ss(nRet, "\0", "\0");

    nRet = snprintf_s(tmppath, sizeof(tmppath), MAXPGPATH - 1, "pg_replslot/%s.tmp", NameStr(slot->data.name));
    securec_check_ss(nRet, "\0", "\0");

    /*
* Rename the slot directory on disk, so that we'll no longer recognize
* this as a valid slot.  Note that if this fails, we've got to mark the
* slot inactive before bailing out.  If we're dropping a ephemeral slot,
* we better never fail hard as the caller won't expect the slot to
* survive and this might get called during error handling.

 */
    if (rename(path, tmppath) == 0) {
        /*
         * We need to fsync() the directory we just renamed and its parent to
         * make sure that our changes are on disk in a crash-safe fashion.  If
         * fsync() fails, we can't be sure whether the changes are on disk or
         * not.  For now, we handle that by panicking;
         * StartupReplicationSlots() will try to straighten it out after
         * restart.
         */
        START_CRIT_SECTION();
        fsync_fname(tmppath, true);
        fsync_fname("pg_replslot", true);
        END_CRIT_SECTION();
    } else {
        volatile ReplicationSlot* vslot = slot;

        bool fail_softly = slot->data.persistency == RS_EPHEMERAL;
        SpinLockAcquire(&slot->mutex);
        vslot->active = false;
        SpinLockRelease(&slot->mutex);

        ereport(fail_softly ? WARNING : ERROR,
            (errcode_for_file_access(), errmsg("could not rename \"%s\" to \"%s\": %m", path, tmppath)));
    }

    /*
     * The slot is definitely gone.  Lock out concurrent scans of the array
     * long enough to kill it.  It's OK to clear the active flag here without
     * grabbing the mutex because nobody else can be scanning the array here,
     * and nobody can be attached to this slot and thus access it without
     * scanning the array.
     */
    LWLockAcquire(ReplicationSlotControlLock, LW_EXCLUSIVE);
    slot->active = false;
    slot->in_use = false;
    LWLockRelease(ReplicationSlotControlLock);

    /*
     * Slot is dead and doesn't prevent resource removal anymore, recompute
     * limits.
     */
    ReplicationSlotsComputeRequiredXmin(false);
    ReplicationSlotsComputeRequiredLSN(NULL);

    /*
     * If removing the directory fails, the worst thing that will happen is
     * that the user won't be able to create a new slot with the same name
     * until the next server restart.  We warn about it, but that's all.
     */
    if (!rmtree(tmppath, true))
        ereport(WARNING, (errcode_for_file_access(), errmsg("could not remove directory \"%s\"", tmppath)));

    /*
     * We release this at the very end, so that nobody starts trying to create
     * a slot while we're still cleaning up the detritus of the old one.
     */
    LWLockRelease(ReplicationSlotAllocationLock);
}

/*
 * Serialize the currently acquired slot's state from memory to disk, thereby
 * guaranteeing the current state will survive a crash.
 */
void ReplicationSlotSave(void)
{
    char path[MAXPGPATH];
    int nRet = 0;

    Assert(t_thrd.slot_cxt.MyReplicationSlot != NULL);

    nRet = snprintf_s(
        path, MAXPGPATH, MAXPGPATH - 1, "pg_replslot/%s", NameStr(t_thrd.slot_cxt.MyReplicationSlot->data.name));
    securec_check_ss(nRet, "\0", "\0");
    if (unlikely(CheckFileExists(path) == FILE_NOT_EXIST)) {
        CreateSlotOnDisk(t_thrd.slot_cxt.MyReplicationSlot);
    }

    SaveSlotToPath(t_thrd.slot_cxt.MyReplicationSlot, path, ERROR);
}

/*
 * Signal that it would be useful if the currently acquired slot would be
 * flushed out to disk.
 *
 * Note that the actual flush to disk can be delayed for a long time, if
 * required for correctness explicitly do a ReplicationSlotSave().
 */
void ReplicationSlotMarkDirty(void)
{
    Assert(t_thrd.slot_cxt.MyReplicationSlot != NULL);

    {
        volatile ReplicationSlot* vslot = t_thrd.slot_cxt.MyReplicationSlot;

        SpinLockAcquire(&vslot->mutex);
        t_thrd.slot_cxt.MyReplicationSlot->just_dirtied = true;
        t_thrd.slot_cxt.MyReplicationSlot->dirty = true;
        SpinLockRelease(&vslot->mutex);
    }
}

/*
 * Set dummy standby replication slot's lsn invalid
 */
void SetDummyStandbySlotLsnInvalid(void)
{
    Assert(t_thrd.slot_cxt.MyReplicationSlot != NULL);

    volatile ReplicationSlot* vslot = t_thrd.slot_cxt.MyReplicationSlot;

    Assert(vslot->data.isDummyStandby);

    if (!XLByteEQ(vslot->data.restart_lsn, InvalidXLogRecPtr)) {
        SpinLockAcquire(&vslot->mutex);
        vslot->data.restart_lsn = 0;
        SpinLockRelease(&vslot->mutex);

        ReplicationSlotMarkDirty();
        ReplicationSlotsComputeRequiredLSN(NULL);
    }
}

/*
 * Convert a slot that's marked as RS_DROP_ON_ERROR to a RS_PERSISTENT slot,
 * guaranteeing it will be there after a eventual crash.
 */
void ReplicationSlotPersist(void)
{
    ReplicationSlot* slot = t_thrd.slot_cxt.MyReplicationSlot;

    Assert(slot != NULL);
    Assert(slot->data.persistency != RS_PERSISTENT);
    LWLockAcquire(LogicalReplicationSlotPersistentDataLock, LW_EXCLUSIVE);

    {
        volatile ReplicationSlot* vslot = slot;

        SpinLockAcquire(&slot->mutex);
        vslot->data.persistency = RS_PERSISTENT;
        SpinLockRelease(&slot->mutex);
    }
    LWLockRelease(LogicalReplicationSlotPersistentDataLock);

    ReplicationSlotMarkDirty();
    ReplicationSlotSave();
}

/*
 * Compute the oldest xmin across all slots and store it in the ProcArray.
 *
 * If already_locked is true, ProcArrayLock has already been acquired
 * exclusively.
 */
void ReplicationSlotsComputeRequiredXmin(bool already_locked)
{
    int i;
    TransactionId agg_xmin = InvalidTransactionId;
    TransactionId agg_catalog_xmin = InvalidTransactionId;

    Assert(t_thrd.slot_cxt.ReplicationSlotCtl != NULL);
    LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

    for (i = 0; i < g_instance.attr.attr_storage.max_replication_slots; i++) {
        ReplicationSlot* s = &t_thrd.slot_cxt.ReplicationSlotCtl->replication_slots[i];
        TransactionId effective_xmin;
        TransactionId effective_catalog_xmin;

        if (!s->in_use)
            continue;

        {
            volatile ReplicationSlot* vslot = s;

            SpinLockAcquire(&s->mutex);
            effective_xmin = vslot->effective_xmin;
            effective_catalog_xmin = vslot->effective_catalog_xmin;
            SpinLockRelease(&s->mutex);
        }

        /* check the data xmin */
        if (TransactionIdIsValid(effective_xmin) &&
            (!TransactionIdIsValid(agg_xmin) || TransactionIdPrecedes(effective_xmin, agg_xmin)))
            agg_xmin = effective_xmin;
        /* check the catalog xmin */
        if (TransactionIdIsValid(effective_catalog_xmin) &&
            (!TransactionIdIsValid(agg_catalog_xmin) ||
                TransactionIdPrecedes(effective_catalog_xmin, agg_catalog_xmin)))
            agg_catalog_xmin = effective_catalog_xmin;
    }
    LWLockRelease(ReplicationSlotControlLock);

    ProcArraySetReplicationSlotXmin(agg_xmin, agg_catalog_xmin, already_locked);
}

/*
 * Compute the oldest restart LSN across all slots and inform xlog module.
 */
void ReplicationSlotsComputeRequiredLSN(ReplicationSlotState* repl_slt_state)
{
    int i;
    XLogRecPtr min_required = InvalidXLogRecPtr;
    XLogRecPtr max_required = InvalidXLogRecPtr;
    bool in_use = false;

    if (g_instance.attr.attr_storage.max_replication_slots == 0) {
        return;
    }

    Assert(t_thrd.slot_cxt.ReplicationSlotCtl != NULL);
    /* server_mode must be set before computing LSN */
    load_server_mode();

    LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
    for (i = 0; i < g_instance.attr.attr_storage.max_replication_slots; i++) {
        ReplicationSlot* s = &t_thrd.slot_cxt.ReplicationSlotCtl->replication_slots[i];
        volatile ReplicationSlot* vslot = s;
        SpinLockAcquire(&s->mutex);
        XLogRecPtr restart_lsn;

        if (t_thrd.xlog_cxt.server_mode != PRIMARY_MODE && t_thrd.xlog_cxt.server_mode != PENDING_MODE &&
            s->data.database == InvalidOid) {
            goto lock_release;
        }

        if (!s->in_use) {
            goto lock_release;
        }

        {
            in_use = true;
            restart_lsn = vslot->data.restart_lsn;
            SpinLockRelease(&s->mutex);
        }

        if ((!XLByteEQ(restart_lsn, InvalidXLogRecPtr)) &&
            (XLByteEQ(min_required, InvalidXLogRecPtr) || XLByteLT(restart_lsn, min_required))) {
            min_required = restart_lsn;
        }

        if (XLByteLT(max_required, restart_lsn)) {
            max_required = restart_lsn;
        }
        continue;
    lock_release:
        SpinLockRelease(&s->mutex);
    }
    LWLockRelease(ReplicationSlotControlLock);

    XLogSetReplicationSlotMinimumLSN(min_required);
    XLogSetReplicationSlotMaximumLSN(max_required);
    if (repl_slt_state != NULL) {
        repl_slt_state->min_required = min_required;
        repl_slt_state->max_required = max_required;
        repl_slt_state->exist_in_use = in_use;
    }
}

/*
 * Report the restart LSN in replication slots.
 */
void ReplicationSlotReportRestartLSN(void)
{
    int i;

    if (g_instance.attr.attr_storage.max_replication_slots == 0)
        return;

    Assert(t_thrd.slot_cxt.ReplicationSlotCtl != NULL);

    LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
    for (i = 0; i < g_instance.attr.attr_storage.max_replication_slots; i++) {
        volatile ReplicationSlot* s = &t_thrd.slot_cxt.ReplicationSlotCtl->replication_slots[i];

        if (!s->in_use)
            continue;

        ereport(LOG,
            (errmsg("slotname: %s, dummy: %d, restartlsn: %X/%X",
                NameStr(s->data.name),
                s->data.isDummyStandby,
                (uint32)(s->data.restart_lsn >> 32),
                (uint32)s->data.restart_lsn)));
    }
    LWLockRelease(ReplicationSlotControlLock);
}

/*
 * Compute the oldest WAL LSN required by *logical* decoding slots..
 *
 * Returns InvalidXLogRecPtr if logical decoding is disabled or no logicals
 * slots exist.
 *
 * NB: this returns a value >= ReplicationSlotsComputeRequiredLSN(), since it
 * ignores physical replication slots.
 *
 * The results aren't required frequently, so we don't maintain a precomputed
 * value like we do for ComputeRequiredLSN() and ComputeRequiredXmin().
 */
XLogRecPtr ReplicationSlotsComputeLogicalRestartLSN(void)
{
    XLogRecPtr result = InvalidXLogRecPtr;
    int i;

    if (g_instance.attr.attr_storage.max_replication_slots <= 0)
        return InvalidXLogRecPtr;

    LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);

    for (i = 0; i < g_instance.attr.attr_storage.max_replication_slots; i++) {
        ReplicationSlot* s = NULL;
        XLogRecPtr restart_lsn;

        s = &t_thrd.slot_cxt.ReplicationSlotCtl->replication_slots[i];

        /* cannot change while ReplicationSlotCtlLock is held */
        if (!s->in_use)
            continue;

        /* we're only interested in logical slots */
        if (s->data.database == InvalidOid)
            continue;

        /* read once, it's ok if it increases while we're checking */
        SpinLockAcquire(&s->mutex);
        restart_lsn = s->data.restart_lsn;
        SpinLockRelease(&s->mutex);

        if (XLByteEQ(result, InvalidXLogRecPtr) || XLByteLT(restart_lsn, result))

            result = restart_lsn;
    }

    LWLockRelease(ReplicationSlotControlLock);

    return result;
}

/*
 * ReplicationSlotsCountDBSlots -- count the number of slots that refer to the
 * passed database oid.
 *
 * Returns true if there are any slots referencing the database. *nslots will
 * be set to the absolute number of slots in the database, *nactive to ones
 * currently active.
 */
bool ReplicationSlotsCountDBSlots(Oid dboid, int* nslots, int* nactive)
{
    int i;

    *nslots = *nactive = 0;

    if (g_instance.attr.attr_storage.max_replication_slots <= 0)
        return false;

    LWLockAcquire(ReplicationSlotControlLock, LW_SHARED);
    for (i = 0; i < g_instance.attr.attr_storage.max_replication_slots; i++) {
        volatile ReplicationSlot* s = NULL;

        s = &t_thrd.slot_cxt.ReplicationSlotCtl->replication_slots[i];

        /* cannot change while ReplicationSlotCtlLock is held */
        if (!s->in_use)
            continue;

        /* not database specific, skip */
        if (s->data.database == InvalidOid)
            continue;

        /* not our database, skip */
        if (s->data.database != dboid)
            continue;

        /* count slots with spinlock held */
        SpinLockAcquire(&s->mutex);
        (*nslots)++;
        if (s->active)
            (*nactive)++;
        SpinLockRelease(&s->mutex);
    }
    LWLockRelease(ReplicationSlotControlLock);

    if (*nslots > 0) {
        return true;
    }
    return false;
}

/*
 * Check whether the server's configuration supports using replication
 * slots.
 */
void CheckSlotRequirements(void)
{
    if (g_instance.attr.attr_storage.max_replication_slots == 0)
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                (errmsg("replication slots can only be used if max_replication_slots > 0"))));

    if (g_instance.attr.attr_storage.wal_level < WAL_LEVEL_ARCHIVE)
        ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                errmsg("replication slots can only be used if wal_level >= archive")));
}

/*
 * Returns whether the string `str' has the postfix `end'.
 */
static bool string_endswith(const char* str, const char* end)
{
    size_t slen = strlen(str);
    size_t elen = strlen(end);
    /* can't be a postfix if longer */
    if (elen > slen)
        return false;

    /* compare the end of the strings */
    str += slen - elen;
    return strcmp(str, end) == 0;
}

/*
 * Flush all replication slots to disk.
 *
 * This needn't actually be part of a checkpoint, but it's a convenient
 * location.
 */
void CheckPointReplicationSlots(void)
{
    int i;
    int nRet = 0;

    ereport(DEBUG1, (errmsg("performing replication slot checkpoint")));

    /*
     * Prevent any slot from being created/dropped while we're active. As we
     * explicitly do *not* want to block iterating over replication_slots or
     * acquiring a slot we cannot take the control lock - but that's OK,
     * because holding ReplicationSlotAllocationLock is strictly stronger,
     * and enough to guarantee that nobody can change the in_use bits on us.
     */
    LWLockAcquire(ReplicationSlotAllocationLock, LW_SHARED);

    for (i = 0; i < g_instance.attr.attr_storage.max_replication_slots; i++) {
        ReplicationSlot* s = &t_thrd.slot_cxt.ReplicationSlotCtl->replication_slots[i];
        char path[MAXPGPATH];

        if (!s->in_use)
            continue;

        /* save the slot to disk, locking is handled in SaveSlotToPath() */
        nRet = snprintf_s(path, MAXPGPATH, MAXPGPATH - 1, "pg_replslot/%s", NameStr(s->data.name));
        securec_check_ss(nRet, "\0", "\0");

        if (unlikely(CheckFileExists(path) == FILE_NOT_EXIST)) {
            CreateSlotOnDisk(s);
        }
        SaveSlotToPath(s, path, LOG);
    }
    LWLockRelease(ReplicationSlotAllocationLock);
}

/*
 * Load all replication slots from disk into memory at server startup. This
 * needs to be run before we start crash recovery.
 */
void StartupReplicationSlots()
{
    DIR* replication_dir = NULL;
    struct dirent* replication_de = NULL;
    int nRet = 0;

    ereport(DEBUG1, (errmsg("starting up replication slots")));

    /* restore all slots by iterating over all on-disk entries */
    replication_dir = AllocateDir("pg_replslot");
    if (replication_dir == NULL) {
        char tmppath[MAXPGPATH];

        nRet = snprintf_s(tmppath, sizeof(tmppath), MAXPGPATH - 1, "%s", "pg_replslot");
        securec_check_ss(nRet, "\0", "\0");

        if (mkdir(tmppath, S_IRWXU) < 0)
            ereport(ERROR, (errcode_for_file_access(), errmsg("could not create directory \"%s\": %m", tmppath)));
        fsync_fname(tmppath, true);
        return;
    }
    while ((replication_de = ReadDir(replication_dir, "pg_replslot")) != NULL) {
        struct stat statbuf;
        char path[MAXPGPATH];

        if (strcmp(replication_de->d_name, ".") == 0 || strcmp(replication_de->d_name, "..") == 0)
            continue;

        nRet = snprintf_s(path, sizeof(path), MAXPGPATH - 1, "pg_replslot/%s", replication_de->d_name);
        securec_check_ss(nRet, "\0", "\0");

        /* we're only creating directories here, skip if it's not our's */
        if (lstat(path, &statbuf) == 0 && !S_ISDIR(statbuf.st_mode))
            continue;

        /* we crashed while a slot was being setup or deleted, clean up */
        if (string_endswith(replication_de->d_name, ".tmp")) {
            if (!rmtree(path, true)) {
                ereport(WARNING, (errcode_for_file_access(), errmsg("could not remove directory \"%s\"", path)));
                continue;
            }
            fsync_fname("pg_replslot", true);
            continue;
        }

        /* looks like a slot in a normal state, restore */
        RestoreSlotFromDisk(replication_de->d_name);
    }
    FreeDir(replication_dir);

    /* currently no slots exist, we're done. */
    if (g_instance.attr.attr_storage.max_replication_slots <= 0) {
        return;
    }

    /* Now that we have recovered all the data, compute replication xmin */
    ReplicationSlotsComputeRequiredXmin(false);
    ReplicationSlotsComputeRequiredLSN(NULL);
}

/* ----
 * Manipulation of ondisk state of replication slots
 *
 * NB: none of the routines below should take any notice whether a slot is the
 * current one or not, that's all handled a layer above.
 * ----
 */
void CreateSlotOnDisk(ReplicationSlot* slot)
{
    char tmppath[MAXPGPATH];
    char path[MAXPGPATH];
    struct stat st;
    int nRet = 0;

    /*
     * No need to take out the io_in_progress_lock, nobody else can see this
     * slot yet, so nobody else will write. We're reusing SaveSlotToPath which
     * takes out the lock, if we'd take the lock here, we'd deadlock.
     */
    nRet = snprintf_s(path, sizeof(path), MAXPGPATH - 1, "pg_replslot/%s", NameStr(slot->data.name));
    securec_check_ss(nRet, "\0", "\0");

    nRet = snprintf_s(tmppath, sizeof(tmppath), MAXPGPATH - 1, "pg_replslot/%s.tmp", NameStr(slot->data.name));
    securec_check_ss(nRet, "\0", "\0");

    /*
     * It's just barely possible that some previous effort to create or
     * drop a slot with this name left a temp directory lying around.
     * If that seems to be the case, try to remove it.  If the rmtree()
     * fails, we'll error out at the mkdir() below, so we don't bother
     * checking success.
     */
    if (stat(tmppath, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (!rmtree(tmppath, true)) {
            ereport(ERROR, (errcode_for_file_access(), 
                errmsg("could not rm directory \"%s\": %m", tmppath)));
        }
    }

    /* Create and fsync the temporary slot directory. */
    if (mkdir(tmppath, S_IRWXU) < 0)
        ereport(ERROR, (errcode_for_file_access(),
            errmsg("could not create directory \"%s\": %m", tmppath)));
    fsync_fname(tmppath, true);

    /* Write the actual state file. */
    slot->dirty = true; /* signal that we really need to write */
    SaveSlotToPath(slot, tmppath, ERROR);

    /* Rename the directory into place. */
    if (rename(tmppath, path) != 0)
        ereport(ERROR, (errcode_for_file_access(),
            errmsg("could not rename file \"%s\" to \"%s\": %m", tmppath, path)));

    /*
     * If we'd now fail - really unlikely - we wouldn't know whether this slot
     * would persist after an OS crash or not - so, force a restart. The
     * restart would try to fysnc this again till it works.
     */
    START_CRIT_SECTION();

    fsync_fname(path, true);
    fsync_fname("pg_replslot", true);

    END_CRIT_SECTION();

    if (!RecoveryInProgress())
        ereport(LOG, (errcode_for_file_access(),
            errmsg("create slot \"%s\" on disk successfully", path)));
}

/*
 * Shared functionality between saving and creating a replication slot.
 */
static void SaveSlotToPath(ReplicationSlot* slot, const char* dir, int elevel)
{
    char tmppath[MAXPGPATH];
    char path[MAXPGPATH];
    const int STATE_FILE_NUM = 2;
    char* fname[STATE_FILE_NUM];
    int fd;
    ReplicationSlotOnDisk cp;
    bool was_dirty = false;
    errno_t rc = EOK;

    /* first check whether there's something to write out */
    {
        volatile ReplicationSlot* vslot = slot;

        SpinLockAcquire(&vslot->mutex);
        was_dirty = vslot->dirty;
        vslot->just_dirtied = false;
        SpinLockRelease(&vslot->mutex);
    }

    /* and don't do anything if there's nothing to write */
    if (!was_dirty) {
        return;
    }

    LWLockAcquire(slot->io_in_progress_lock, LW_EXCLUSIVE);

    /* silence valgrind :( */
    rc = memset_s(&cp, sizeof(ReplicationSlotOnDisk), 0, sizeof(ReplicationSlotOnDisk));
    securec_check(rc, "\0", "\0");

    fname[0] = "%s/state.backup";
    fname[1] = "%s/state.tmp";

    rc = snprintf_s(path, MAXPGPATH, MAXPGPATH - 1, "%s/state", dir);
    securec_check_ss(rc, "\0", "\0");

    for (int i = 0; i < STATE_FILE_NUM; i++) {
        rc = snprintf_s(tmppath, MAXPGPATH, MAXPGPATH - 1, fname[i], dir);
        securec_check_ss(rc, "\0", "\0");

        fd = BasicOpenFile(tmppath, O_CREAT | O_WRONLY | PG_BINARY, S_IRUSR | S_IWUSR);
        if (fd < 0) {
            LWLockRelease(slot->io_in_progress_lock);
            ereport(elevel, (errcode_for_file_access(), errmsg("could not create file \"%s\": %m", tmppath)));
            return;
        }

        cp.magic = SLOT_MAGIC;
        INIT_CRC32C(cp.checksum);
        cp.version = 1;
        cp.length = ReplicationSlotOnDiskDynamicSize;

        SpinLockAcquire(&slot->mutex);

        rc = memcpy_s(
            &cp.slotdata, sizeof(ReplicationSlotPersistentData), &slot->data, sizeof(ReplicationSlotPersistentData));
        securec_check(rc, "\0", "\0");

        SpinLockRelease(&slot->mutex);

        COMP_CRC32C(cp.checksum, (char*)(&cp) + ReplicationSlotOnDiskConstantSize, ReplicationSlotOnDiskDynamicSize);
        FIN_CRC32C(cp.checksum);

        /* Causing errno to potentially come from a previous system call. */
        errno = 0;
        pgstat_report_waitevent(WAIT_EVENT_REPLICATION_SLOT_WRITE);
        if ((write(fd, &cp, (uint32)sizeof(cp))) != sizeof(cp)) {
            int save_errno = errno;
            pgstat_report_waitevent(WAIT_EVENT_END);
            if (close(fd)) {
                ereport(elevel, (errcode_for_file_access(), errmsg("could not close file \"%s\": %m", tmppath)));
            }
            /* if write didn't set errno, assume problem is no disk space */
            errno = save_errno ? save_errno : ENOSPC;
            LWLockRelease(slot->io_in_progress_lock);
            ereport(elevel, (errcode_for_file_access(), errmsg("could not write to file \"%s\": %m", tmppath)));
            return;
        }
        pgstat_report_waitevent(WAIT_EVENT_END);

        /* fsync the temporary file */
        pgstat_report_waitevent(WAIT_EVENT_REPLICATION_SLOT_SYNC);
        if (pg_fsync(fd) != 0) {
            int save_errno = errno;
            pgstat_report_waitevent(WAIT_EVENT_END);
            if (close(fd)) {
                ereport(elevel, (errcode_for_file_access(), errmsg("could not close file \"%s\": %m", tmppath)));
            }
            errno = save_errno;
            LWLockRelease(slot->io_in_progress_lock);
            ereport(elevel, (errcode_for_file_access(), errmsg("could not fsync file \"%s\": %m", tmppath)));
            return;
        }
        pgstat_report_waitevent(WAIT_EVENT_END);
        if (close(fd)) {
            ereport(elevel, (errcode_for_file_access(), errmsg("could not close file \"%s\": %m", tmppath)));
        }
    }

    /* rename to permanent file, fsync file and directory */
    if (rename(tmppath, path) != 0) {
        LWLockRelease(slot->io_in_progress_lock);
        ereport(elevel, (errcode_for_file_access(), errmsg("could not rename \"%s\" to \"%s\": %m", tmppath, path)));
        return;
    }

    /* Check CreateSlot() for the reasoning of using a crit. section. */
    START_CRIT_SECTION();

    fsync_fname(path, false);
    fsync_fname(dir, true);
    fsync_fname("pg_replslot", true);

    END_CRIT_SECTION();

    /*
     * Successfully wrote, unset dirty bit, unless somebody dirtied again
     * already.
     */
    {
        volatile ReplicationSlot* vslot = slot;

        SpinLockAcquire(&vslot->mutex);
        if (!vslot->just_dirtied)
            vslot->dirty = false;
        SpinLockRelease(&vslot->mutex);
    }

    LWLockRelease(slot->io_in_progress_lock);
}

/*
 * Load a single slot from disk into memory.
 */
static void RestoreSlotFromDisk(const char* name)
{
    ReplicationSlotOnDisk cp;
    int i;
    char path[MAXPGPATH];
    int fd;
    bool restored = false;
    int readBytes;
    pg_crc32c checksum;
    errno_t rc = EOK;
    int ret;
    bool ignore_bak = false;
    bool retry = false;

    /* no need to lock here, no concurrent access allowed yet
     *
     * delete temp file if it exists 
     */
    rc = snprintf_s(path, sizeof(path), MAXPGPATH - 1, "pg_replslot/%s/state.tmp", name);
    securec_check_ss(rc, "\0", "\0");

    ret = unlink(path);
    if (ret < 0 && errno != ENOENT)
        ereport(PANIC, (errcode_for_file_access(), errmsg("could not unlink file \"%s\": %m", path)));

    /* unlink backup file if rename failed */
    if (ret == 0) {
        rc = snprintf_s(path, sizeof(path), MAXPGPATH - 1, "pg_replslot/%s/state.backup", name);
        securec_check_ss(rc, "\0", "\0");
        if (unlink(path) < 0 && errno != ENOENT)
            ereport(PANIC, (errcode_for_file_access(), errmsg("could not unlink file \"%s\": %m", path)));
        ignore_bak = true;
    }

    rc = snprintf_s(path, sizeof(path), MAXPGPATH - 1, "pg_replslot/%s/state", name);
    securec_check_ss(rc, "\0", "\0");

    elog(DEBUG1, "restoring replication slot from \"%s\"", path);

loop:
    fd = BasicOpenFile(path, O_RDONLY | PG_BINARY, 0);

    /*
     * We do not need to handle this as we are rename()ing the directory into
     * place only after we fsync()ed the state file.
     */
    if (fd < 0)
        ereport(PANIC, (errcode_for_file_access(), errmsg("could not open file \"%s\": %m", path)));

    /*
     * Sync state file before we're reading from it. We might have crashed
     * while it wasn't synced yet and we shouldn't continue on that basis.
     */
    pgstat_report_waitevent(WAIT_EVENT_REPLICATION_SLOT_RESTORE_SYNC);
    if (pg_fsync(fd) != 0) {
        int save_errno = errno;
        if (close(fd)) {
            ereport(PANIC, (errcode_for_file_access(), errmsg("could not close file \"%s\": %m", path)));
        }
        errno = save_errno;
        ereport(PANIC, (errcode_for_file_access(), errmsg("could not fsync file \"%s\": %m", path)));
    }
    pgstat_report_waitevent(WAIT_EVENT_END);

    /* Also sync the parent directory */
    START_CRIT_SECTION();
    fsync_fname(path, true);
    END_CRIT_SECTION();

    /* read part of statefile that's guaranteed to be version independent */
    pgstat_report_waitevent(WAIT_EVENT_REPLICATION_SLOT_READ);

    errno = 0;
    /* read the whole state file */
    readBytes = read(fd, &cp, (uint32)sizeof(ReplicationSlotOnDisk));
    pgstat_report_waitevent(WAIT_EVENT_END);
    if (readBytes != sizeof(ReplicationSlotOnDisk)) {
        int saved_errno = errno;
        if (close(fd)) {
            ereport(PANIC, (errcode_for_file_access(), errmsg("could not close file \"%s\": %m", path)));
        }
        errno = saved_errno;
        ereport(PANIC,
            (errcode_for_file_access(),
                errmsg("could not read file \"%s\", read %d of %u: %m",
                    path,
                    readBytes,
                    (uint32)sizeof(ReplicationSlotOnDisk))));
    }
    if (close(fd)) {
        ereport(PANIC, (errcode_for_file_access(), errmsg("could not close file \"%s\": %m", path)));
    }

    /* now verify the CRC */
    INIT_CRC32C(checksum);
    COMP_CRC32C(checksum, (char*)&cp + ReplicationSlotOnDiskConstantSize, ReplicationSlotOnDiskDynamicSize);
    FIN_CRC32C(checksum);
    if (!EQ_CRC32C(checksum, cp.checksum)) {
        if (ignore_bak == false) {
            ereport(WARNING,
                (errmsg("replication slot file %s: checksum mismatch, is %u, should be %u, try backup file",
                    path,
                    checksum,
                    cp.checksum)));
            rc = snprintf_s(path, sizeof(path), MAXPGPATH - 1, "pg_replslot/%s/state.backup", name);
            securec_check_ss(rc, "\0", "\0");
            ignore_bak = true;
            retry = true;
            goto loop;
        } else {
            ereport(PANIC,
                (errmsg(
                    "replication slot file %s: checksum mismatch, is %u, should be %u", path, checksum, cp.checksum)));
        }
    }
    /* verify magic */
    if (cp.magic != SLOT_MAGIC) {
        if (ignore_bak == false) {
            ereport(WARNING,
                (errcode_for_file_access(),
                    errmsg("replication slot file \"%s\" has wrong magic %u instead of %d, try backup file",
                        path,
                        cp.magic,
                        SLOT_MAGIC)));
            rc = snprintf_s(path, sizeof(path), MAXPGPATH - 1, "pg_replslot/%s/state.backup", name);
            securec_check_ss(rc, "\0", "\0");
            ignore_bak = true;
            retry = true;
            goto loop;
        } else {
            ereport(PANIC,
                (errcode_for_file_access(),
                    errmsg(
                        "replication slot file \"%s\" has wrong magic %u instead of %d", path, cp.magic, SLOT_MAGIC)));
        }
    }
    /* boundary check on length */
    if (cp.length != ReplicationSlotOnDiskDynamicSize) {
        if (ignore_bak == false) {
            ereport(WARNING,
                (errcode_for_file_access(),
                    errmsg("replication slot file \"%s\" has corrupted length %u, try backup file", path, cp.length)));
            rc = snprintf_s(path, sizeof(path), MAXPGPATH - 1, "pg_replslot/%s/state.backup", name);
            securec_check_ss(rc, "\0", "\0");
            ignore_bak = true;
            retry = true;
            goto loop;
        } else {
            ereport(PANIC,
                (errcode_for_file_access(),
                    errmsg("replication slot file \"%s\" has corrupted length %u", path, cp.length)));
        }
    }

    /*
     * If we crashed with an ephemeral slot active, don't restore but delete it.
     */
    if (cp.slotdata.persistency != RS_PERSISTENT) {
        rc = snprintf_s(path, MAXPGPATH, MAXPGPATH - 1, "pg_replslot/%s", name);
        securec_check_ss(rc, "\0", "\0");
        if (!rmtree(path, true)) {
            ereport(WARNING, (errcode_for_file_access(), errmsg("could not remove directory \"%s\"", path)));
        }
        fsync_fname("pg_replslot", true);
        return;
    }

    if (retry == true)
        RecoverReplSlotFile(cp, name);

    /* nothing can be active yet, don't lock anything */
    for (i = 0; i < g_instance.attr.attr_storage.max_replication_slots; i++) {
        ReplicationSlot* slot = NULL;

        slot = &t_thrd.slot_cxt.ReplicationSlotCtl->replication_slots[i];

        if (slot->in_use)
            continue;

        /* restore the entire set of persistent data */
        rc = memcpy_s(
            &slot->data, sizeof(ReplicationSlotPersistentData), &cp.slotdata, sizeof(ReplicationSlotPersistentData));
        securec_check(rc, "\0", "\0");

        /* initialize in memory state */
        slot->effective_xmin = slot->data.xmin;
        slot->effective_catalog_xmin = slot->data.catalog_xmin;

        slot->candidate_catalog_xmin = InvalidTransactionId;
        slot->candidate_xmin_lsn = InvalidXLogRecPtr;
        slot->candidate_restart_lsn = InvalidXLogRecPtr;
        slot->candidate_restart_valid = InvalidXLogRecPtr;
        slot->in_use = true;
        slot->active = false;

        restored = true;
        break;
    }

    if (!restored)
        ereport(PANIC,
            (errmsg("too many replication slots active before shutdown"),
                errhint("Increase g_instance.attr.attr_storage.max_replication_slots and try again.")));
}

/*
 * when incorrect checksum is detected in slot file,
 * we should recover the slot file using the content of backup file
 */
static void RecoverReplSlotFile(const ReplicationSlotOnDisk& cp, const char* name)
{
    int fd;
    char path[MAXPGPATH];
    errno_t rc = EOK;

    rc = snprintf_s(path, sizeof(path), MAXPGPATH - 1, "pg_replslot/%s/state", name);
    securec_check_ss(rc, "\0", "\0");

    ereport(WARNING, (errmsg("recover the replication slot file %s", name)));

    fd = BasicOpenFile(path, O_TRUNC | O_WRONLY | PG_BINARY, S_IRUSR | S_IWUSR);
    if (fd < 0)
        ereport(PANIC, (errcode_for_file_access(), errmsg("recover failed could not open slot file \"%s\": %m", path)));

    errno = 0;
    if ((write(fd, &cp, sizeof(cp))) != sizeof(cp)) {
        /* if write didn't set errno, assume problem is no disk space */
        if (errno == 0)
            errno = ENOSPC;
        ereport(
            PANIC, (errcode_for_file_access(), errmsg("recover failed could not write to slot file \"%s\": %m", path)));
    }

    /* fsync the temporary file */
    if (pg_fsync(fd) != 0)
        ereport(
            PANIC, (errcode_for_file_access(), errmsg("recover failed could not fsync slot file \"%s\": %m", path)));

    if (close(fd))
        ereport(
            PANIC, (errcode_for_file_access(), errmsg("recover failed could not close slot file \"%s\": %m", path)));
}
/*
 * get cur nodes slotname
 *
 */
char* get_my_slot_name(void)
{
    ReplConnInfo* conninfo = NULL;
    errno_t retcode = EOK;

    /* local and local port is the same, so we choose the first one is ok */
    int repl_idx = 0;
    char* slotname = NULL;
    char* t_appilcation_name = NULL;
    slotname = (char*)palloc0(NAMEDATALEN);
    t_appilcation_name = get_application_name();
    /* get current repl conninfo, */
    conninfo = GetRepConnArray(&repl_idx);
    if (u_sess->attr.attr_storage.PrimarySlotName != NULL) {
        retcode = strncpy_s(slotname, NAMEDATALEN, u_sess->attr.attr_storage.PrimarySlotName, NAMEDATALEN - 1);
        securec_check(retcode, "\0", "\0");
    } else if (t_appilcation_name && strlen(t_appilcation_name) > 0) {
        int rc = 0;
        rc = snprintf_s(slotname, NAMEDATALEN, NAMEDATALEN - 1, "%s", t_appilcation_name);
        securec_check_ss(rc, "\0", "\0");
    } else if (g_instance.attr.attr_common.PGXCNodeName != NULL) {
        int rc = 0;
        if (IS_DN_DUMMY_STANDYS_MODE()) {
            rc = snprintf_s(slotname, NAMEDATALEN, NAMEDATALEN - 1, "%s", g_instance.attr.attr_common.PGXCNodeName);
        } else if (conninfo != NULL) {
            rc = snprintf_s(slotname,
                NAMEDATALEN,
                NAMEDATALEN - 1,
                "%s_%s_%d",
                g_instance.attr.attr_common.PGXCNodeName,
                conninfo->localhost,
                conninfo->localport);
        }
        securec_check_ss(rc, "\0", "\0");
    }
    pfree(t_appilcation_name);
    t_appilcation_name = NULL;
    return slotname;
}

/*
 * get the  application_name specified in postgresql.conf
 */
static char* get_application_name(void)
{
#define INVALID_LINES_IDX (int)(~0)
    char** optlines = NULL;
    int lines_index = 0;
    int optvalue_off;
    int optvalue_len;
    char arg_str[NAMEDATALEN] = {0};
    const char* config_para_build = "application_name";
    int rc;
    char conf_path[MAXPGPATH] = {0};
    char* trim_app_name = NULL;
    char* app_name = NULL;
    app_name = (char*)palloc0(MAXPGPATH);
    rc = snprintf_s(conf_path, MAXPGPATH, MAXPGPATH - 1, "%s/%s", t_thrd.proc_cxt.DataDir, "postgresql.conf");
    securec_check_ss_c(rc, "\0", "\0");

    if ((optlines = (char**)read_guc_file(conf_path)) != NULL) {
        lines_index = find_guc_option(optlines, config_para_build, NULL, NULL, &optvalue_off, &optvalue_len);
        if (lines_index != INVALID_LINES_IDX) {
            rc = strncpy_s(arg_str, NAMEDATALEN, optlines[lines_index] + optvalue_off, optvalue_len);
            securec_check_c(rc, "\0", "\0");
        }
        /* first free one-dimensional array memory in case memory leak */
        int i = 0;
        while (optlines[i] != NULL) {
            selfpfree(const_cast<char* >(optlines[i]));
            optlines[i] = NULL;
            i++;
        }
        selfpfree(optlines);
        optlines = NULL;
    } else {
        return app_name;
    }
    /* construct slotname */
    trim_app_name = trim_str(arg_str, NAMEDATALEN, '\'');
    if (trim_app_name != NULL) {
        rc = snprintf_s(app_name, NAMEDATALEN, NAMEDATALEN - 1, "%s", trim_app_name);
        securec_check_ss_c(rc, "\0", "\0");
        pfree(trim_app_name);
        trim_app_name = NULL;
    }
    return app_name;
}

/*
 * Get the string beside space or sep
 */
static char* trim_str(char* str, int str_len, char sep)
{
    int len;
    char* begin = NULL;
    char* end = NULL;
    char* cur = NULL;
    char* cpyStr = NULL;
    errno_t rc;

    if (str == NULL || str_len <= 0) {
        return NULL;
    }
    cpyStr = (char*)palloc(str_len);
    begin = str;
    while (begin != NULL && (isspace((int)*begin) || *begin == sep)) {
        begin++;
    }
    for (end = cur = begin; *cur != '\0'; cur++) {
        if (!isspace((int)*cur) && *cur != sep) {
            end = cur;
        }
    }
    if (*begin == '\0') {
        pfree(cpyStr);
        cpyStr = NULL;
        return NULL;
    }
    len = end - begin + 1;
    rc = memmove_s(cpyStr, (uint32)str_len, begin, len);
    securec_check_c(rc, "\0", "\0");
    cpyStr[len] = '\0';
    return cpyStr;
}