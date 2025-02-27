/*
 * ptrack.c
 *		Block level incremental backup engine
 *
 * Copyright (c) 2019-2020, Postgres Professional
 *
 * IDENTIFICATION
 *	  ptrack/ptrack.c
 *
 * INTERFACE ROUTINES (PostgreSQL side)
 *	  ptrackMapInit()          --- allocate new shared ptrack_map
 *	  ptrackMapAttach()        --- attach to the existing ptrack_map
 *	  assign_ptrack_map_size() --- ptrack_map_size GUC assign callback
 *	  ptrack_walkdir()         --- walk directory and mark all blocks of all
 *	                               data files in ptrack_map
 *	  ptrack_mark_block()      --- mark single page in ptrack_map
 *
 * Currently ptrack has following public API methods:
 *
 * # ptrack_version                  --- returns ptrack version string (2.0 currently).
 * # ptrack_get_pagemapset('LSN')    --- returns a set of changed data files with
 * 										 bitmaps of changed blocks since specified LSN.
 * # ptrack_init_lsn                 --- returns LSN of the last ptrack map initialization.
 *
 */

#include "postgres.h"

#include <unistd.h>
#include <sys/stat.h>

#if PG_VERSION_NUM < 120000
#include "access/htup_details.h"
#endif
#include "catalog/pg_tablespace.h"
#include "catalog/pg_type.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/pg_list.h"
#ifdef PGPRO_EE
/* For file_is_in_cfs_tablespace() only. */
#include "replication/basebackup.h"
#endif
#include "storage/copydir.h"
#include "storage/lmgr.h"
#if PG_VERSION_NUM >= 120000
#include "storage/md.h"
#endif
#include "storage/smgr.h"
#include "storage/reinit.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/pg_lsn.h"

#include "datapagemap.h"
#include "engine.h"
#include "ptrack.h"

PG_MODULE_MAGIC;

PtrackMap	ptrack_map = NULL;
uint64		ptrack_map_size;
int			ptrack_map_size_tmp;

static copydir_hook_type prev_copydir_hook = NULL;
static mdwrite_hook_type prev_mdwrite_hook = NULL;
static mdextend_hook_type prev_mdextend_hook = NULL;
static ProcessSyncRequests_hook_type prev_ProcessSyncRequests_hook = NULL;

void		_PG_init(void);
void		_PG_fini(void);

static void ptrack_copydir_hook(const char *path);
static void ptrack_mdwrite_hook(RelFileNodeBackend smgr_rnode,
								ForkNumber forkno, BlockNumber blkno);
static void ptrack_mdextend_hook(RelFileNodeBackend smgr_rnode,
								 ForkNumber forkno, BlockNumber blkno);
static void ptrack_ProcessSyncRequests_hook(void);

static void ptrack_gather_filelist(List **filelist, char *path, Oid spcOid, Oid dbOid);
static int	ptrack_filelist_getnext(PtScanCtx * ctx);

/*
 * Module load callback
 */
void
_PG_init(void)
{
	if (!process_shared_preload_libraries_in_progress)
		elog(ERROR, "ptrack module must be initialized by Postmaster. "
			 "Put the following line to configuration file: "
			 "shared_preload_libraries='ptrack'");

	/*
	 * Define (or redefine) custom GUC variables.
	 *
	 * XXX: for some reason assign_ptrack_map_size is called twice during the
	 * postmaster boot!  First, it is always called with bootValue, so we use
	 * -1 as default value and no-op here.  Next, it is called with the actual
	 * value from config.
	 */
	DefineCustomIntVariable("ptrack.map_size",
							"Sets the size of ptrack map in MB used for incremental backup (0 disabled).",
							NULL,
							&ptrack_map_size_tmp,
							-1,
							-1, 32 * 1024, /* limit to 32 GB */
							PGC_POSTMASTER,
							0,
							NULL,
							assign_ptrack_map_size,
							NULL);

	/* Install hooks */
	prev_copydir_hook = copydir_hook;
	copydir_hook = ptrack_copydir_hook;
	prev_mdwrite_hook = mdwrite_hook;
	mdwrite_hook = ptrack_mdwrite_hook;
	prev_mdextend_hook = mdextend_hook;
	mdextend_hook = ptrack_mdextend_hook;
	prev_ProcessSyncRequests_hook = ProcessSyncRequests_hook;
	ProcessSyncRequests_hook = ptrack_ProcessSyncRequests_hook;
}

/*
 * Module unload callback
 */
void
_PG_fini(void)
{
	/* Uninstall hooks */
	copydir_hook = prev_copydir_hook;
	mdwrite_hook = prev_mdwrite_hook;
	mdextend_hook = prev_mdextend_hook;
	ProcessSyncRequests_hook = prev_ProcessSyncRequests_hook;
}

/*
 * Ptrack follow up for copydir() routine.  It parses database OID
 * and tablespace OID from path string.  We do not need to recoursively
 * walk subdirs here, copydir() will do it for us if needed.
 */
static void
ptrack_copydir_hook(const char *path)
{
	Oid			spcOid = InvalidOid;
	Oid			dbOid = InvalidOid;
	int			oidchars;
	char		oidbuf[OIDCHARS + 1];

	elog(DEBUG1, "ptrack_copydir_hook: path %s", path);

	if (strstr(path, "global/") == path)
		spcOid = GLOBALTABLESPACE_OID;
	else if (strstr(path, "base/") == path)
	{
		spcOid = DEFAULTTABLESPACE_OID;
		oidchars = strspn(path + 5, "0123456789");
		strncpy(oidbuf, path + 5, oidchars);
		oidbuf[oidchars] = '\0';
		dbOid = atooid(oidbuf);
	}
	else if (strstr(path, "pg_tblspc/") == path)
	{
		char	   *dbPos;

		oidchars = strspn(path + 10, "0123456789");
		strncpy(oidbuf, path + 10, oidchars);
		oidbuf[oidchars] = '\0';
		spcOid = atooid(oidbuf);

		dbPos = strstr(path, TABLESPACE_VERSION_DIRECTORY) + strlen(TABLESPACE_VERSION_DIRECTORY) + 1;
		oidchars = strspn(dbPos, "0123456789");
		strncpy(oidbuf, dbPos, oidchars);
		oidbuf[oidchars] = '\0';
		dbOid = atooid(oidbuf);
	}

	elog(DEBUG1, "ptrack_copydir_hook: spcOid %u, dbOid %u", spcOid, dbOid);

#ifdef PGPRO_EE
	/*
	 * Currently, we do not track files from compressed tablespaces in ptrack.
	 */
	if (file_is_in_cfs_tablespace(path))
		elog(DEBUG1, "ptrack_copydir_hook: skipping changes tracking in the CFS tablespace %u", spcOid);
	else
#endif
	ptrack_walkdir(path, spcOid, dbOid);

	if (prev_copydir_hook)
		prev_copydir_hook(path);
}

static void
ptrack_mdwrite_hook(RelFileNodeBackend smgr_rnode,
					ForkNumber forknum, BlockNumber blocknum)
{
	ptrack_mark_block(smgr_rnode, forknum, blocknum);

	if (prev_mdwrite_hook)
		prev_mdwrite_hook(smgr_rnode, forknum, blocknum);
}

static void
ptrack_mdextend_hook(RelFileNodeBackend smgr_rnode,
					 ForkNumber forknum, BlockNumber blocknum)
{
	ptrack_mark_block(smgr_rnode, forknum, blocknum);

	if (prev_mdextend_hook)
		prev_mdextend_hook(smgr_rnode, forknum, blocknum);
}

static void
ptrack_ProcessSyncRequests_hook()
{
	ptrackCheckpoint();

	if (prev_ProcessSyncRequests_hook)
		prev_ProcessSyncRequests_hook();
}

/*
 * Recursively walk through the path and add all data files to filelist.
 */
static void
ptrack_gather_filelist(List **filelist, char *path, Oid spcOid, Oid dbOid)
{
	DIR		   *dir;
	struct dirent *de;

	dir = AllocateDir(path);

	while ((de = ReadDirExtended(dir, path, LOG)) != NULL)
	{
		char		subpath[MAXPGPATH * 2];
		struct stat fst;
		int			sret;

		CHECK_FOR_INTERRUPTS();

		if (strcmp(de->d_name, ".") == 0 ||
			strcmp(de->d_name, "..") == 0 ||
			looks_like_temp_rel_name(de->d_name))
			continue;

		snprintf(subpath, sizeof(subpath), "%s/%s", path, de->d_name);

		sret = lstat(subpath, &fst);

		if (sret < 0)
		{
			ereport(LOG,
					(errcode_for_file_access(),
					 errmsg("ptrack: could not stat file \"%s\": %m", subpath)));
			continue;
		}

		if (S_ISREG(fst.st_mode))
		{
			/* Regular file inside database directory, otherwise skip it */
			if (dbOid != InvalidOid || spcOid == GLOBALTABLESPACE_OID)
			{
				int			oidchars;
				char		oidbuf[OIDCHARS + 1];
				char	   *segpath;
				PtrackFileList_i *pfl = palloc0(sizeof(PtrackFileList_i));

				/*
				 * Check that filename seems to be a regular relation file.
				 */
				if (!parse_filename_for_nontemp_relation(de->d_name, &oidchars, &pfl->forknum))
					continue;

				/* Parse segno for main fork */
				if (pfl->forknum == MAIN_FORKNUM)
				{
					segpath = strstr(de->d_name, ".");
					pfl->segno = segpath != NULL ? atoi(segpath + 1) : 0;
				}
				else
					pfl->segno = 0;

				memcpy(oidbuf, de->d_name, oidchars);
				oidbuf[oidchars] = '\0';
				pfl->relnode.relNode = atooid(oidbuf);
				pfl->relnode.dbNode = dbOid;
				pfl->relnode.spcNode = spcOid == InvalidOid ? DEFAULTTABLESPACE_OID : spcOid;
				pfl->path = GetRelationPath(dbOid, pfl->relnode.spcNode,
											pfl->relnode.relNode, InvalidBackendId, pfl->forknum);

				*filelist = lappend(*filelist, pfl);

				elog(DEBUG3, "ptrack: added file %s of rel %u to file list",
					 pfl->path, pfl->relnode.relNode);
			}
		}
		else if (S_ISDIR(fst.st_mode))
		{
			if (strspn(de->d_name + 1, "0123456789") == strlen(de->d_name + 1)
				&& dbOid == InvalidOid)
				ptrack_gather_filelist(filelist, subpath, spcOid, atooid(de->d_name));
			else if (spcOid != InvalidOid && strcmp(de->d_name, TABLESPACE_VERSION_DIRECTORY) == 0)
				ptrack_gather_filelist(filelist, subpath, spcOid, InvalidOid);
		}
		/* TODO: is it enough to properly check symlink support? */
#ifndef WIN32
		else if (S_ISLNK(fst.st_mode))
#else
		else if (pgwin32_is_junction(subpath))
#endif
		{
			/*
			 * We expect that symlinks with only digits in the name to be
			 * tablespaces
			 */
			if (strspn(de->d_name + 1, "0123456789") == strlen(de->d_name + 1))
				ptrack_gather_filelist(filelist, subpath, atooid(de->d_name), InvalidOid);
		}
	}

	FreeDir(dir);				/* we ignore any error here */
}

static int
ptrack_filelist_getnext(PtScanCtx * ctx)
{
	PtrackFileList_i *pfl = NULL;
	ListCell   *cell;
	char	   *fullpath;
	struct stat fst;

	/* No more file in the list */
	if (list_length(ctx->filelist) == 0)
		return -1;

	/* Get first file from the head */
	cell = list_head(ctx->filelist);
	pfl = (PtrackFileList_i *) lfirst(cell);

	/* Remove this file from the list */
	ctx->filelist = list_delete_first(ctx->filelist);

	if (pfl->segno > 0)
	{
		Assert(pfl->forknum == MAIN_FORKNUM);
		fullpath = psprintf("%s/%s.%d", DataDir, pfl->path, pfl->segno);
		ctx->relpath = psprintf("%s.%d", pfl->path, pfl->segno);
	}
	else
	{
		fullpath = psprintf("%s/%s", DataDir, pfl->path);
		ctx->relpath = pfl->path;
	}

	ctx->bid.relnode.spcNode = pfl->relnode.spcNode;
	ctx->bid.relnode.dbNode = pfl->relnode.dbNode;
	ctx->bid.relnode.relNode = pfl->relnode.relNode;
	ctx->bid.forknum = pfl->forknum;
	ctx->bid.blocknum = 0;

	if (stat(fullpath, &fst) != 0)
	{
		elog(WARNING, "ptrack: cannot stat file %s", fullpath);

		/* But try the next one */
		return ptrack_filelist_getnext(ctx);
	}

	if (pfl->segno > 0)
	{
		ctx->relsize = pfl->segno * RELSEG_SIZE + fst.st_size / BLCKSZ;
		ctx->bid.blocknum = pfl->segno * RELSEG_SIZE;
	}
	else
		/* Estimate relsize as size of first segment in blocks */
		ctx->relsize = fst.st_size / BLCKSZ;

	elog(DEBUG3, "ptrack: got file %s with size %u from the file list", pfl->path, ctx->relsize);

	return 0;
}

/*
 * Returns ptrack version currently in use.
 */
PG_FUNCTION_INFO_V1(ptrack_version);
Datum
ptrack_version(PG_FUNCTION_ARGS)
{
	PG_RETURN_TEXT_P(cstring_to_text(PTRACK_VERSION));
}

/*
 * Function to get last ptrack map initialization LSN.
 */
PG_FUNCTION_INFO_V1(ptrack_init_lsn);
Datum
ptrack_init_lsn(PG_FUNCTION_ARGS)
{
	if (ptrack_map != NULL)
	{
		XLogRecPtr	init_lsn = pg_atomic_read_u64(&ptrack_map->init_lsn);

		PG_RETURN_LSN(init_lsn);
	}
	else
	{
		elog(WARNING, "ptrack is disabled");
		PG_RETURN_LSN(InvalidXLogRecPtr);
	}
}

/*
 * Return set of database blocks which were changed since specified LSN.
 * This function may return false positives (blocks that have not been updated).
 */
PG_FUNCTION_INFO_V1(ptrack_get_pagemapset);
Datum
ptrack_get_pagemapset(PG_FUNCTION_ARGS)
{
	FuncCallContext *funcctx;
	PtScanCtx  *ctx;
	MemoryContext oldcontext;
	XLogRecPtr	update_lsn;
	datapagemap_t pagemap;
	char		gather_path[MAXPGPATH];

	/* Exit immediately if there is no map */
	if (ptrack_map == NULL)
		elog(ERROR, "ptrack is disabled");

	if (SRF_IS_FIRSTCALL())
	{
		TupleDesc	tupdesc;

		funcctx = SRF_FIRSTCALL_INIT();

		oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);

		ctx = (PtScanCtx *) palloc0(sizeof(PtScanCtx));
		ctx->lsn = PG_GETARG_LSN(0);
		ctx->filelist = NIL;

		/* Make tuple descriptor */
#if PG_VERSION_NUM >= 120000
		tupdesc = CreateTemplateTupleDesc(2);
#else
		tupdesc = CreateTemplateTupleDesc(2, false);
#endif
		TupleDescInitEntry(tupdesc, (AttrNumber) 1, "path", TEXTOID, -1, 0);
		TupleDescInitEntry(tupdesc, (AttrNumber) 2, "pagemap", BYTEAOID, -1, 0);
		funcctx->tuple_desc = BlessTupleDesc(tupdesc);

		funcctx->user_fctx = ctx;

		/*
		 * Form a list of all data files inside global, base and pg_tblspc.
		 *
		 * TODO: refactor it to do not form a list, but use iterator instead,
		 * e.g. just ptrack_filelist_getnext(ctx).
		 */
		sprintf(gather_path, "%s/%s", DataDir, "global");
		ptrack_gather_filelist(&ctx->filelist, gather_path, GLOBALTABLESPACE_OID, InvalidOid);

		sprintf(gather_path, "%s/%s", DataDir, "base");
		ptrack_gather_filelist(&ctx->filelist, gather_path, InvalidOid, InvalidOid);

		sprintf(gather_path, "%s/%s", DataDir, "pg_tblspc");
		ptrack_gather_filelist(&ctx->filelist, gather_path, InvalidOid, InvalidOid);

		MemoryContextSwitchTo(oldcontext);
	}

	funcctx = SRF_PERCALL_SETUP();
	ctx = (PtScanCtx *) funcctx->user_fctx;

	/* Initialize bitmap */
	pagemap.bitmap = NULL;
	pagemap.bitmapsize = 0;

	/* Take next file from the list */
	if (ptrack_filelist_getnext(ctx) < 0)
		SRF_RETURN_DONE(funcctx);

	while (true)
	{
		/* Stop traversal if there are no more segments */
		if (ctx->bid.blocknum > ctx->relsize)
		{
			/* We completed a segment and there is a bitmap to return */
			if (pagemap.bitmap != NULL)
			{
				Datum		values[2];
				bool		nulls[2] = {false};
				char		pathname[MAXPGPATH];
				bytea	   *result = NULL;
				Size		result_sz = pagemap.bitmapsize + VARHDRSZ;
				HeapTuple	htup = NULL;

				/* Create a bytea copy of our bitmap */
				result = (bytea *) palloc(result_sz);
				SET_VARSIZE(result, result_sz);
				memcpy(VARDATA(result), pagemap.bitmap, pagemap.bitmapsize);

				strcpy(pathname, ctx->relpath);

				values[0] = CStringGetTextDatum(pathname);
				values[1] = PointerGetDatum(result);

				pfree(pagemap.bitmap);
				pagemap.bitmap = NULL;
				pagemap.bitmapsize = 0;

				htup = heap_form_tuple(funcctx->tuple_desc, values, nulls);
				if (htup)
					SRF_RETURN_NEXT(funcctx, HeapTupleGetDatum(htup));
			}
			else
			{
				/* We have just processed unchanged file, let's pick next */
				if (ptrack_filelist_getnext(ctx) < 0)
					SRF_RETURN_DONE(funcctx);
			}
		}

		update_lsn = pg_atomic_read_u64(&ptrack_map->entries[BID_HASH_FUNC(ctx->bid)]);

		if (update_lsn != InvalidXLogRecPtr)
			elog(DEBUG3, "ptrack: update_lsn %X/%X of blckno %u of file %s",
				 (uint32) (update_lsn >> 32), (uint32) update_lsn,
				 ctx->bid.blocknum, ctx->relpath);

		/* Block has been changed since specified LSN. Mark it in the bitmap */
		if (update_lsn >= ctx->lsn)
			datapagemap_add(&pagemap, ctx->bid.blocknum % ((BlockNumber) RELSEG_SIZE));

		ctx->bid.blocknum += 1;
	}
}
