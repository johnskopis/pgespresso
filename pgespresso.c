/*-------------------------------------------------------------------------
 *
 * pgespresso.c
 *
 *
 * Copyright (c) 2014, 2ndQuadrant Limited <www.2ndquadrant.com>
 *
 * Authors: Simon Riggs <simon@2ndQuadrant.com>
 *          Marco Nenciarini <marco.nenciarini@2ndQuadrant.it>
 *          Gabriele Bartolini <gabriele.bartolini@2ndQuadrant.it>
 *
 * See COPYING for licensing information
 *
 * IDENTIFICATION
 *	  pgespresso/pgespresso.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/xlog.h"
#include "access/xlog_internal.h"
#include "access/xlogdefs.h"
#include "utils/builtins.h"
#include "miscadmin.h"

PG_MODULE_MAGIC;

Datum pgespresso_start_backup(PG_FUNCTION_ARGS);
Datum pgespresso_stop_backup(PG_FUNCTION_ARGS);
Datum pgespresso_abort_backup(PG_FUNCTION_ARGS);

PG_FUNCTION_INFO_V1(pgespresso_start_backup);
PG_FUNCTION_INFO_V1(pgespresso_stop_backup);
PG_FUNCTION_INFO_V1(pgespresso_abort_backup);

/*
 * pgespresso_start_backup: set up for taking an on-line backup dump
 *
 * Essentially what this does is to return a backup label file that the
 * user is responsible for placing in the $PGDATA of the backup AFTER
 * the backup has been taken.  The label file must not be written to the
 * data directory of the server from which the backup is taken because
 * this type of backup presumes and allows that more than one backup
 * may be in progress at any one time.  The label file
 * contains the user-supplied label string (typically this would be used
 * to tell where the backup dump will be stored) and the starting time and
 * starting WAL location for the dump.
 */
Datum
pgespresso_start_backup(PG_FUNCTION_ARGS)
{
	text	   *backupid = PG_GETARG_TEXT_P(0);
	bool		fast = PG_GETARG_BOOL(1);
	char	   *backupidstr;
	char       *labelfile;

	backupidstr = text_to_cstring(backupid);

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		   errmsg("must be superuser or replication role to run a backup")));

	/*
	 * ThisTimeLineID is always 0 in a normal backend during recovery.
	 * We get latest redo apply position timeline and we update it globally
	 * to make do_pg_start_backup use the correct value when generating
	 * the backup label text
	 */
	if (RecoveryInProgress()) {
		TimeLineID	replayTLI;

		GetXLogReplayRecPtr(&replayTLI);
		ThisTimeLineID = replayTLI;
		elog(DEBUG1, "updated ThisTimeLineID = %u", ThisTimeLineID);
	}

	/*
	 * Starting from 9.3 the do_pg_start_backup returns the timeline ID
	 * in *starttli_p additional argument
	 */
	#if PG_VERSION_NUM >= 90300
		do_pg_start_backup(backupidstr, fast, NULL, &labelfile);
	#else
		do_pg_start_backup(backupidstr, fast, &labelfile);
	#endif

	PG_RETURN_TEXT_P(cstring_to_text(labelfile));
}

/*
 * pgespresso_stop_backup: finish taking an on-line backup dump
 *
 * Only parameter is the labelfile returned from pg_start_concurrent_backup
 *
 * Return is the XLOG filename containing end of backup location, combining
 * both the TLI and the end location. NOTE: the user is responsible for
 * ensuring that the last file is correctly archived.
 */
Datum
pgespresso_stop_backup(PG_FUNCTION_ARGS)
{
	XLogRecPtr	stoppoint;
	text	   *labelfile = PG_GETARG_TEXT_P(0);
	char	   *backupidstr;
	char		xlogfilename[MAXFNAMELEN];

	backupidstr = text_to_cstring(labelfile);

	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		 (errmsg("must be superuser or replication role to run a backup"))));

	#if PG_VERSION_NUM >= 90300
	{
		XLogSegNo	xlogsegno;
		TimeLineID	endtli;

		stoppoint = do_pg_stop_backup(backupidstr,
									  false,  /* don't wait for archive */
									  &endtli);

		XLByteToPrevSeg(stoppoint, xlogsegno);
		XLogFileName(xlogfilename, endtli, xlogsegno);
	}
	#else
	{
		uint32		xlogid;
		uint32		xlogseg;

		stoppoint = do_pg_stop_backup(backupidstr,
									  false);  /* don't wait for archive */

		/*
		 * In 9.2 the do_pg_stop_backup doesn't return the timeline ID and
		 * ThisTimeLineID is always 0 in a normal backend during recovery.
		 * We get latest redo apply position timeline and we update it globally
		 */
		if (RecoveryInProgress()) {
			TimeLineID	replayTLI;

			GetXLogReplayRecPtr(&replayTLI);
			ThisTimeLineID = replayTLI;
			elog(DEBUG1, "updated ThisTimeLineID = %u", ThisTimeLineID);
		}

		XLByteToPrevSeg(stoppoint, xlogid, xlogseg);
		XLogFileName(xlogfilename, ThisTimeLineID, xlogid, xlogseg);
	}
	#endif

	PG_RETURN_TEXT_P(cstring_to_text(xlogfilename));
}

/*
 * pgespresso_abort_backup: abort a running backup
 *
 * This does just the most basic steps of pgespresso_stop_backup(), by taking
 * the system out of backup mode, thus making it a lot more safe to call from
 * an error handler.
 */
Datum
pgespresso_abort_backup(PG_FUNCTION_ARGS)
{
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
		 (errmsg("must be superuser or replication role to run a backup"))));

	do_pg_abort_backup();

	PG_RETURN_VOID();
}
