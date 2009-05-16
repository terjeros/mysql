/* Copyright � 2006-2008 MySQL AB, 2008-2009 Sun Microsystems, Inc.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

/* XXX correct? */

#ifndef MYSQL_SERVER
#define MYSQL_SERVER
#endif

#include "mysql_priv.h"
#include "falcon_probes.h"

#ifdef _WIN32
#pragma pack()
#endif

#include "ha_falcon.h"
#include "StorageConnection.h"
#include "StorageTable.h"
#include "StorageTableShare.h"
#include "StorageHandler.h"
#include "CmdGen.h"
#include "InfoTable.h"
#include "Format.h"
#include "Error.h"
#include "Log.h"
#include "ErrorInjector.h"

#ifdef _WIN32
#define I64FORMAT			"%I64d"
#else
#define I64FORMAT			"%lld"
#endif

#include "ScaledBinary.h"
#include "BigInt.h"

/* Verify that the compiler options have enabled C++ exception support */
#if (defined(__GNUC__) && !defined(__EXCEPTIONS)) || (defined (_MSC_VER) && !defined (_CPPUNWIND))
#error Falcon needs to be compiled with support for C++ exceptions. Please check your compiler settings.
#endif

//#define NO_OPTIMIZE
#define VALIDATE
//#define DEBUG_BACKLOG

#ifndef MIN
#define MIN(a,b)			((a <= b) ? (a) : (b))
#define MAX(a,b)			((a >= b) ? (a) : (b))
#endif

#ifndef ONLINE_ALTER
#define ONLINE_ALTER
#endif

#ifdef DEBUG_BACKLOG
static const uint LOAD_AUTOCOMMIT_RECORDS = 10000000;
#else
static const uint LOAD_AUTOCOMMIT_RECORDS = 10000;
#endif

static const char falcon_hton_name[] = "Falcon";

static const char *falcon_extensions[] = {
	".fts",
	".fl1",
	".fl2",
	NullS
};

extern StorageHandler	*storageHandler;

#define PARAMETER_UINT(_name, _text, _min, _default, _max, _flags, _update_function) \
	uint falcon_##_name;
#define PARAMETER_BOOL(_name, _text, _default, _flags, _update_function) \
	my_bool falcon_##_name;
#include "StorageParameters.h"
#undef PARAMETER_UINT
#undef PARAMETER_BOOL

ulonglong	falcon_record_memory_max;
ulonglong	falcon_serial_log_file_size;
uint		falcon_allocation_extent;
ulonglong	falcon_page_cache_size;
char*		falcon_serial_log_dir;
char*		falcon_checkpoint_schedule;
char*		falcon_scavenge_schedule;
char*		falcon_error_inject;
FILE		*falcon_log_file;

// Determine the largest memory address, assume 64-bits max

static const ulonglong MSB = 1ULL << ((sizeof(void *)*8 - 1) & 63);
ulonglong max_memory_address = MSB | (MSB - 1);

// These are the isolation levels we actually use.
// They corespond to enum_tx_isolation from hamdler.h
// 0 = ISO_READ_UNCOMMITTED, 1 = ISO_READ_COMMITTED,
// 2 = ISO_REPEATABLE_READ,  3 = ISO_SERIALIZABLE

int	isolation_levels[4] = 
	{TRANSACTION_CONSISTENT_READ, 	// TRANSACTION_READ_UNCOMMITTED
	TRANSACTION_READ_COMMITTED,
	TRANSACTION_CONSISTENT_READ, 	// TRANSACTION_WRITE_COMMITTED, // This is repeatable read
	TRANSACTION_CONSISTENT_READ};	// TRANSACTION_SERIALIZABLE

static const ulonglong default_table_flags = (	  HA_REC_NOT_IN_SEQ
												| HA_NULL_IN_KEY
												| HA_PARTIAL_COLUMN_READ
												| HA_CAN_GEOMETRY
												//| HA_AUTO_PART_KEY
												| HA_ONLINE_ALTER
												| HA_BINLOG_ROW_CAPABLE
												| HA_CAN_READ_ORDER_IF_LIMIT);

static struct st_mysql_show_var falconStatus[] =
{
  //{"static",     (char*)"just a static text",     SHOW_CHAR},
  //{"called",     (char*)&number_of_calls, SHOW_LONG},
  {0,0,SHOW_UNDEF}
};

extern THD*		current_thd;
static int getTransactionIsolation( THD * thd);

static handler *falcon_create_handler(handlerton *hton,
                                      TABLE_SHARE *table, MEM_ROOT *mem_root)
{
	return new (mem_root) StorageInterface(hton, table);
}

handlerton *falcon_hton;

void openFalconLogFile(const char *file)
{
	if (falcon_log_file)
		fclose(falcon_log_file);
	falcon_log_file = fopen(file, "a");
}

void closeFalconLogFile()
{
	if (falcon_log_file)
		{
		fclose(falcon_log_file);
		falcon_log_file = NULL;
		}
}

void flushFalconLogFile()
{
	if (falcon_log_file)
		fflush(falcon_log_file);
}

bool checkExceptionSupport()
{
    // Validate that the code has been compiled with support for exceptions
    // by throwing and catching an exception. If the executable does not
    // support exceptions we will reach the return false statement
	try
		{
		throw 1;
		}
	catch (int) 
		{
		return true;
		}
	return false;
}

// Init/term routines for THR_LOCK, used within StorageTableShare.
void falcon_lock_init(void *lock)
{
	thr_lock_init((THR_LOCK *)lock);
}


void falcon_lock_deinit(void *lock)
{
	thr_lock_delete((THR_LOCK *)lock);
}

int StorageInterface::falcon_init(void *p)
{
	DBUG_ENTER("falcon_init");
	falcon_hton = (handlerton *)p;
	
	ERROR_INJECTOR_PARSE(falcon_error_inject);
	
	my_bool error = false;

	if (!checkExceptionSupport()) 
		{
		sql_print_error("Falcon must be compiled with C++ exceptions enabled to work. Please adjust your compile flags.");
		FATAL("Falcon exiting process.\n");
		}

	StorageHandler::setDataDirectory(mysql_real_data_home);

	storageHandler = getFalconStorageHandler(sizeof(THR_LOCK));
	
	falcon_hton->state = SHOW_OPTION_YES;
	falcon_hton->db_type = DB_TYPE_FALCON;
	falcon_hton->savepoint_offset = sizeof(void*);
	falcon_hton->close_connection = StorageInterface::closeConnection;
	falcon_hton->savepoint_set = StorageInterface::savepointSet;
	falcon_hton->savepoint_rollback = StorageInterface::savepointRollback;
	falcon_hton->savepoint_release = StorageInterface::savepointRelease;
	falcon_hton->commit = StorageInterface::commit;
	falcon_hton->rollback = StorageInterface::rollback;
	falcon_hton->create = falcon_create_handler;
	falcon_hton->drop_database  = StorageInterface::dropDatabase;
	falcon_hton->panic  = StorageInterface::panic;

#if 0
	falcon_hton->alter_table_flags  = StorageInterface::alter_table_flags;
#endif

	if (falcon_support_xa)
		{
		falcon_hton->prepare = StorageInterface::prepare;
		falcon_hton->recover = StorageInterface::recover;
		falcon_hton->commit_by_xid = StorageInterface::commit_by_xid;
		falcon_hton->rollback_by_xid = StorageInterface::rollback_by_xid;
		}
	else
		{
		falcon_hton->prepare = NULL;
		falcon_hton->recover = NULL;
		falcon_hton->commit_by_xid = NULL;
		falcon_hton->rollback_by_xid = NULL;
		}

	falcon_hton->start_consistent_snapshot = StorageInterface::start_consistent_snapshot;

	falcon_hton->alter_tablespace = StorageInterface::alter_tablespace;
	falcon_hton->fill_is_table = StorageInterface::fill_is_table;
	//falcon_hton->show_status  = StorageInterface::show_status;
	falcon_hton->flags = HTON_NO_FLAGS;
	falcon_debug_mask&= ~(LogMysqlInfo|LogMysqlWarning|LogMysqlError);
	storageHandler->addNfsLogger(falcon_debug_mask, StorageInterface::logger, NULL);
	storageHandler->addNfsLogger(LogMysqlInfo|LogMysqlWarning|LogMysqlError, StorageInterface::mysqlLogger, NULL);

	if (falcon_debug_server)
		storageHandler->startNfsServer();

	try
		{
		storageHandler->initialize();
		}
	catch(SQLException &e)
		{
		sql_print_error("Falcon: %s", e.getText());
		error = true;
		}
	catch(...)
		{
		sql_print_error("Falcon: General exception in initialization");
		error = true;
		}


	if (error)
		{
		// Cleanup after error
		falcon_deinit(0);
		DBUG_RETURN(1);
		}
		
	DBUG_RETURN(0);
}


int StorageInterface::falcon_deinit(void *p)
{
	if(storageHandler)
		{
		storageHandler->deleteNfsLogger(StorageInterface::mysqlLogger, NULL);
		storageHandler->deleteNfsLogger(StorageInterface::logger, NULL);
		storageHandler->shutdownHandler();
		freeFalconStorageHandler();
		}
	return 0;
}

int falcon_strnxfrm (void *cs,
                     const char *dst, uint dstlen, int nweights,
                     const char *src, uint srclen)
{
	CHARSET_INFO *charset = (CHARSET_INFO*) cs;

	return (int)charset->coll->strnxfrm(charset, (uchar *) dst, dstlen, nweights,
	                              (uchar *) src, srclen, 0);
}

int falcon_strnxfrm_space_pad (void *cs,
                     const char *dst, uint dstlen, int nweights,
                     const char *src, uint srclen)
{
	CHARSET_INFO *charset = (CHARSET_INFO*) cs;

	return (int)charset->coll->strnxfrm(charset, (uchar *) dst, dstlen, nweights,
	                              (uchar *) src, srclen, MY_STRXFRM_PAD_WITH_SPACE);
}

char falcon_get_pad_char (void *cs)
{
	return (char) ((CHARSET_INFO*) cs)->pad_char;
}

int falcon_cs_is_binary (void *cs)
{
	return (0 == strcmp(((CHARSET_INFO*) cs)->name, "binary"));
//	return ((((CHARSET_INFO*) cs)->state & MY_CS_BINSORT) == MY_CS_BINSORT);
}

unsigned int falcon_get_mbmaxlen (void *cs)
{
	return ((CHARSET_INFO*) cs)->mbmaxlen;
}

char falcon_get_min_sort_char (void *cs)
{
	return (char) ((CHARSET_INFO*) cs)->min_sort_char;
}

// Return the actual number of characters in the string
// Note, this is not the number of characters with collatable weight.

uint falcon_strnchrlen(void *cs, const char *s, uint l)
{
	CHARSET_INFO *charset = (CHARSET_INFO*) cs;

	if (charset->mbmaxlen == 1)
		return l;

	uint chrCount = 0;
	uchar *ch = (uchar *) s;
	uchar *end = ch + l;

	while (ch < end)
		{
		int len = charset->cset->mbcharlen(charset, *ch);
		if (len == 0)
			break;  // invalid character.

		ch += len;
		chrCount++;
		}

	return chrCount;
}

// Determine how many bytes are required to store the output of cs->coll->strnxfrm()
// cs is how the source string is formatted.
// srcLen is the number of bytes in the source string.
// partialKey is the max key buffer size if not zero.
// bufSize is the ultimate maximum destSize.
// If the string is multibyte, strnxfrmlen expects srcLen to be
// the maximum number of characters this can be.  Falcon wants to send
// a number that represents the actual number of characters in the string
// so that the call to cs->coll->strnxfrm() will not pad.

uint falcon_strnxfrmlen(void *cs, const char *s, uint srcLen,
						int partialKey, int bufSize)
{
	CHARSET_INFO *charset = (CHARSET_INFO*) cs;
	uint chrLen = falcon_strnchrlen(cs, s, srcLen);
	int maxChrLen = partialKey ? min(chrLen, partialKey / charset->mbmaxlen) : chrLen;

	return (uint)min(charset->coll->strnxfrmlen(charset, maxChrLen * charset->mbmaxlen), (uint) bufSize);
}

// Return the number of bytes used in s to hold a certain number of characters.
// This partialKey is a byte length with charset->mbmaxlen figured in.
// In other words, it is the number of characters times mbmaxlen.

uint falcon_strntrunc(void *cs, int partialKey, const char *s, uint l)
{
	CHARSET_INFO *charset = (CHARSET_INFO*) cs;

	if ((charset->mbmaxlen == 1) || (partialKey == 0))
		return min((uint) partialKey, l);

	int charLimit = partialKey / charset->mbmaxlen;
	uchar *ch = (uchar *) s;
	uchar *end = ch + l;

	while ((ch < end) && charLimit)
		{
		int len = charset->cset->mbcharlen(charset, *ch);
		if (len == 0)
			break;  // invalid character.

		ch += len;
		charLimit--;
		}

	return (uint)(ch - (uchar *) s);
}

int falcon_strnncoll(void *cs, const char *s1, uint l1, const char *s2, uint l2, char flag)
{
	CHARSET_INFO *charset = (CHARSET_INFO*) cs;

	return charset->coll->strnncoll(charset, (uchar *) s1, l1, (uchar *) s2, l2, flag);
}

int falcon_strnncollsp(void *cs, const char *s1, uint l1, const char *s2, uint l2, char flag)
{
	CHARSET_INFO *charset = (CHARSET_INFO*) cs;

	return charset->coll->strnncollsp(charset, (uchar *) s1, l1, (uchar *) s2, l2, flag);
}

int (*strnncoll)(struct charset_info_st *, const uchar *, uint, const uchar *, uint, my_bool);

/***
#ifdef _DEBUG
#undef THIS_FILE
static const char THIS_FILE[]=__FILE__;
#endif
***/

#ifndef DIG_PER_DEC1
typedef decimal_digit_t dec1;
typedef longlong      dec2;

#define DIG_PER_DEC1 9
#define DIG_MASK     100000000
#define DIG_BASE     1000000000
#define DIG_MAX      (DIG_BASE-1)
#define DIG_BASE2    ((dec2)DIG_BASE * (dec2)DIG_BASE)
#define ROUND_UP(X)  (((X)+DIG_PER_DEC1-1)/DIG_PER_DEC1)
#endif

//////////////////////////////////////////////////////////////////////
// Construction/Destruction
//////////////////////////////////////////////////////////////////////

StorageInterface::StorageInterface(handlerton *hton, TABLE_SHARE *table_arg)
  : handler(hton, table_arg)
{
	ref_length = sizeof(lastRecord);
	stats.records = 1000;
	stats.data_file_length = 10000;
	stats.index_file_length = 0;
	tableLocked = false;
	lockForUpdate = false;
	storageTable = NULL;
	storageConnection = NULL;
	storageShare = NULL;
	share = table_arg;
	lastRecord = -1;
	mySqlThread = NULL;
	activeBlobs = NULL;
	freeBlobs = NULL;
	errorText = NULL;
	fieldMap = NULL;
	indexOrder = false;
	
	if (table_arg)
		{
		recordLength = table_arg->reclength;
		tempTable = false;
		}
		
	tableFlags = default_table_flags;
}


StorageInterface::~StorageInterface(void)
{
	if (storageTable)
		storageTable->clearCurrentIndex();

	if (activeBlobs)
		freeActiveBlobs();

	for (StorageBlob *blob; (blob = freeBlobs); )
		{
		freeBlobs = blob->next;
		delete blob;
		}

	if (storageTable)
		{
		storageTable->deleteStorageTable();
		storageTable = NULL;
		}

	if (storageConnection)
		{
		storageConnection->release();
		storageConnection = NULL;
		}

	unmapFields();
}

int StorageInterface::rnd_init(bool scan)
{
	DBUG_ENTER("StorageInterface::rnd_init");
	nextRecord = 0;
	lastRecord = -1;

	DBUG_RETURN(0);
}


int StorageInterface::open(const char *name, int mode, uint test_if_locked)
{
	DBUG_ENTER("StorageInterface::open");

	// Temporarily comment out DTrace probes in Falcon, see bug #36403
	// FALCON_OPEN();

	if (!mySqlThread)
		mySqlThread = current_thd;

	if (!storageTable)
		{
		storageShare = storageHandler->findTable(name);
		storageConnection = storageHandler->getStorageConnection(storageShare, mySqlThread, mySqlThread->thread_id, OpenDatabase);

		if (!storageConnection)
			DBUG_RETURN(HA_ERR_NO_CONNECTION);

		*((StorageConnection**) thd_ha_data(mySqlThread, falcon_hton)) = storageConnection;
		storageConnection->addRef();
		storageTable = storageConnection->getStorageTable(storageShare);

		if (!storageShare->initialized)
			{
			storageShare->lock(true);

			if (!storageShare->initialized)
				{
				storageShare->setTablePath(name, tempTable);
				storageShare->initialized = true;
				}

			// Register any collations used

			uint fieldCount = (table) ? table->s->fields : 0;

			for (uint n = 0; n < fieldCount; ++n)
				{
				Field *field = table->field[n];
				CHARSET_INFO *charset = field->charset();

				if (charset)
					storageShare->registerCollation(charset->name, charset);
				}

			storageShare->unlock();
			}
		}

	int ret = storageTable->open();

	if (ret == StorageErrorTableNotFound)
		sql_print_error("Server is attempting to access a table %s,\n"
				"which doesn't exist in Falcon.", name);

	if (ret)
		DBUG_RETURN(error(ret));

	thr_lock_data_init((THR_LOCK *)storageShare->impure, &lockData, NULL);

	// Map fields for Falcon record encoding
	
	mapFields(table);
	
	// Map server indexes to Falcon internal indexes
	
	setIndexes(table);
	
	DBUG_RETURN(error(ret));
}


StorageConnection* StorageInterface::getStorageConnection(THD* thd)
{
	return *(StorageConnection**) thd_ha_data(thd, falcon_hton);
}

int StorageInterface::close(void)
{
	DBUG_ENTER("StorageInterface::close");

	if (storageTable)
		storageTable->clearCurrentIndex();

	unmapFields();

	// Temporarily comment out DTrace probes in Falcon, see bug #36403
	// FALCON_CLOSE();

	DBUG_RETURN(0);
}


int StorageInterface::check(THD* thd, HA_CHECK_OPT* check_opt)
{
#ifdef VALIDATE
	DBUG_ENTER("StorageInterface::check");

	if (storageConnection)
		storageConnection->validate(0);
		
	DBUG_RETURN(0);
#else
	return HA_ADMIN_NOT_IMPLEMENTED;
#endif
}

int StorageInterface::repair(THD* thd, HA_CHECK_OPT* check_opt)
{
#ifdef VALIDATE
	DBUG_ENTER("StorageInterface::repair");
	
	if (storageConnection)
		storageConnection->validate(VALIDATE_REPAIR);

	DBUG_RETURN(0);
#else
	return HA_ADMIN_NOT_IMPLEMENTED;
#endif
}

int StorageInterface::rnd_next(uchar *buf)
{
	DBUG_ENTER("StorageInterface::rnd_next");
	ha_statistic_increment(&SSV::ha_read_rnd_next_count);

	if (activeBlobs)
		freeActiveBlobs();

	lastRecord = storageTable->next(nextRecord, lockForUpdate);

	if (lastRecord < 0)
		{
		if (lastRecord == StorageErrorRecordNotFound)
			{
			lastRecord = -1;
			table->status = STATUS_NOT_FOUND;
			DBUG_RETURN(HA_ERR_END_OF_FILE);
			}

		DBUG_RETURN(error(lastRecord));
		}
	else
		table->status = 0;

	decodeRecord(buf);
	nextRecord = lastRecord + 1;

	DBUG_RETURN(0);
}


void StorageInterface::unlock_row(void)
{
	storageTable->unlockRow();
}

int StorageInterface::rnd_pos(uchar *buf, uchar *pos)
{
	int recordNumber;
	DBUG_ENTER("StorageInterface::rnd_pos");
	ha_statistic_increment(&SSV::ha_read_rnd_next_count);

	memcpy(&recordNumber, pos, sizeof(recordNumber));

	if (activeBlobs)
		freeActiveBlobs();

	int ret = storageTable->fetch(recordNumber, lockForUpdate);

	if (ret)
		{
		table->status = STATUS_NOT_FOUND;
		DBUG_RETURN(error(ret));
		}

	lastRecord = recordNumber;
	decodeRecord(buf);
	table->status = 0;

	DBUG_RETURN(0);
}

void StorageInterface::position(const uchar *record)
{
  DBUG_ENTER("StorageInterface::position");
  memcpy(ref, &lastRecord, sizeof(lastRecord));
  DBUG_VOID_RETURN;
}

int StorageInterface::info(uint what)
{
	DBUG_ENTER("StorageInterface::info");

#ifndef NO_OPTIMIZE
	if (what & HA_STATUS_VARIABLE)
		getDemographics();
#endif

	if (what & HA_STATUS_AUTO)
		// Return the next number to use.  Falcon stores the last number used.
		stats.auto_increment_value = storageShare->getSequenceValue(0) + 1;

	if (what & HA_STATUS_ERRKEY)
		errkey = indexErrorId;

	DBUG_RETURN(0);
}

void StorageInterface::getDemographics(void)
{
	DBUG_ENTER("StorageInterface::getDemographics");

	stats.records = (ha_rows) storageShare->estimateCardinality();
	// Temporary fix for Bug#28686. (HK) 2007-05-26.
	if (!stats.records)
		stats.records = 2;

	stats.block_size = 4096;

	storageShare->lockIndexes();

	for (uint n = 0; n < table->s->keys; ++n)
		{
		KEY *key = table->s->key_info + n;
		StorageIndexDesc *indexDesc = storageShare->getIndex(n);

		if (indexDesc)
			{
			ha_rows rows = 1 << indexDesc->numberSegments;

			for (uint segment = 0; segment < (uint)indexDesc->numberSegments /*key->key_parts*/; ++segment, rows >>= 1)
				{
				ha_rows recordsPerSegment = (ha_rows)indexDesc->segmentRecordCounts[segment];
				key->rec_per_key[segment] = (ulong) MAX(recordsPerSegment, rows);
				}
			}
		}

	storageShare->unlockIndexes();

	DBUG_VOID_RETURN;
}

int StorageInterface::optimize(THD* thd, HA_CHECK_OPT* check_opt)
{
	DBUG_ENTER("StorageInterface::optimize");

	int ret = storageTable->optimize();

	if (ret)
		DBUG_RETURN(error(ret));

	DBUG_RETURN(0);
}

uint8 StorageInterface::table_cache_type(void)
{
	return HA_CACHE_TBL_TRANSACT;
}

const char *StorageInterface::table_type(void) const
{
	DBUG_ENTER("StorageInterface::table_type");
	DBUG_RETURN(falcon_hton_name);
}


const char **StorageInterface::bas_ext(void) const
{
	DBUG_ENTER("StorageInterface::bas_ext");
	DBUG_RETURN(falcon_extensions);
}

void StorageInterface::update_create_info(HA_CREATE_INFO* create_info)
{
	DBUG_ENTER("StorageInterface::update_create_info");
	if (!(create_info->used_fields & HA_CREATE_USED_AUTO)) 
		{
		StorageInterface::info(HA_STATUS_AUTO);
		create_info->auto_increment_value = stats.auto_increment_value;
		}
	DBUG_VOID_RETURN;
}

ulonglong StorageInterface::table_flags(void) const
{
	DBUG_ENTER("StorageInterface::table_flags");
	DBUG_RETURN(tableFlags);
}

ulong StorageInterface::index_flags(uint idx, uint part, bool all_parts) const
{
	DBUG_ENTER("StorageInterface::index_flags");
	ulong flags = HA_READ_RANGE | ((indexOrder) ? HA_READ_ORDER : HA_KEY_SCAN_NOT_ROR);
	
	DBUG_RETURN(flags);
	//DBUG_RETURN(HA_READ_RANGE | HA_KEY_SCAN_NOT_ROR | (indexOrder ? HA_READ_ORDER : 0));
}

int StorageInterface::create(const char *mySqlName, TABLE *form, HA_CREATE_INFO *info)
{
	DBUG_ENTER("StorageInterface::create");
	tempTable = (info->options & HA_LEX_CREATE_TMP_TABLE) ? true : false;
	OpenOption openOption = (tempTable) ? OpenTemporaryDatabase : OpenOrCreateDatabase;

	if (storageTable)
		{
		storageTable->deleteStorageTable();
		storageTable = NULL;
		}

	if (!mySqlThread)
		mySqlThread = current_thd;

	storageShare = storageHandler->createTable(mySqlName, info->tablespace, tempTable);

	if (!storageShare)
		DBUG_RETURN(HA_ERR_TABLE_EXIST);

	storageConnection = storageHandler->getStorageConnection(storageShare, mySqlThread, mySqlThread->thread_id, openOption);
	*((StorageConnection**) thd_ha_data(mySqlThread, falcon_hton)) = storageConnection;

	if (!storageConnection)
		DBUG_RETURN(HA_ERR_NO_CONNECTION);

	storageTable = storageConnection->getStorageTable(storageShare);
	storageTable->setLocalTable(this);
	
	int ret;
	int64 incrementValue = 0;
	uint n;
	CmdGen gen;
	const char *tableName = storageTable->getName();
	const char *schemaName = storageTable->getSchemaName();
	//gen.gen("create table \"%s\".\"%s\" (\n", schemaName, tableName);
	genTable(form, &gen);
	
	if (form->found_next_number_field) // && form->s->next_number_key_offset == 0)
		{
		incrementValue = info->auto_increment_value;

		if (incrementValue == 0)
			incrementValue = 1;
		}

	/***
	if (form->s->primary_key < form->s->keys)
		{
		KEY *key = form->key_info + form->s->primary_key;
		gen.gen(",\n  primary key ");
		genKeyFields(key, &gen);
		}

	gen.gen (")");
	***/
	const char *tableSpace = NULL;

	if (tempTable)
		{
		if (info->tablespace)
			push_warning_printf(mySqlThread, MYSQL_ERROR::WARN_LEVEL_WARN, ER_ILLEGAL_HA_CREATE_OPTION,
				"TABLESPACE option is not supported for temporary tables. Switching to '%s' tablespace.", TEMPORARY_TABLESPACE);
		tableSpace = TEMPORARY_TABLESPACE;
		}
	else if (info->tablespace)
		{
		if (!strcasecmp(info->tablespace, TEMPORARY_TABLESPACE))
			{
			my_printf_error(ER_ILLEGAL_HA_CREATE_OPTION,
				"Cannot create non-temporary table '%s' in '%s' tablespace.", MYF(0), tableName, TEMPORARY_TABLESPACE);
			storageTable->deleteTable();
			DBUG_RETURN(HA_WRONG_CREATE_OPTION);
			}
		tableSpace = storageTable->getTableSpaceName();
		}
	else
		tableSpace = DEFAULT_TABLESPACE;

	if (tableSpace)
		gen.gen(" tablespace \"%s\"", tableSpace);

	DBUG_PRINT("info",("incrementValue = " I64FORMAT, (long long int)incrementValue));

	if ((ret = storageTable->create(gen.getString(), incrementValue)))
		{
		storageTable->deleteTable();
		
		DBUG_RETURN(error(ret));
		}

	for (n = 0; n < form->s->keys; ++n)
		if (n != form->s->primary_key)
			if ((ret = createIndex(schemaName, tableName, form, n)))
				{
				storageTable->deleteTable();

				DBUG_RETURN(error(ret));
				}

	// Map fields for Falcon record encoding
	
	mapFields(form);
	
	// Map server indexes to Falcon indexes
	
	setIndexes(table);

	DBUG_RETURN(0);
}

int StorageInterface::add_index(TABLE* table_arg, KEY* key_info, uint num_of_keys)
{
	DBUG_ENTER("StorageInterface::add_index");
	int ret = createIndex(storageTable->getSchemaName(), storageTable->getName(), table_arg, table_arg->s->keys);

	DBUG_RETURN(ret);
}

int StorageInterface::createIndex(const char *schemaName, const char *tableName, TABLE *srvTable, int indexId)
{
	int ret = 0;
	CmdGen gen;
	StorageIndexDesc indexDesc;
	getKeyDesc(srvTable, indexId, &indexDesc);
	
	if (indexDesc.primaryKey)
		{
		int64 incrementValue = 0;
		genTable(srvTable, &gen);
		
		// Primary keys are a special case, so use upgrade()
	
		ret = storageTable->upgrade(gen.getString(), incrementValue);
		}
	else
		{
		KEY *key = srvTable->key_info + indexId;
		const char *unique = (key->flags & HA_NOSAME) ? "unique " : "";
		gen.gen("create %sindex \"%s\" on %s.\"%s\" ", unique, indexDesc.name, schemaName, tableName);
		genKeyFields(key, &gen);
		
		ret = storageTable->createIndex(&indexDesc, gen.getString());
		}

	return ret;
}

int StorageInterface::dropIndex(const char *schemaName, const char *tableName, TABLE *srvTable, int indexId, bool online)
{
	StorageIndexDesc indexDesc;
	getKeyDesc(srvTable, indexId, &indexDesc);
	
	CmdGen gen;
	gen.gen("drop index %s.\"%s\"", schemaName, indexDesc.name);
	const char *sql = gen.getString();

	return storageTable->dropIndex(&indexDesc, sql, online);
}

#if 0
uint StorageInterface::alter_table_flags(uint flags)
{
	if (flags & ALTER_DROP_PARTITION)
		return 0;

	return HA_ONLINE_ADD_INDEX | HA_ONLINE_ADD_UNIQUE_INDEX;
}
#endif

bool StorageInterface::check_if_incompatible_data(HA_CREATE_INFO* create_info, uint table_changes)
{
	if (true || create_info->auto_increment_value != 0)
		return COMPATIBLE_DATA_NO;

	return COMPATIBLE_DATA_YES;
}

THR_LOCK_DATA **StorageInterface::store_lock(THD *thd, THR_LOCK_DATA **to,
                                            enum thr_lock_type lock_type)
{
	DBUG_ENTER("StorageInterface::store_lock");
	//lockForUpdate = (lock_type == TL_WRITE && thd->lex->sql_command == SQLCOM_SELECT);
	lockForUpdate = (lock_type == TL_WRITE);

	if (lock_type != TL_IGNORE && lockData.type == TL_UNLOCK)
		{
		/*
		  Here is where we get into the guts of a row level lock.
		  MySQL server will serialize write access to tables unless 
		  we tell it differently.  Falcon can handle concurrent changes
		  for most operations.  But allow the server to set its own 
		  lock type for certain SQL commands.
		*/

		const uint sql_command = thd_sql_command(thd);
		if (    (lock_type >= TL_WRITE_CONCURRENT_INSERT && lock_type <= TL_WRITE)
		    && !(thd_in_lock_tables(thd) && sql_command == SQLCOM_LOCK_TABLES)
		    && !(thd_tablespace_op(thd))
		    &&  (sql_command != SQLCOM_ALTER_TABLE)
		    &&  (sql_command != SQLCOM_DROP_TABLE)
		    &&  (sql_command != SQLCOM_CREATE_INDEX)
		    &&  (sql_command != SQLCOM_DROP_INDEX)
		    &&  (sql_command != SQLCOM_TRUNCATE)
		    &&  (sql_command != SQLCOM_OPTIMIZE)
		    &&  (sql_command != SQLCOM_CREATE_TABLE)
		   )
			lock_type = TL_WRITE_ALLOW_WRITE;

		/*
		In queries of type INSERT INTO t1 SELECT ... FROM t2 ...
		MySQL would use the lock TL_READ_NO_INSERT on t2 to prevent
		concurrent inserts into this table. Since Falcon can handle
		concurrent changes using its own mechanisms and this type of
		lock conflicts with TL_WRITE_ALLOW_WRITE we convert it to
		a normal read lock to allow concurrent changes.
		*/

		if (   lock_type == TL_READ_NO_INSERT
		    && !(thd_in_lock_tables(thd)
		    && sql_command == SQLCOM_LOCK_TABLES))
			{
			lock_type = TL_READ;
			}

		lockData.type = lock_type;
		}

	*to++ = &lockData;
	DBUG_RETURN(to);
}

int StorageInterface::delete_table(const char *tableName)
{
	DBUG_ENTER("StorageInterface::delete_table");

	if (!mySqlThread)
		mySqlThread = current_thd;

	if (!storageShare)
		if ( !(storageShare = storageHandler->preDeleteTable(tableName)) )
			DBUG_RETURN(0);

	if (!storageConnection)
		if ( !(storageConnection = storageHandler->getStorageConnection(storageShare, mySqlThread, mySqlThread->thread_id, OpenDatabase)) )
			DBUG_RETURN(0);

	if (!storageTable)
		storageTable = storageConnection->getStorageTable(storageShare);

	if (storageShare)
		{
		
		// Lock out other clients before locking the table
		
		storageShare->lockIndexes(true);
		storageShare->lock(true);

		storageShare->initialized = false;

		storageShare->unlock();
		storageShare->unlockIndexes();
		}

	int res = storageTable->deleteTable();
	storageTable->deleteStorageTable();
	storageTable = NULL;

	if (res == StorageErrorTableNotFound)
		sql_print_error("Server is attempting to drop a table %s,\n"
				"which doesn't exist in Falcon.", tableName);

	// (hk) Fix for Bug#31465 Running Falcon test suite leads
	//                        to warnings about temp tables
	// This fix could affect other DROP TABLE scenarios.
	// if (res == StorageErrorTableNotFound)
	
	if (res != StorageErrorUncommittedUpdates)
		res = 0;

	DBUG_RETURN(error(res));
}

#ifdef TRUNCATE_ENABLED
int StorageInterface::delete_all_rows()
{
	DBUG_ENTER("StorageInterface::delete_all_rows");

	if (!mySqlThread)
		mySqlThread = current_thd;

	// If this isn't a truncate, punt!
	
	if (thd_sql_command(mySqlThread) != SQLCOM_TRUNCATE)
		DBUG_RETURN(my_errno=HA_ERR_WRONG_COMMAND);

	int ret = 0;
	TABLE_SHARE *tableShare = table_share;
	const char *tableName = tableShare->normalized_path.str;
	
	if (!storageShare)
		if ( !(storageShare = storageHandler->preDeleteTable(tableName)) )
			DBUG_RETURN(0);

	if (!storageConnection)
		if ( !(storageConnection = storageHandler->getStorageConnection(storageShare, mySqlThread, mySqlThread->thread_id, OpenDatabase)) )
			DBUG_RETURN(0);

	if (!storageTable)
		storageTable = storageConnection->getStorageTable(storageShare);
		
	ret = storageTable->truncateTable();
		
	DBUG_RETURN(ret);
}
#endif

uint StorageInterface::max_supported_keys(void) const
{
	DBUG_ENTER("StorageInterface::max_supported_keys");
	DBUG_RETURN(MAX_KEY);
}

int StorageInterface::write_row(uchar *buff)
{
	DBUG_ENTER("StorageInterface::write_row");
	ha_statistic_increment(&SSV::ha_write_count);

	/* If we have a timestamp column, update it to the current time */

	if (table->timestamp_field_type & TIMESTAMP_AUTO_SET_ON_INSERT)
		table->timestamp_field->set_time();

	/*
		If we have an auto_increment column and we are writing a changed row
		or a new row, then update the auto_increment value in the record.
	*/

	if (table->next_number_field && buff == table->record[0])
		{
		int code = update_auto_increment();
		/*
		   May fail, e.g. due to an out of range value in STRICT mode.
		*/
		if (code)
			DBUG_RETURN(code);

		/*
		   If the new value is less than the current highest value, it will be
		   ignored by setSequenceValue().
		*/

		code = storageShare->setSequenceValue(table->next_number_field->val_int());

		if (code)
			DBUG_RETURN(error(code));
		}

	encodeRecord(buff, false);
	lastRecord = storageTable->insert();

	if (lastRecord < 0)
		{
		int code = lastRecord >> StoreErrorIndexShift;
		indexErrorId = (lastRecord & StoreErrorIndexMask) - 1;

		DBUG_RETURN(error(code));
		}

	if ((++insertCount % LOAD_AUTOCOMMIT_RECORDS) == 0)
		switch (thd_sql_command(mySqlThread))
			{
			case SQLCOM_LOAD:
			case SQLCOM_ALTER_TABLE:
			case SQLCOM_CREATE_TABLE:
			case SQLCOM_CREATE_INDEX:
				storageHandler->commit(mySqlThread);
				storageConnection->startTransaction(getTransactionIsolation(mySqlThread));
				storageConnection->markVerb();
				insertCount = 0;
				break;

			default:
				;
			}

	DBUG_RETURN(0);
}


int StorageInterface::update_row(const uchar* oldData, uchar* newData)
{
	DBUG_ENTER("StorageInterface::update_row");
	DBUG_ASSERT (lastRecord >= 0);

	/* If we have a timestamp column, update it to the current time */

	if (table->timestamp_field_type & TIMESTAMP_AUTO_SET_ON_UPDATE)
		table->timestamp_field->set_time();

	/* If we have an auto_increment column, update the sequence value.  */

	Field *autoInc = table->found_next_number_field;

	if ((autoInc) && bitmap_is_set(table->read_set, autoInc->field_index))
		{
		int code = storageShare->setSequenceValue(autoInc->val_int());

		if (code)
			DBUG_RETURN(error(code));
		}

	encodeRecord(newData, true);

	int ret = storageTable->updateRow(lastRecord);

	if (ret)
		{
		int code = ret >> StoreErrorIndexShift;
		indexErrorId = (ret & StoreErrorIndexMask) - 1;
		DBUG_RETURN(error(code));
		}

	ha_statistic_increment(&SSV::ha_update_count);

	DBUG_RETURN(0);
}


int StorageInterface::delete_row(const uchar* buf)
{
	DBUG_ENTER("StorageInterface::delete_row");
	DBUG_ASSERT (lastRecord >= 0);
	ha_statistic_increment(&SSV::ha_delete_count);

	int ret = storageTable->deleteRow(lastRecord);

	if (ret < 0)
		DBUG_RETURN(error(ret));

	lastRecord = -1;

	DBUG_RETURN(0);
}

int StorageInterface::commit(handlerton *hton, THD* thd, bool all)
{
	DBUG_ENTER("StorageInterface::commit");
	StorageConnection *storageConnection = getStorageConnection(thd);
	int ret = 0;

	if (all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
		{
		if (storageConnection)
			ret = storageConnection->commit();
		else
			ret = storageHandler->commit(thd);
		}
	else
		{
		if (storageConnection)
			storageConnection->releaseVerb();
		else
			ret = storageHandler->releaseVerb(thd);
		}

	if (ret != 0)
		{
		DBUG_RETURN(getMySqlError(ret));
		}

	DBUG_RETURN(0);
}

int StorageInterface::prepare(handlerton* hton, THD* thd, bool all)
{
	DBUG_ENTER("StorageInterface::prepare");

	if (all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
		storageHandler->prepare(thd, sizeof(thd->transaction.xid_state.xid), (const unsigned char*) &thd->transaction.xid_state.xid);
	else
		{
		StorageConnection *storageConnection = getStorageConnection(thd);

		if (storageConnection)
			storageConnection->releaseVerb();
		else
			storageHandler->releaseVerb(thd);
		}

	DBUG_RETURN(0);
}

int StorageInterface::rollback(handlerton *hton, THD *thd, bool all)
{
	DBUG_ENTER("StorageInterface::rollback");
	StorageConnection *storageConnection = getStorageConnection(thd);
	int ret = 0;

	if (all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
		{
		if (storageConnection)
			ret = storageConnection->rollback();
		else
			ret = storageHandler->rollback(thd);
		}
	else
		{
		if (storageConnection)
			ret = storageConnection->rollbackVerb();
		else
			ret = storageHandler->rollbackVerb(thd);
		}

	if (ret != 0)
		{
		DBUG_RETURN(getMySqlError(ret));
		}

	DBUG_RETURN(0);
}

int StorageInterface::commit_by_xid(handlerton* hton, XID* xid)
{
	DBUG_ENTER("StorageInterface::commit_by_xid");
	int ret = storageHandler->commitByXid(sizeof(XID), (const unsigned char*) xid);
	DBUG_RETURN(ret);
}

int StorageInterface::rollback_by_xid(handlerton* hton, XID* xid)
{
	DBUG_ENTER("StorageInterface::rollback_by_xid");
	int ret = storageHandler->rollbackByXid(sizeof(XID), (const unsigned char*) xid);
	DBUG_RETURN(ret);
}

int StorageInterface::start_consistent_snapshot(handlerton *hton, THD *thd)
{
	DBUG_ENTER("StorageInterface::start_consistent_snapshot");
	int ret = storageHandler->startTransaction(thd, TRANSACTION_CONSISTENT_READ);
	if (!ret)
		trans_register_ha(thd, true, hton);
	DBUG_RETURN(ret);

}

const COND* StorageInterface::cond_push(const COND* cond)
{
#ifdef COND_PUSH_ENABLED
	DBUG_ENTER("StorageInterface::cond_push");
	char buff[256];
	String str(buff,(uint32) sizeof(buff), system_charset_info);
	str.length(0);
	Item *cond_ptr= (COND *)cond;
	cond_ptr->print(&str);
	str.append('\0');
	DBUG_PRINT("StorageInterface::cond_push", ("%s", str.ptr()));
	DBUG_RETURN(0);
#else
	return handler::cond_push(cond);
#endif
}

void StorageInterface::startTransaction(void)
{
	threadSwitch(table->in_use);

	int isolation = getTransactionIsolation(mySqlThread);

	if (!storageConnection->transactionActive)
		{
		storageConnection->startTransaction(isolation);
		trans_register_ha(mySqlThread, true, falcon_hton);
		}

	switch (thd_tx_isolation(mySqlThread))
		{
		case ISO_READ_UNCOMMITTED:
			error(StorageWarningReadUncommitted);
			break;

		case ISO_SERIALIZABLE:
			error(StorageWarningSerializable);
			break;
		}
}

int StorageInterface::savepointSet(handlerton *hton, THD *thd, void *savePoint)
{
	return storageHandler->savepointSet(thd, savePoint);
}


int StorageInterface::savepointRollback(handlerton *hton, THD *thd, void *savePoint)
{
	return storageHandler->savepointRollback(thd, savePoint);
}


int StorageInterface::savepointRelease(handlerton *hton, THD *thd, void *savePoint)
{
	return storageHandler->savepointRelease(thd, savePoint);
}


int StorageInterface::index_read(uchar *buf, const uchar *keyBytes, uint key_len,
                                enum ha_rkey_function find_flag)
{
	DBUG_ENTER("StorageInterface::index_read");
	int which = 0;
	ha_statistic_increment(&SSV::ha_read_key_count);

	int ret = storageTable->checkCurrentIndex();

	if (ret)
		DBUG_RETURN(error(ret));
	
	// XXX This needs to be revisited
	switch(find_flag)
		{
		case HA_READ_KEY_EXACT:
			which = UpperBound | LowerBound;
			break;

		case HA_READ_KEY_OR_NEXT:
			storageTable->setPartialKey();
			which = LowerBound;
			break;

		case HA_READ_AFTER_KEY:     // ???
			if (!storageTable->isKeyNull((const unsigned char*) keyBytes, key_len))
				which = LowerBound;

			break;

		case HA_READ_BEFORE_KEY:    // ???
		case HA_READ_PREFIX_LAST_OR_PREV: // ???
		case HA_READ_PREFIX_LAST:   // ???
		case HA_READ_PREFIX:
		case HA_READ_KEY_OR_PREV:
		default:
			DBUG_RETURN(HA_ERR_UNSUPPORTED);
		}

	const unsigned char *key = (const unsigned char*) keyBytes;

	if (which)
		if ((ret = storageTable->setIndexBound(key, key_len, which)))
			DBUG_RETURN(error(ret));

	storageTable->clearBitmap();
	if ((ret = storageTable->indexScan(indexOrder)))
		DBUG_RETURN(error(ret));

	nextRecord = 0;

	for (;;)
		{
		int ret = index_next(buf);

		if (ret)
			DBUG_RETURN(ret);

		int comparison = storageTable->compareKey(key, key_len);

		if ((which & LowerBound) && comparison < 0)
			continue;

		if ((which & UpperBound) && comparison > 0)
			continue;

		DBUG_RETURN(0);
		}
}

int StorageInterface::index_init(uint idx, bool sorted)
{
	DBUG_ENTER("StorageInterface::index_init");
	active_index = idx;
	nextRecord = 0;
	haveStartKey = false;
	haveEndKey = false;

	// Get and hold a shared lock on StorageTableShare::indexes, then set
	// the corresponding Falcon index for use on the current thread
	
	int ret = storageTable->setCurrentIndex(idx);

	// If the index is not found, remap the index and try again
		
	if (ret)
		{
		storageShare->lockIndexes(true);
		remapIndexes(table);
		storageShare->unlockIndexes();
		
		ret = storageTable->setCurrentIndex(idx);
		
		// Online ALTER allows access to partially deleted indexes, so
		// fail silently for now to avoid fatal assert in server.
		// 
		// TODO: Restore error when server imposes DDL lock across the
		//       three phases of online ALTER.
		
		if (ret)
			//DBUG_RETURN(error(ret));
			DBUG_RETURN(error(0));
		}
		
	DBUG_RETURN(error(ret));
}

int StorageInterface::index_end(void)
{
	DBUG_ENTER("StorageInterface::index_end");
	
	storageTable->indexEnd();
	
	DBUG_RETURN(0);
}

ha_rows StorageInterface::records_in_range(uint indexId, key_range *lower, key_range *upper)
{
	DBUG_ENTER("StorageInterface::records_in_range");

#ifdef NO_OPTIMIZE
	DBUG_RETURN(handler::records_in_range(indexId, lower, upper));
#endif

	ha_rows cardinality = (ha_rows) storageShare->estimateCardinality();

	if (!lower)
		DBUG_RETURN(MAX(cardinality, 2));

	StorageIndexDesc indexDesc;

	if (!storageShare->getIndex(indexId, &indexDesc))
		DBUG_RETURN(MAX(cardinality, 2));

	int numberSegments = 0;

	for (int map = lower->keypart_map; map; map >>= 1)
		++numberSegments;

	if (indexDesc.unique && numberSegments == indexDesc.numberSegments)
		DBUG_RETURN(1);

	ha_rows segmentRecords = (ha_rows)indexDesc.segmentRecordCounts[numberSegments - 1];
	ha_rows guestimate = cardinality;

	if (lower->flag == HA_READ_KEY_EXACT)
		{
		if (segmentRecords)
			guestimate = segmentRecords;
		else
			for (int n = 0; n < numberSegments; ++n)
				guestimate /= 10;
		}
	else
		guestimate /= 3;

	DBUG_RETURN(MAX(guestimate, 2));
}

void StorageInterface::getKeyDesc(TABLE *srvTable, int indexId, StorageIndexDesc *indexDesc)
{
	KEY *keyInfo = srvTable->key_info + indexId;
	int numberKeys = keyInfo->key_parts;
	
	indexDesc->id			  = indexId;
	indexDesc->numberSegments = numberKeys;
	indexDesc->unique		  = (keyInfo->flags & HA_NOSAME);
	indexDesc->primaryKey	  = (srvTable->s->primary_key == (uint)indexId);
	
	// Clean up the index name for internal use
	
	strncpy(indexDesc->rawName, (const char*)keyInfo->name, MIN(indexNameSize, (int)strlen(keyInfo->name)+1));
	indexDesc->rawName[indexNameSize-1] = '\0';
	storageShare->createIndexName(indexDesc->rawName, indexDesc->name, indexDesc->primaryKey);
	
	for (int n = 0; n < numberKeys; ++n)
		{
		StorageSegment *segment = indexDesc->segments + n;
		KEY_PART_INFO *part = keyInfo->key_part + n;
		
		segment->offset	= part->offset;
		segment->length	= part->length;
		segment->type	= part->field->key_type();
		segment->nullBit = part->null_bit;
		segment->mysql_charset = NULL;

		// Separate correctly between types that may map to
		// the same key type, but that should be treated differently.
		// This way StorageInterface::getSegmentValue only have
		// to switch on the keyFormat, and the logic needed at runtime
		// is minimal.
		// Also set the correct charset where appropriate.
		switch (segment->type)
			{
			case HA_KEYTYPE_LONG_INT:
				segment->keyFormat = KEY_FORMAT_LONG_INT;
				break;
				
			case HA_KEYTYPE_SHORT_INT:
				segment->keyFormat = KEY_FORMAT_SHORT_INT;
				break;
				
			case HA_KEYTYPE_ULONGLONG:
				segment->keyFormat = KEY_FORMAT_ULONGLONG;
				break;

			case HA_KEYTYPE_LONGLONG:
				segment->keyFormat = KEY_FORMAT_LONGLONG;
				break;
				
			case HA_KEYTYPE_FLOAT:
				segment->keyFormat = KEY_FORMAT_FLOAT;
				break;
				
			case HA_KEYTYPE_DOUBLE:
				segment->keyFormat = KEY_FORMAT_DOUBLE;
				break;
				
			case HA_KEYTYPE_VARBINARY1:
			case HA_KEYTYPE_VARBINARY2:
				segment->keyFormat = KEY_FORMAT_VARBINARY;
				segment->mysql_charset = part->field->charset();
				break;

			case HA_KEYTYPE_VARTEXT1:
			case HA_KEYTYPE_VARTEXT2:
				segment->keyFormat = KEY_FORMAT_VARTEXT;
				segment->mysql_charset = part->field->charset();
				break;
				
			case HA_KEYTYPE_BINARY:
				switch (part->field->real_type())
					{
					case MYSQL_TYPE_TINY:
					case MYSQL_TYPE_BIT:
					case MYSQL_TYPE_YEAR:
					case MYSQL_TYPE_SET:
					case MYSQL_TYPE_ENUM:
					case MYSQL_TYPE_DATETIME:
						segment->keyFormat = KEY_FORMAT_BINARY_INTEGER;
						break;
						
					case MYSQL_TYPE_NEWDECIMAL:
						segment->keyFormat = KEY_FORMAT_BINARY_NEWDECIMAL;
						break;
						
					default:
						segment->keyFormat = KEY_FORMAT_BINARY_STRING;
						break;
					}
				break;
				
			case HA_KEYTYPE_TEXT:
				segment->keyFormat = KEY_FORMAT_TEXT;
				segment->mysql_charset = part->field->charset();
				break;
				
			case HA_KEYTYPE_ULONG_INT:
				if (part->field->real_type() == MYSQL_TYPE_TIMESTAMP)
					segment->keyFormat = KEY_FORMAT_TIMESTAMP;
				else
					segment->keyFormat = KEY_FORMAT_ULONG_INT;
				break;
				
			case HA_KEYTYPE_INT8:
				segment->keyFormat = KEY_FORMAT_INT8;
				break;
				
			case HA_KEYTYPE_USHORT_INT:
				segment->keyFormat = KEY_FORMAT_USHORT_INT;
				break;
				
			case HA_KEYTYPE_UINT24:
				segment->keyFormat = KEY_FORMAT_UINT24;
				break;
				
			case HA_KEYTYPE_INT24:
				segment->keyFormat = KEY_FORMAT_INT24;
				break;
				
			default:
				segment->keyFormat = KEY_FORMAT_OTHER;
			}
		}
}

int StorageInterface::rename_table(const char *from, const char *to)
{
	DBUG_ENTER("StorageInterface::rename_table");
	//tempTable = storageHandler->isTempTable(from) > 0;
	table = 0; // XXX hack?
	int ret = open(from, 0, 0);

	if (ret)
		DBUG_RETURN(error(ret));

	storageTable->clearCurrentIndex();
	storageShare->lockIndexes(true);
	storageShare->lock(true);

	ret = storageShare->renameTable(storageConnection, to);
	
	remapIndexes(table);
	
	storageShare->unlock();
	storageShare->unlockIndexes();

	DBUG_RETURN(error(ret));
}

double StorageInterface::read_time(uint index, uint ranges, ha_rows rows)
{
	DBUG_ENTER("StorageInterface::read_time");
	DBUG_RETURN(rows2double(rows / 3));
}

int StorageInterface::read_range_first(const key_range *start_key,
                                      const key_range *end_key,
                                      bool eq_range_arg, bool sorted)
{
        int res;
	DBUG_ENTER("StorageInterface::read_range_first");

	storageTable->clearIndexBounds();
	if ((res= scanRange(start_key, end_key, eq_range_arg)))
		DBUG_RETURN(res);

	int result = index_next(table->record[0]);

	if (result)
		{
		if (result == HA_ERR_KEY_NOT_FOUND)
			result = HA_ERR_END_OF_FILE;

		table->status = result;
		DBUG_RETURN(result);
		}

	DBUG_RETURN(0);
}


int StorageInterface::scanRange(const key_range *start_key,
								const key_range *end_key,
								bool eqRange)
{
	DBUG_ENTER("StorageInterface::read_range_first");
	haveStartKey = false;
	haveEndKey = false;
	storageTable->upperBound = storageTable->lowerBound = NULL;

	int ret = storageTable->checkCurrentIndex();

	if (ret)
		DBUG_RETURN(error(ret));
	
	if (start_key && !storageTable->isKeyNull((const unsigned char*) start_key->key, start_key->length))
		{
		haveStartKey = true;
		startKey = *start_key;

		if (start_key->flag == HA_READ_KEY_OR_NEXT)
			storageTable->setPartialKey();
		else if (start_key->flag == HA_READ_AFTER_KEY)
			storageTable->setReadAfterKey();

		ret = storageTable->setIndexBound((const unsigned char*) start_key->key,
												start_key->length, LowerBound);
		if (ret)
			DBUG_RETURN(error(ret));
		}

	if (end_key)
		{
		if (end_key->flag == HA_READ_BEFORE_KEY)
			storageTable->setReadBeforeKey();

		ret = storageTable->setIndexBound((const unsigned char*) end_key->key,
												end_key->length, UpperBound);
		if (ret)
			DBUG_RETURN(error(ret));
		}

	storageTable->indexScan(indexOrder);
	nextRecord = 0;
	lastRecord = -1;
	eq_range = eqRange;
	end_range = 0;

	if (end_key)
		{
		haveEndKey = true;
		endKey = *end_key;
		end_range = &save_end_range;
		save_end_range = *end_key;
		key_compare_result_on_equal = ((end_key->flag == HA_READ_BEFORE_KEY) ? 1 :
										(end_key->flag == HA_READ_AFTER_KEY) ? -1 :
										0);
		}

	range_key_part = table->key_info[active_index].key_part;
	DBUG_RETURN(0);
}

int StorageInterface::index_first(uchar* buf)
{
	storageTable->indexScan(indexOrder);

	return index_next(buf);
}

int StorageInterface::index_next(uchar *buf)
{
	DBUG_ENTER("StorageInterface::index_next");
	ha_statistic_increment(&SSV::ha_read_next_count);

	if (activeBlobs)
		freeActiveBlobs();

	int ret = storageTable->checkCurrentIndex();

	if (ret)
		DBUG_RETURN(error(ret));
	
	for (;;)
		{
		lastRecord = storageTable->nextIndexed(nextRecord, lockForUpdate);

		if (lastRecord < 0)
			{
			if (lastRecord == StorageErrorRecordNotFound)
				{
				lastRecord = -1;
				table->status = STATUS_NOT_FOUND;
				DBUG_RETURN(HA_ERR_END_OF_FILE);
				}

			DBUG_RETURN(error(lastRecord));
			}

		nextRecord = lastRecord + 1;

		if (haveStartKey)
			{
			int n = storageTable->compareKey((const unsigned char*) startKey.key, startKey.length);

			if (n < 0 || (n == 0 && startKey.flag == HA_READ_AFTER_KEY))
				{
				storageTable->unlockRow();
				continue;
				}
			}

		if (haveEndKey)
			{
			int n = storageTable->compareKey((const unsigned char*) endKey.key, endKey.length);

			if (n > 0 || (n == 0 && endKey.flag == HA_READ_BEFORE_KEY))
				{
				storageTable->unlockRow();
				continue;
				}
			}

		decodeRecord(buf);
		table->status = 0;

		DBUG_RETURN(0);
		}
}

int StorageInterface::index_next_same(uchar *buf, const uchar *key, uint key_len)
{
	DBUG_ENTER("StorageInterface::index_next_same");
	ha_statistic_increment(&SSV::ha_read_next_count);

	for (;;)
		{
		int ret = index_next(buf);

		if (ret)
			DBUG_RETURN(ret);

		int comparison = storageTable->compareKey((const unsigned char*) key, key_len);

		if (comparison == 0)
			DBUG_RETURN(0);
		}
}


//*****************************************************************************
// Falcon MRR implementation: One-sweep DS-MRR
//
// Overview
//  - MRR scan is always done in one pass, which consists of two steps:
//      1. Scan the index and populate Falcon's internal record number bitmap
//      2. Scan the bitmap, retrieve and return records
//    (without MRR falcon does steps 1 and 2 for each range)
//  - The record number bitmap is allocated using Falcon's internal
//    allocation routines, so we're not using the SQL layer's join buffer space.
//  - multi_range_read_next() may return "garbage" records - records that are
//    outside of any of the scanned ranges. Filtering out these records is
//    the responsibility of whoever is making MRR calls.
//
//*****************************************************************************

int StorageInterface::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                                            uint n_ranges, uint mode, 
                                            HANDLER_BUFFER *buf)
{
	DBUG_ENTER("StorageInterface::multi_range_read_init");
	if (mode & HA_MRR_USE_DEFAULT_IMPL)
		{
		useDefaultMrrImpl= true;
		int res = handler::multi_range_read_init(seq, seq_init_param, n_ranges, 
												 mode, buf);
		DBUG_RETURN(res);
		}
	useDefaultMrrImpl = false;
	multi_range_buffer = buf;
	mrr_funcs = *seq;

	mrr_iter = mrr_funcs.init(seq_init_param, n_ranges, mode);
	fillMrrBitmap();

	// multi_range_read_next() will be calling index_next(). The following is
	// to make index_next() not to check whether the retrieved record is in 
	// range
	haveStartKey = haveEndKey = 0;
	DBUG_RETURN(0);
}


int StorageInterface::fillMrrBitmap()
{
	int res;
	key_range *startKey;
	key_range *endKey;
	DBUG_ENTER("StorageInterface::fillMrrBitmap");

	storageTable->clearBitmap();
	while (!(res = mrr_funcs.next(mrr_iter, &mrr_cur_range)))
		{
		startKey = mrr_cur_range.start_key.keypart_map? &mrr_cur_range.start_key: NULL;
		endKey   = mrr_cur_range.end_key.keypart_map?   &mrr_cur_range.end_key: NULL;
		if ((res = scanRange(startKey, endKey, test(mrr_cur_range.range_flag & EQ_RANGE))))
			DBUG_RETURN(res);
		}
	DBUG_RETURN(0);
}

int StorageInterface::multi_range_read_next(char **rangeInfo)
{
	if (useDefaultMrrImpl)
		return handler::multi_range_read_next(rangeInfo);
	return index_next(table->record[0]);
}


ha_rows StorageInterface::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
													  void *seq_init_param,
													  uint n_ranges, uint *bufsz,
													  uint *flags, COST_VECT *cost)
{
	ha_rows res;
	// TODO: SergeyP: move the optimizer_use_mrr check from here out to the
	// SQL layer, do same for all other MRR implementations
	bool native_requested = test(*flags & HA_MRR_USE_DEFAULT_IMPL ||
								 (current_thd->variables.optimizer_use_mrr == 2));
	res = handler::multi_range_read_info_const(keyno, seq, seq_init_param, n_ranges, bufsz,
											   flags, cost);
	if ((res != HA_POS_ERROR) && !native_requested)
		{
		*flags &= ~(HA_MRR_USE_DEFAULT_IMPL | HA_MRR_SORTED);
		/* We'll be returning records without telling which range they are contained in */
		*flags |= HA_MRR_NO_ASSOCIATION;
		/* We'll use our own internal buffer so we won't need any buffer space from the SQL layer */
		*bufsz = 0;
		}
	return res;
}


ha_rows StorageInterface::multi_range_read_info(uint keyno, uint n_ranges, 
												uint keys, uint *bufsz, 
												uint *flags, COST_VECT *cost)
{
	ha_rows res;
	bool native_requested = test(*flags & HA_MRR_USE_DEFAULT_IMPL || 
								 (current_thd->variables.optimizer_use_mrr == 2));
	res = handler::multi_range_read_info(keyno, n_ranges, keys, bufsz, flags,
										 cost);
	if ((res != HA_POS_ERROR) && !native_requested)
		{
		*flags &= ~(HA_MRR_USE_DEFAULT_IMPL | HA_MRR_SORTED);
		/* See _info_const() function for explanation of these: */
		*flags |= HA_MRR_NO_ASSOCIATION;
		*bufsz = 0;
		}
	return res;
}

////////////////////////////////////////////////////////////////////////////////////

double StorageInterface::scan_time(void)
{
	DBUG_ENTER("StorageInterface::scan_time");
	DBUG_RETURN((double)stats.records * 1000);
}

bool StorageInterface::threadSwitch(THD* newThread)
{
	if (newThread == mySqlThread)
		return false;

	if (storageConnection)
		{
		if (!storageConnection->mySqlThread && !mySqlThread)
			{
			storageConnection->setMySqlThread(newThread);
			mySqlThread = newThread;

			return false;
			}

		storageConnection->release();
		}

	storageConnection = storageHandler->getStorageConnection(storageShare, newThread, newThread->thread_id, OpenDatabase);

	if (storageTable)
		storageTable->setConnection(storageConnection);
	else
		storageTable = storageConnection->getStorageTable(storageShare);

	mySqlThread = newThread;

	return true;
}

int StorageInterface::threadSwitchError(void)
{
	return 1;
}

int StorageInterface::error(int storageError)
{
	DBUG_ENTER("StorageInterface::error");

	if (storageError == 0)
		{
		DBUG_PRINT("info", ("returning 0"));
		DBUG_RETURN(0);
		}

	int mySqlError = getMySqlError(storageError);
	
	switch (storageError)
		{
		case StorageErrorNoSequence:
			if (storageConnection)
				storageConnection->setErrorText("no sequenced defined for autoincrement operation");

			break;

		case StorageWarningSerializable:
			push_warning_printf(current_thd, MYSQL_ERROR::WARN_LEVEL_WARN,
			                    ER_CANT_CHANGE_TX_ISOLATION,
			                    "Falcon does not support SERIALIZABLE ISOLATION, using REPEATABLE READ instead.");
			break;

		case StorageWarningReadUncommitted:
			push_warning_printf(current_thd, MYSQL_ERROR::WARN_LEVEL_WARN,
			                    ER_CANT_CHANGE_TX_ISOLATION,
			                    "Falcon does not support READ UNCOMMITTED ISOLATION, using REPEATABLE READ instead.");
			break;

		case StorageErrorIndexOverflow:
			my_error(ER_TOO_LONG_KEY, MYF(0), max_key_length());
			break;

		default:
			;
		}

	DBUG_RETURN(mySqlError);
}

int StorageInterface::getMySqlError(int storageError)
{
	switch (storageError)
		{
		case 0:
			return 0;
			
		case StorageErrorDupKey:
			DBUG_PRINT("info", ("StorageErrorDupKey"));
			return (HA_ERR_FOUND_DUPP_KEY);

		case StorageErrorDeadlock:
			DBUG_PRINT("info", ("StorageErrorDeadlock"));
			return (HA_ERR_LOCK_DEADLOCK);

		case StorageErrorLockTimeout:
			DBUG_PRINT("info", ("StorageErrorLockTimeout"));
			return (HA_ERR_LOCK_WAIT_TIMEOUT);

		case StorageErrorRecordNotFound:
			DBUG_PRINT("info", ("StorageErrorRecordNotFound"));
			return (HA_ERR_KEY_NOT_FOUND);

		case StorageErrorTableNotFound:
			DBUG_PRINT("info", ("StorageErrorTableNotFound"));
			return (HA_ERR_NO_SUCH_TABLE);

		case StorageErrorNoIndex:
			DBUG_PRINT("info", ("StorageErrorNoIndex"));
			return (HA_ERR_WRONG_INDEX);

		case StorageErrorBadKey:
			DBUG_PRINT("info", ("StorageErrorBadKey"));
			return (HA_ERR_WRONG_INDEX);

		case StorageErrorTableExits:
			DBUG_PRINT("info", ("StorageErrorTableExits"));
			return (HA_ERR_TABLE_EXIST);

		case StorageErrorUpdateConflict:
			DBUG_PRINT("info", ("StorageErrorUpdateConflict"));
			return (HA_ERR_RECORD_CHANGED);

		case StorageErrorUncommittedUpdates:
			DBUG_PRINT("info", ("StorageErrorUncommittedUpdates"));
			return (HA_ERR_LOCK_OR_ACTIVE_TRANSACTION);

		case StorageErrorUncommittedRecords:
			DBUG_PRINT("info", ("StorageErrorUncommittedRecords"));
			return (200 - storageError);

		case StorageErrorNoSequence:
			DBUG_PRINT("info", ("StorageErrorNoSequence"));
			return (200 - storageError);

		case StorageErrorTruncation:
			DBUG_PRINT("info", ("StorageErrorTruncation"));
			return (HA_ERR_TO_BIG_ROW);
			
		case StorageErrorTableSpaceNotExist:
			DBUG_PRINT("info", ("StorageErrorTableSpaceNotExist"));
			return (HA_ERR_NO_SUCH_TABLESPACE);

		case StorageErrorTableSpaceExist:
			DBUG_PRINT("info", ("StorageErrorTableSpaceExist"));
			return (HA_ERR_TABLESPACE_EXIST);

		case StorageErrorDeviceFull:
			DBUG_PRINT("info", ("StorageErrorDeviceFull"));
			return (HA_ERR_RECORD_FILE_FULL);
			
		case StorageWarningSerializable:
			return (0);

		case StorageWarningReadUncommitted:
			return (0);

		case StorageErrorOutOfMemory:
			DBUG_PRINT("info", ("StorageErrorOutOfMemory"));
			return (HA_ERR_OUT_OF_MEM);

		case StorageErrorOutOfRecordMemory:
			DBUG_PRINT("info", ("StorageErrorOutOfRecordMemory"));
			return (200 - storageError);

		case StorageErrorTableNotEmpty:
			DBUG_PRINT("info", ("StorageErrorTableNotEmpty"));
			return HA_ERR_TABLESPACE_NOT_EMPTY;

		case StorageErrorTableSpaceDataFileExist:
			DBUG_PRINT("info", ("StorageErrorTableSpaceDataFileExist"));
			return (HA_ERR_TABLESPACE_DATAFILE_EXIST);

		case StorageErrorIOErrorSerialLog:
			DBUG_PRINT("info", ("StorageErrorIOErrorSerialLog"));
			return (HA_ERR_LOGGING_IMPOSSIBLE);

		default:
			DBUG_PRINT("info", ("Unknown Falcon Error"));
			return (200 - storageError);
		}
}

int StorageInterface::start_stmt(THD *thd, thr_lock_type lock_type)
{
	DBUG_ENTER("StorageInterface::start_stmt");
	threadSwitch(thd);

	if (storageConnection->markVerb())
		trans_register_ha(thd, FALSE, falcon_hton);

	DBUG_RETURN(0);
}

int StorageInterface::reset()
{
	DBUG_ENTER("StorageInterface::start_stmt");
	indexOrder = false;

	DBUG_RETURN(0);
}

int StorageInterface::external_lock(THD *thd, int lock_type)
{
	DBUG_ENTER("StorageInterface::external_lock");
	threadSwitch(thd);

	if (lock_type == F_UNLCK)
		{
		int ret = 0;

		storageConnection->setCurrentStatement(NULL);

		if (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
			ret = storageConnection->endImplicitTransaction();
		else
			storageConnection->releaseVerb();

		if (storageTable)
			{
			storageTable->clearStatement();
			storageTable->clearCurrentIndex();
			}

		if (ret)
			DBUG_RETURN(error(ret));
		}
	else
		{
		if (storageConnection && thd->query)
			storageConnection->setCurrentStatement(thd->query);

		insertCount = 0;

		switch (thd_sql_command(thd))
			{
			case SQLCOM_TRUNCATE:
			case SQLCOM_DROP_TABLE:
			case SQLCOM_ALTER_TABLE:
			case SQLCOM_DROP_INDEX:
			case SQLCOM_CREATE_INDEX:
				{
				int ret = storageTable->alterCheck();

				if (ret)
					{
					if (storageTable)
						storageTable->clearCurrentIndex();
						
					DBUG_RETURN(error(ret));
					}
				}
				storageTable->waitForWriteComplete();
				break;
			default:
				break;
			}
			
		int isolation = getTransactionIsolation(thd);
		
		if (thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
			{
			checkBinLog();
			
			if (storageConnection->startTransaction(isolation))
				trans_register_ha(thd, true, falcon_hton);

			if (storageConnection->markVerb())
				trans_register_ha(thd, false, falcon_hton);
			}
		else
			{
			checkBinLog();
			
			if (storageConnection->startImplicitTransaction(isolation))
				trans_register_ha(thd, false, falcon_hton);
			}

		switch (thd_tx_isolation(mySqlThread))
			{
			case ISO_READ_UNCOMMITTED:
				error(StorageWarningReadUncommitted);
				break;

			case ISO_SERIALIZABLE:
				error(StorageWarningSerializable);
				break;
			}
		}

	DBUG_RETURN(0);
}

void StorageInterface::get_auto_increment(ulonglong offset, ulonglong increment,
                                         ulonglong nb_desired_values,
                                         ulonglong *first_value,
                                         ulonglong *nb_reserved_values)
{
	DBUG_ENTER("StorageInterface::get_auto_increment");
	*first_value = storageShare->getSequenceValue(1);
	*nb_reserved_values = 1;

	DBUG_VOID_RETURN;
}

const char *StorageInterface::index_type(uint key_number)
{
  DBUG_ENTER("StorageInterface::index_type");
  DBUG_RETURN("BTREE");
}

void StorageInterface::dropDatabase(handlerton *hton, char *path)
{
	DBUG_ENTER("StorageInterface::dropDatabase");
	storageHandler->dropDatabase(path);

	DBUG_VOID_RETURN;
}

void StorageInterface::freeActiveBlobs(void)
{
	for (StorageBlob *blob; (blob = activeBlobs); )
		{
		activeBlobs = blob->next;
		storageTable->freeBlob(blob);
		blob->next = freeBlobs;
		freeBlobs = blob;
		}
}

void StorageInterface::shutdown(handlerton *htons)
{
	falcon_deinit(0);
}

int StorageInterface::panic(handlerton* hton, ha_panic_function flag)
{
	return falcon_deinit(0);
}

int StorageInterface::closeConnection(handlerton *hton, THD *thd)
{
	DBUG_ENTER("NfsStorageEngine::closeConnection");
	storageHandler->closeConnections(thd);
	*thd_ha_data(thd, hton) = NULL;

	DBUG_RETURN(0);
}

int StorageInterface::alter_tablespace(handlerton* hton, THD* thd, st_alter_tablespace* ts_info)
{
	DBUG_ENTER("NfsStorageEngine::alter_tablespace");
	int ret = 0;
	const char *data_file_name= ts_info->data_file_name;
	char buff[FN_REFLEN];
	/*
	CREATE TABLESPACE tablespace
		ADD DATAFILE 'file'
		USE LOGFILE GROUP logfile_group         // NDB only
		[EXTENT_SIZE [=] extent_size]           // Not supported
		[INITIAL_SIZE [=] initial_size]         // Not supported
		[AUTOEXTEND_SIZE [=] autoextend_size]   // Not supported
		[MAX_SIZE [=] max_size]                 // Not supported
		[NODEGROUP [=] nodegroup_id]            // NDB only
		[WAIT]                                  // NDB only
		[COMMENT [=] comment_text]
		ENGINE [=] engine


	Parameters EXTENT_SIZE, INITIAL,SIZE, AUTOEXTEND_SIZE and MAX_SIZE are
	currently not supported by Falcon. LOGFILE GROUP, NODEGROUP and WAIT are
	for NDB only.
	*/

	if (data_file_name)
		{
		size_t length= strlen(data_file_name);
		if (length <= 4 || strcmp(data_file_name + length - 4, ".fts"))
			{
			if (!length || length > FN_REFLEN - 5)
				{
				my_error(ER_BAD_PATH, MYF(0), data_file_name);
				DBUG_RETURN(1);
				}
			memcpy(buff, data_file_name, length);
			buff[length]= '.';
			buff[length + 1]= 'f';
			buff[length + 2]= 't';
			buff[length + 3]= 's';
			buff[length + 4]= '\0';
			data_file_name= buff;
			}
		}

	switch (ts_info->ts_cmd_type)
		{
		case CREATE_TABLESPACE:
			ret = storageHandler->createTablespace(	ts_info->tablespace_name, data_file_name, ts_info->ts_comment);
			break;

		case DROP_TABLESPACE:
			ret = storageHandler->deleteTablespace(ts_info->tablespace_name);
			break;

		default:
			DBUG_RETURN(HA_ADMIN_NOT_IMPLEMENTED);
		}

	DBUG_RETURN(getMySqlError(ret));
}

int StorageInterface::fill_is_table(handlerton *hton, THD *thd, TABLE_LIST *tables, class Item *cond, enum enum_schema_tables schema_table_idx)
{

	InfoTableImpl infoTable(thd, tables, system_charset_info);

	if (!storageHandler)
		return 0;

	switch (schema_table_idx)
		{
		case SCH_TABLESPACES:
			storageHandler->getTableSpaceInfo(&infoTable);
			break;
		case SCH_FILES:
			storageHandler->getTableSpaceFilesInfo(&infoTable);
			break;
		default:
			return 0;
		}

	return infoTable.error;
}

int StorageInterface::check_if_supported_alter(TABLE *altered_table, HA_CREATE_INFO *create_info, HA_ALTER_FLAGS *alter_flags, uint table_changes)
{
	DBUG_ENTER("StorageInterface::check_if_supported_alter");
	tempTable = (create_info->options & HA_LEX_CREATE_TMP_TABLE) ? true : false;
	HA_ALTER_FLAGS supported;
	supported = supported | HA_ADD_INDEX | HA_DROP_INDEX | HA_ADD_UNIQUE_INDEX | HA_DROP_UNIQUE_INDEX
	                      | HA_ADD_PK_INDEX | HA_DROP_PK_INDEX | HA_ADD_COLUMN;
	HA_ALTER_FLAGS notSupported = ~(supported);
	
#ifndef ONLINE_ALTER
	DBUG_RETURN(HA_ALTER_NOT_SUPPORTED);
#endif	

	if (tempTable || (*alter_flags & notSupported).is_set())
		DBUG_RETURN(HA_ALTER_NOT_SUPPORTED);

	if (alter_flags->is_set(HA_ADD_COLUMN))
		{
		Field *field = NULL;
		
		for (uint i = 0; i < altered_table->s->fields; i++)
			{
			field = altered_table->s->field[i];
			bool found = false;
			
			for (uint n = 0; n < table->s->fields; n++)
				if (found = (strcmp(table->s->field[n]->field_name, field->field_name) == 0))
					break;
		
			if (field && !found)
				if (!field->real_maybe_null())
					{
					DBUG_PRINT("info",("Online add column must be nullable"));
					DBUG_RETURN(HA_ALTER_NOT_SUPPORTED);
					}
			}
		}
		
	DBUG_RETURN(HA_ALTER_SUPPORTED_NO_LOCK);
}

// Prepare for online ALTER

int StorageInterface::alter_table_phase1(THD* thd, TABLE* altered_table, HA_CREATE_INFO* create_info, HA_ALTER_INFO* alter_info, HA_ALTER_FLAGS* alter_flags)
{
	DBUG_ENTER("StorageInterface::alter_table_phase1");

	DBUG_RETURN(0);
}

// Perform the online ALTER

int StorageInterface::alter_table_phase2(THD* thd, TABLE* altered_table, HA_CREATE_INFO* create_info, HA_ALTER_INFO* alter_info, HA_ALTER_FLAGS* alter_flags)
{
	DBUG_ENTER("StorageInterface::alter_table_phase2");

	int ret = 0;
	
	if (alter_flags->is_set(HA_ADD_COLUMN))
		ret = addColumn(thd, altered_table, create_info, alter_info, alter_flags);
		
	if ((alter_flags->is_set(HA_ADD_INDEX) || alter_flags->is_set(HA_ADD_UNIQUE_INDEX) || alter_flags->is_set(HA_ADD_PK_INDEX)) && !ret)
		ret = addIndex(thd, altered_table, create_info, alter_info, alter_flags);
		
	if ((alter_flags->is_set(HA_DROP_INDEX) || alter_flags->is_set(HA_DROP_UNIQUE_INDEX) || alter_flags->is_set(HA_DROP_PK_INDEX)) && !ret)
		ret = dropIndex(thd, altered_table, create_info, alter_info, alter_flags);
	
	DBUG_RETURN(ret);
}

// Notification that changes are written and table re-opened

int StorageInterface::alter_table_phase3(THD* thd, TABLE* altered_table)
{
	DBUG_ENTER("StorageInterface::alter_table_phase3");
	
	DBUG_RETURN(0);
}

int StorageInterface::addColumn(THD* thd, TABLE* alteredTable, HA_CREATE_INFO* createInfo, HA_ALTER_INFO* alterInfo, HA_ALTER_FLAGS* alterFlags)
{
	int ret;
	int64 incrementValue = 0;
	/***
	const char *tableName = storageTable->getName();
	const char *schemaName = storageTable->getSchemaName();
	***/
	CmdGen gen;
	genTable(alteredTable, &gen);
	
	/***
	if (alteredTable->found_next_number_field)
		{
		incrementValue = alterInfo->auto_increment_value;

		if (incrementValue == 0)
			incrementValue = 1;
		}
	***/
	
	/***
	if (alteredTable->s->primary_key < alteredTable->s->keys)
		{
		KEY *key = alteredTable->key_info + alteredTable->s->primary_key;
		gen.gen(",\n  primary key ");
		genKeyFields(key, &gen);
		}

	gen.gen (")");
	***/
	
	if ((ret = storageTable->upgrade(gen.getString(), incrementValue)))
		return (error(ret));

	return 0;
}

int StorageInterface::addIndex(THD* thd, TABLE* alteredTable, HA_CREATE_INFO* createInfo, HA_ALTER_INFO* alterInfo, HA_ALTER_FLAGS* alterFlags)
{
	int ret = 0;
	const char *tableName = storageTable->getName();
	const char *schemaName = storageTable->getSchemaName();

	// Lock out other clients before locking the table
	
	storageShare->lockIndexes(true);
	storageShare->lock(true);

	// Find indexes to be added by comparing table and alteredTable

	for (unsigned int n = 0; n < alteredTable->s->keys; n++)
		{
		KEY *key = alteredTable->key_info + n;
		KEY *tableEnd = table->key_info + table->s->keys;
		KEY *tableKey;
			
		for (tableKey = table->key_info; tableKey < tableEnd; tableKey++)
			if (!strcmp(tableKey->name, key->name))
				break;
					
		if (tableKey >= tableEnd)
			if ((ret = createIndex(schemaName, tableName, alteredTable, n)))
				break;
		}
		
	// The server indexes may have been reordered, so remap to the Falcon indexes
	
	remapIndexes(alteredTable);
	
	storageShare->unlock();
	storageShare->unlockIndexes();
	
	return error(ret);
}

int StorageInterface::dropIndex(THD* thd, TABLE* alteredTable, HA_CREATE_INFO* createInfo, HA_ALTER_INFO* alterInfo, HA_ALTER_FLAGS* alterFlags)
{
	int ret = 0;
	const char *tableName = storageTable->getName();
	const char *schemaName = storageTable->getSchemaName();
	
	// Lock out other clients before locking the table
	
	storageShare->lockIndexes(true);
	storageShare->lock(true);
	
	// Find indexes to be dropped by comparing table and alteredTable
	
	for (unsigned int n = 0; n < table->s->keys; n++)
		{
		KEY *key = table->key_info + n;
		KEY *alterEnd = alteredTable->key_info + alteredTable->s->keys;
		KEY *alterKey;
		
		for (alterKey = alteredTable->key_info; alterKey < alterEnd; alterKey++)
			if (!strcmp(alterKey->name, key->name))
				break;

		if (alterKey >= alterEnd)
			if ((ret = dropIndex(schemaName, tableName, table, n, true)))
				break;
		}
	
	// The server indexes have been reordered, so remap to the Falcon indexes
	
	remapIndexes(alteredTable);
	
	storageShare->unlock();
	storageShare->unlockIndexes();
	
	return error(ret);
}

uint StorageInterface::max_supported_key_length(void) const
{
	// Assume 4K page unless proven otherwise.
	if (storageConnection)
		return storageConnection->getMaxKeyLength();

	return MAX_INDEX_KEY_LENGTH_4K;  // Default value.
}

uint StorageInterface::max_supported_key_part_length(void) const
{
	// Assume 4K page unless proven otherwise.
	if (storageConnection)
		return storageConnection->getMaxKeyLength();

	return MAX_INDEX_KEY_LENGTH_4K;  // Default for future sizes.
}

void StorageInterface::logger(int mask, const char* text, void* arg)
{
	if (mask & falcon_debug_mask)
		{
		printf("%s", text);
		fflush(stdout);

		if (falcon_log_file)
			{
			fprintf(falcon_log_file, "%s", text);
			fflush(falcon_log_file);
			}
		}
}

void StorageInterface::mysqlLogger(int mask, const char* text, void* arg)
{
	if (mask & LogMysqlError)
		sql_print_error("%s", text);
	else if (mask & LogMysqlWarning)
		sql_print_warning("%s", text);
	else if (mask & LogMysqlInfo)
		sql_print_information("%s", text);
}

int StorageInterface::setIndex(TABLE *srvTable, int indexId)
{
	StorageIndexDesc indexDesc;
	getKeyDesc(srvTable, indexId, &indexDesc);

	return storageTable->setIndex(&indexDesc);
}

int StorageInterface::setIndexes(TABLE *srvTable)
{
	int ret = 0;
	
	if (!srvTable || storageShare->haveIndexes(srvTable->s->keys))
		return ret;

	storageShare->lockIndexes(true);
	storageShare->lock(true);

	ret = remapIndexes(srvTable);

	storageShare->unlock();
	storageShare->unlockIndexes();
	
	return ret;
}

// Create an index entry in StorageTableShare for each TABLE index
// Assume exclusive lock on StorageTableShare::syncIndexMap

int StorageInterface::remapIndexes(TABLE *srvTable)
{
	int ret = 0;
	
	storageShare->deleteIndexes();

	if (!srvTable)
		return ret;
		
	// Ok to ignore errors in this context
	
	for (uint n = 0; n < srvTable->s->keys; ++n)
		setIndex(srvTable, n);

	// validateIndexes(srvTable, true);
	
	return ret;
}

bool StorageInterface::validateIndexes(TABLE *srvTable, bool exclusiveLock)
{
	bool ret = true;
	
	if (!srvTable)
		return false;
	
	storageShare->lockIndexes(exclusiveLock);
		
	for (uint n = 0; (n < srvTable->s->keys) && ret; ++n)
		{
		StorageIndexDesc indexDesc;
		getKeyDesc(srvTable, n, &indexDesc);
		
		if (!storageShare->validateIndex(n, &indexDesc))
			ret = false;
		}
	
	if (ret && (srvTable->s->keys != (uint)storageShare->numberIndexes()))
		ret = false;
	
	storageShare->unlockIndexes();

	return ret;
}

int StorageInterface::genTable(TABLE* srvTable, CmdGen* gen)
{
	const char *tableName = storageTable->getName();
	const char *schemaName = storageTable->getSchemaName();
	gen->gen("upgrade table \"%s\".\"%s\" (\n", schemaName, tableName);
	const char *sep = "";
	char nameBuffer[256];

	for (uint n = 0; n < srvTable->s->fields; ++n)
		{
		Field *field = srvTable->field[n];
		CHARSET_INFO *charset = field->charset();

		if (charset)
			storageShare->registerCollation(charset->name, charset);

		storageShare->cleanupFieldName(field->field_name, nameBuffer, sizeof(nameBuffer), true);
		gen->gen("%s  \"%s\" ", sep, nameBuffer);
		int ret = genType(field, gen);

		if (ret)
			return (ret);

		if (!field->maybe_null())
			gen->gen(" not null");

		sep = ",\n";
		}

	if (srvTable->s->primary_key < srvTable->s->keys)
		{
		KEY *key = srvTable->key_info + srvTable->s->primary_key;
		gen->gen(",\n  primary key ");
		genKeyFields(key, gen);
		}

#if 0		
	// Disable until UPGRADE TABLE supports index syntax
	
	for (unsigned int n = 0; n < srvTable->s->keys; n++)
		{
		if (n != srvTable->s->primary_key)
			{
			KEY *key = srvTable->key_info + n;
			const char *unique = (key->flags & HA_NOSAME) ? "unique " : "";
			gen->gen(",\n  %s key ", unique);
			genKeyFields(key, gen);
			}
		}
#endif		

	gen->gen (")");

	return 0;
}

int StorageInterface::genType(Field* field, CmdGen* gen)
{
	const char *type;
	const char *arg = NULL;
	int length = 0;

	switch (field->real_type())
		{
		case MYSQL_TYPE_DECIMAL:
		case MYSQL_TYPE_TINY:
		case MYSQL_TYPE_SHORT:
		case MYSQL_TYPE_BIT:
			type = "smallint";
			break;

		case MYSQL_TYPE_INT24:
		case MYSQL_TYPE_LONG:
		case MYSQL_TYPE_YEAR:
			type = "int";
			break;

		case MYSQL_TYPE_FLOAT:
			type = "float";
			break;

		case MYSQL_TYPE_DOUBLE:
			type = "double";
			break;

		case MYSQL_TYPE_TIMESTAMP:
			type = "timestamp";
			break;

		case MYSQL_TYPE_SET:
		case MYSQL_TYPE_LONGLONG:
			type = "bigint";
			break;

		/*
			Falcon's date and time types don't handle invalid dates like MySQL's do,
			so we just use an int for storage
		*/

		case MYSQL_TYPE_DATE:
		case MYSQL_TYPE_TIME:
		case MYSQL_TYPE_ENUM:
		case MYSQL_TYPE_NEWDATE:
			type = "int";
			break;

		case MYSQL_TYPE_DATETIME:
			type = "bigint";
			break;

		case MYSQL_TYPE_VARCHAR:
		case MYSQL_TYPE_VAR_STRING:
		case MYSQL_TYPE_STRING:
			{
			CHARSET_INFO *charset = field->charset();

			if (charset)
				{
				arg = charset->name;
				type = "varchar (%d) collation %s";
				}
			else
				type = "varchar (%d)";

			length = field->field_length;
			}
			break;

		case MYSQL_TYPE_TINY_BLOB:
		case MYSQL_TYPE_LONG_BLOB:
		case MYSQL_TYPE_BLOB:
		case MYSQL_TYPE_MEDIUM_BLOB:
		case MYSQL_TYPE_GEOMETRY:
			if (field->field_length < 256)
				type = "varchar (256)";
			else
				type = "blob";
			break;

		case MYSQL_TYPE_NEWDECIMAL:
			{
			Field_new_decimal *newDecimal = (Field_new_decimal*) field;

			/***
			if (newDecimal->precision > 18 && newDecimal->dec > 9)
				{
				errorText = "columns with greater than 18 digits precision and greater than 9 digits of fraction are not supported";

				return HA_ERR_UNSUPPORTED;
				}
			***/

			gen->gen("numeric (%d,%d)", newDecimal->precision, newDecimal->dec);

			return 0;
			}

		default:
			errorText = "unsupported Falcon data type";

			return HA_ERR_UNSUPPORTED;
		}

	gen->gen(type, length, arg);

	return 0;
}

void StorageInterface::genKeyFields(KEY* key, CmdGen* gen)
{
	const char *sep = "(";
	char nameBuffer[256];

	for (uint n = 0; n < key->key_parts; ++n)
		{
		KEY_PART_INFO *part = key->key_part + n;
		Field *field = part->field;
		storageShare->cleanupFieldName(field->field_name, nameBuffer, sizeof(nameBuffer), true);

		if (part->key_part_flag & HA_PART_KEY_SEG)
			gen->gen("%s\"%s\"(%d)", sep, nameBuffer, part->length);
		else
			gen->gen("%s\"%s\"", sep, nameBuffer);

		sep = ", ";
		}

	gen->gen(")");
}

void StorageInterface::encodeRecord(uchar *buf, bool updateFlag)
{
	storageTable->preInsert();
	my_ptrdiff_t ptrDiff = buf - table->record[0];
	my_bitmap_map *old_map = dbug_tmp_use_all_columns(table, table->read_set);
	EncodedDataStream *dataStream = &storageTable->dataStream;
	FieldFormat *fieldFormat = storageShare->format->format;
	int maxId = storageShare->format->maxId;
	
	for (int n = 0; n < maxId; ++n, ++fieldFormat)
		{
		if (fieldFormat->fieldId < 0 || fieldFormat->offset == 0)
			continue;
			
		Field *field = fieldMap[fieldFormat->fieldId];
		ASSERT(field);
		
		if (ptrDiff)
			field->move_field_offset(ptrDiff);

		if (updateFlag && !bitmap_is_set(table->write_set, field->field_index))
			{
			const unsigned char *p = storageTable->getEncoding(n);
			dataStream->encodeEncoding(p);
			}
		else if (field->is_null())
			dataStream->encodeNull();
		else
			switch (field->real_type())
				{
				case MYSQL_TYPE_TINY:
				case MYSQL_TYPE_SHORT:
				case MYSQL_TYPE_INT24:
				case MYSQL_TYPE_LONG:
				case MYSQL_TYPE_DECIMAL:
				case MYSQL_TYPE_ENUM:
				case MYSQL_TYPE_SET:
				case MYSQL_TYPE_BIT:
					dataStream->encodeInt64(field->val_int());
					break;

				case MYSQL_TYPE_LONGLONG:
					{
					int64 temp = field->val_int();

					// If the field is unsigned and the MSB is set, 
					// encode it as a BigInt to support unsigned values 
					// with the MSB set in the index

					if (((Field_num*)field)->unsigned_flag && (temp & 0x8000000000000000ULL))
						{
						BigInt bigInt;
						bigInt.set((uint64)temp);
						dataStream->encodeBigInt(&bigInt);
						}
					else
						{
						dataStream->encodeInt64(temp);
						}

					}
					break;

				case MYSQL_TYPE_YEAR:
					// Have to use the ptr directly to get the same number for
					// both two and four digit YEAR
					dataStream->encodeInt64((int) field->ptr[0]);
					break;

				case MYSQL_TYPE_NEWDECIMAL:
					{
					int precision = ((Field_new_decimal *)field)->precision;
					int scale = ((Field_new_decimal *)field)->dec;

					if (precision < 19)
						{
						int64 value = ScaledBinary::getInt64FromBinaryDecimal((const char *) field->ptr,
																			precision,
																			scale);
						dataStream->encodeInt64(value, scale);
						}
					else
						{
						BigInt bigInt;
						ScaledBinary::getBigIntFromBinaryDecimal((const char*) field->ptr, precision, scale, &bigInt);

						// Handle value as int64 if possible. Even if the number fits
						// an int64, it can only be scaled within 18 digits or less.

						if (bigInt.fitsInInt64() && scale < 19)
							{
							int64 value = bigInt.getInt();
							dataStream->encodeInt64(value, scale);
							}
						else
							dataStream->encodeBigInt(&bigInt);
						}
					}
					break;

				case MYSQL_TYPE_DOUBLE:
				case MYSQL_TYPE_FLOAT:
					dataStream->encodeDouble(field->val_real());
					break;

				case MYSQL_TYPE_TIMESTAMP:
					{
					my_bool nullValue;
					int64 value = ((Field_timestamp*) field)->get_timestamp(&nullValue);
					dataStream->encodeDate(value * 1000);
					}
					break;

				case MYSQL_TYPE_DATE:
					dataStream->encodeInt64(field->val_int());
					break;

				case MYSQL_TYPE_NEWDATE:
					//dataStream->encodeInt64(field->val_int());
					dataStream->encodeInt64(uint3korr(field->ptr));
					break;

				case MYSQL_TYPE_TIME:
					dataStream->encodeInt64(field->val_int());
					break;

				case MYSQL_TYPE_DATETIME:
					dataStream->encodeInt64(field->val_int());
					break;

				case MYSQL_TYPE_VARCHAR:
				case MYSQL_TYPE_VAR_STRING:
				case MYSQL_TYPE_STRING:
					{
					String string;
					String buffer;
					field->val_str(&buffer, &string);
					dataStream->encodeOpaque(string.length(), string.ptr());
					}
					break;

				case MYSQL_TYPE_TINY_BLOB:
					{
					Field_blob *blob = (Field_blob*) field;
					uint length = blob->get_length();
					uchar *ptr;
					blob->get_ptr(&ptr);
					dataStream->encodeOpaque(length, (const char*) ptr);
					}
					break;

				case MYSQL_TYPE_LONG_BLOB:
				case MYSQL_TYPE_BLOB:
				case MYSQL_TYPE_MEDIUM_BLOB:
				case MYSQL_TYPE_GEOMETRY:
					{
					Field_blob *blob = (Field_blob*) field;
					uint length = blob->get_length();
					uchar *ptr;
					blob->get_ptr(&ptr);
					StorageBlob *storageBlob;
					uint32 blobId;

					for (storageBlob = activeBlobs; storageBlob; storageBlob = storageBlob->next)
						if (storageBlob->data == (uchar*) ptr)
							{
							blobId = storageBlob->blobId;
							break;
							}

					if (!storageBlob)
						{
						StorageBlob storageBlob;
						storageBlob.length = length;
						storageBlob.data = (uchar *)ptr;
						blobId = storageTable->storeBlob(&storageBlob);
						blob->set_ptr(storageBlob.length, storageBlob.data);
						}

					dataStream->encodeBinaryBlob(blobId);
					}
					break;

				default:
					dataStream->encodeOpaque(field->field_length, (const char*) field->ptr);
				}

		if (ptrDiff)
			field->move_field_offset(-ptrDiff);
		}

	dbug_tmp_restore_column_map(table->read_set, old_map);
}

void StorageInterface::decodeRecord(uchar *buf)
{
	EncodedDataStream *dataStream = &storageTable->dataStream;
	my_ptrdiff_t ptrDiff = buf - table->record[0];
	my_bitmap_map *old_map = dbug_tmp_use_all_columns(table, table->write_set);
	DBUG_ENTER("StorageInterface::decodeRecord");

	// Format of this record

	FieldFormat *fieldFormat = storageTable->format->format;
	int maxId = storageTable->format->maxId;
	
	// Current format for the table, possibly newer than the record format
	
	int tableMaxId = storageTable->share->format->maxId;
	FieldFormat *tableFieldFormat = storageTable->share->format->format;
	
	for (int n = 0; n < tableMaxId; ++n, ++fieldFormat, ++tableFieldFormat)
		{
		// Online ALTER ADD COLUMN creates a new record format for the table
		// that will have more fields than the older formats associated with
		// existing rows.
		//
		// Currently, online ALTER ADD COLUMN only supports nullable columns and
		// no default value. If the format of this record has fewer fields
		// than the default format of the table, then there are no fields to
		// decode beyond maxId, so set them to NULL.
		
		if (n >= maxId)
			{
			Field *newField = fieldMap[tableFieldFormat->fieldId];
			newField->set_null();
			newField->reset();
			continue;
			}
	
		// If the format doesn't have an offset, the field doesn't exist in the record
		
		if (fieldFormat->fieldId < 0 || fieldFormat->offset == 0)
			continue;
			
		dataStream->decode();
		Field *field = fieldMap[fieldFormat->fieldId];

		// If we don't have a field for the physical field, just skip over it and don't worry
		
		if (field == NULL)
			continue;
				
		if (ptrDiff)
			field->move_field_offset(ptrDiff);

		if (dataStream->type == edsTypeNull || !bitmap_is_set(table->read_set, field->field_index))
			{
			field->set_null();
			field->reset();
			}
		else
			{
			field->set_notnull();

			switch (field->real_type())
				{
				case MYSQL_TYPE_TINY:
				case MYSQL_TYPE_SHORT:
				case MYSQL_TYPE_INT24:
				case MYSQL_TYPE_LONG:
				case MYSQL_TYPE_DECIMAL:
				case MYSQL_TYPE_ENUM:
				case MYSQL_TYPE_SET:
				case MYSQL_TYPE_BIT:
					field->store(dataStream->getInt64(),
								((Field_num*)field)->unsigned_flag);
					break;

				case MYSQL_TYPE_LONGLONG:
					{
					// If the type is edsTypeBigInt, the value is
					// unsigned and has the MSB set. This case has 
					// been handled specially in encodeRecord() to 
					// support unsigned values with the MSB set in
					// the index

					if (dataStream->type == edsTypeBigInt)
						{
						int64 value = dataStream->bigInt.getInt();
						field->store(value, ((Field_num*)field)->unsigned_flag);
						}
					else
						{
						field->store(dataStream->getInt64(),
									 ((Field_num*)field)->unsigned_flag);
						}
					}
					break;

				case MYSQL_TYPE_YEAR:
					// Must add 1900 to give Field_year::store the value it
					// expects. See also case 'MYSQL_TYPE_YEAR' in encodeRecord()
					field->store(dataStream->getInt64() + 1900, ((Field_num*)field)->unsigned_flag);
					break;

				case MYSQL_TYPE_NEWDECIMAL:
					{
					int precision = ((Field_new_decimal*) field)->precision;
					int scale = ((Field_new_decimal*) field)->dec;

					if (dataStream->type == edsTypeBigInt)
						ScaledBinary::putBigInt(&dataStream->bigInt, (char*) field->ptr, precision, scale);
					else
						{
						int64 value = dataStream->getInt64(scale);
						ScaledBinary::putBinaryDecimal(value, (char*) field->ptr, precision, scale);
						}
					}
					break;

				case MYSQL_TYPE_DOUBLE:
				case MYSQL_TYPE_FLOAT:
					field->store(dataStream->value.dbl);
					break;

				case MYSQL_TYPE_TIMESTAMP:
					{
					int value = (int) (dataStream->value.integer64 / 1000);
					((Field_timestamp*)field)->store_timestamp(value);
					}
					break;

				case MYSQL_TYPE_DATE:
					field->store(dataStream->getInt64(), false);
					break;

				case MYSQL_TYPE_NEWDATE:
					int3store(field->ptr, dataStream->getInt32());
					break;

				case MYSQL_TYPE_TIME:
					field->store(dataStream->getInt64(), false);
					break;

				case MYSQL_TYPE_DATETIME:
					int8store(field->ptr, dataStream->getInt64());
					break;

				case MYSQL_TYPE_VARCHAR:
				case MYSQL_TYPE_VAR_STRING:
				case MYSQL_TYPE_STRING:
					field->store((const char*) dataStream->value.string.data,
								dataStream->value.string.length, field->charset());
					break;

				case MYSQL_TYPE_TINY_BLOB:
					{
					Field_blob *blob = (Field_blob*) field;
					blob->set_ptr(dataStream->value.string.length,
					              (uchar*) dataStream->value.string.data);
					}
					break;

				case MYSQL_TYPE_LONG_BLOB:
				case MYSQL_TYPE_BLOB:
				case MYSQL_TYPE_MEDIUM_BLOB:
				case MYSQL_TYPE_GEOMETRY:
					{
					Field_blob *blob = (Field_blob*) field;
					StorageBlob *storageBlob = freeBlobs;

					if (storageBlob)
						freeBlobs = storageBlob->next;
					else
						storageBlob = new StorageBlob;

					storageBlob->next = activeBlobs;
					activeBlobs = storageBlob;
					storageBlob->blobId = dataStream->value.blobId;
					storageTable->getBlob(storageBlob->blobId, storageBlob);
					blob->set_ptr(storageBlob->length, (uchar*) storageBlob->data);
					}
					break;

				default:
					{
					uint l = dataStream->value.string.length;

					if (field->field_length < l)
						l = field->field_length;

					memcpy(field->ptr, dataStream->value.string.data, l);
					}
				}
			}

		if (ptrDiff)
			field->move_field_offset(-ptrDiff);
		}
	dbug_tmp_restore_column_map(table->write_set, old_map);

	DBUG_VOID_RETURN;
}


int StorageInterface::extra(ha_extra_function operation)
{
	DBUG_ENTER("StorageInterface::extra");
	
	if (operation == HA_EXTRA_ORDERBY_LIMIT)
		{
		// SQL Layer informs us that it is considering an ORDER BY .. LIMIT
		// query. It's time we could
		//  1. start returning HA_READ_ORDER flag from index_flags() calls,
		//     which will make the SQL layer consider using indexes to
		//     satisfy ORDER BY ... LIMIT.
		//  2. If doing #1, every index/range scan must return records in
		//     index order.

		indexOrder = true;
		}

	if (operation == HA_EXTRA_NO_ORDERBY_LIMIT)
		{
		// SQL Layer figured it won't be able to use index to resolve the 
		// ORDER BY ... LIMIT. This could happen for a number of reasons,
		// but the result is that we don't have to return records in index
		// order.
		
		indexOrder = false;
		}

	DBUG_RETURN(0);
}

bool StorageInterface::get_error_message(int error, String *buf)
{
	if (storageConnection)
		{
		const char *text = storageConnection->getLastErrorString();
		buf->set(text, (uint32)strlen(text), system_charset_info);
		}
	else if (errorText)
		buf->set(errorText, (uint32)strlen(errorText), system_charset_info);

	return false;
}

void StorageInterface::unlockTable(void)
{
	if (tableLocked)
		{
		storageShare->unlock();
		tableLocked = false;
		}
}

void StorageInterface::checkBinLog(void)
{
	// If binary logging is enabled, ensure that records are fully populated for replication
	
	if (mysql_bin_log.is_open())
		tableFlags |= HA_PRIMARY_KEY_REQUIRED_FOR_DELETE;
	else
		tableFlags &= ~HA_PRIMARY_KEY_REQUIRED_FOR_DELETE;
}

//*****************************************************************************
//
// NfsPluginHandler
//
//*****************************************************************************
NfsPluginHandler::NfsPluginHandler()
{
	storageConnection	= NULL;
	storageTable		= NULL;
}

NfsPluginHandler::~NfsPluginHandler()
{
}

//*****************************************************************************
//
// FALCON_SYSTEM_MEMORY_DETAIL
//
//*****************************************************************************
int NfsPluginHandler::getSystemMemoryDetailInfo(THD *thd, TABLE_LIST *tables, COND *cond)
{
	InfoTableImpl infoTable(thd, tables, system_charset_info);

	if (storageHandler)
		storageHandler->getMemoryDetailInfo(&infoTable);

	return infoTable.error;
}

ST_FIELD_INFO memoryDetailFieldInfo[]=
{
	{"FILE",		  120, MYSQL_TYPE_STRING,	0, 0, "File", SKIP_OPEN_TABLE},
	{"LINE",			4, MYSQL_TYPE_LONG,		0, 0, "Line", SKIP_OPEN_TABLE},
	{"OBJECTS_IN_USE",	4, MYSQL_TYPE_LONG,		0, 0, "Objects in Use", SKIP_OPEN_TABLE},
	{"SPACE_IN_USE",	4, MYSQL_TYPE_LONG,		0, 0, "Space in Use", SKIP_OPEN_TABLE},
	{"OBJECTS_DELETED", 4, MYSQL_TYPE_LONG,		0, 0, "Objects Deleted", SKIP_OPEN_TABLE},
	{"SPACE_DELETED",	4, MYSQL_TYPE_LONG,		0, 0, "Space Deleted", SKIP_OPEN_TABLE},
	{0,					0, MYSQL_TYPE_STRING,	0, 0, 0, SKIP_OPEN_TABLE}
};

int NfsPluginHandler::initSystemMemoryDetailInfo(void *p)
{
	DBUG_ENTER("initSystemMemoryDetailInfo");
	ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE*) p;
	schema->fields_info = memoryDetailFieldInfo;
	schema->fill_table = NfsPluginHandler::getSystemMemoryDetailInfo;
	DBUG_RETURN(0);
}

int NfsPluginHandler::deinitSystemMemoryDetailInfo(void *p)
{
	DBUG_ENTER("deinitSystemMemoryDetailInfo");
	DBUG_RETURN(0);
}

//*****************************************************************************
//
// FALCON_SYSTEM_MEMORY_SUMMARY
//
//*****************************************************************************

int NfsPluginHandler::getSystemMemorySummaryInfo(THD *thd, TABLE_LIST *tables, COND *cond)
{
	//return(pluginHandler->fillSystemMemorySummaryTable(thd, tables, cond));
	InfoTableImpl infoTable(thd, tables, system_charset_info);

	if (storageHandler)
		storageHandler->getMemorySummaryInfo(&infoTable);

	return infoTable.error;
}

ST_FIELD_INFO memorySummaryFieldInfo[]=
{
	{"TOTAL_SPACE",		4, MYSQL_TYPE_LONGLONG,		0, 0, "Total Space", SKIP_OPEN_TABLE},
	{"FREE_SPACE",		4, MYSQL_TYPE_LONGLONG,		0, 0, "Free Space", SKIP_OPEN_TABLE},
	{"FREE_SEGMENTS",	4, MYSQL_TYPE_LONG,			0, 0, "Free Segments", SKIP_OPEN_TABLE},
	{"BIG_HUNKS",		4, MYSQL_TYPE_LONG,			0, 0, "Big Hunks", SKIP_OPEN_TABLE},
	{"SMALL_HUNKS",		4, MYSQL_TYPE_LONG,			0, 0, "Small Hunks", SKIP_OPEN_TABLE},
	{"UNIQUE_SIZES",	4, MYSQL_TYPE_LONG,			0, 0, "Unique Sizes", SKIP_OPEN_TABLE},
	{0,					0, MYSQL_TYPE_STRING,		0, 0, 0, SKIP_OPEN_TABLE}
};

int NfsPluginHandler::initSystemMemorySummaryInfo(void *p)
{
	DBUG_ENTER("initSystemMemorySummaryInfo");
	ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;
	schema->fields_info = memorySummaryFieldInfo;
	schema->fill_table = NfsPluginHandler::getSystemMemorySummaryInfo;

	DBUG_RETURN(0);
}

int NfsPluginHandler::deinitSystemMemorySummaryInfo(void *p)
{
	DBUG_ENTER("deinitSystemMemorySummaryInfo");
	DBUG_RETURN(0);
}

//*****************************************************************************
//
// FALCON_RECORD_CACHE_DETAIL
//
//*****************************************************************************

int NfsPluginHandler::getRecordCacheDetailInfo(THD *thd, TABLE_LIST *tables, COND *cond)
{
	InfoTableImpl infoTable(thd, tables, system_charset_info);

	if (storageHandler)
		storageHandler->getRecordCacheDetailInfo(&infoTable);

	return infoTable.error;
}

ST_FIELD_INFO recordDetailFieldInfo[]=
{
	{"FILE",		  120, MYSQL_TYPE_STRING,	0, 0, "File", SKIP_OPEN_TABLE},
	{"LINE",			4, MYSQL_TYPE_LONG,		0, 0, "Line", SKIP_OPEN_TABLE},
	{"OBJECTS_IN_USE",	4, MYSQL_TYPE_LONG,		0, 0, "Objects in Use", SKIP_OPEN_TABLE},
	{"SPACE_IN_USE",	4, MYSQL_TYPE_LONG,		0, 0, "Space in Use", SKIP_OPEN_TABLE},
	{"OBJECTS_DELETED", 4, MYSQL_TYPE_LONG,		0, 0, "Objects Deleted", SKIP_OPEN_TABLE},
	{"SPACE_DELETED",	4, MYSQL_TYPE_LONG,		0, 0, "Space Deleted", SKIP_OPEN_TABLE},
	{0,					0, MYSQL_TYPE_STRING,	0, 0, 0, SKIP_OPEN_TABLE}
};

int NfsPluginHandler::initRecordCacheDetailInfo(void *p)
{
	DBUG_ENTER("initRecordCacheDetailInfo");
	ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;
	schema->fields_info = recordDetailFieldInfo;
	schema->fill_table = NfsPluginHandler::getRecordCacheDetailInfo;

	DBUG_RETURN(0);
}

int NfsPluginHandler::deinitRecordCacheDetailInfo(void *p)
{
	DBUG_ENTER("deinitRecordCacheDetailInfo");
	DBUG_RETURN(0);
}

//*****************************************************************************
//
// FALCON_RECORD_CACHE_SUMMARY
//
//*****************************************************************************

int NfsPluginHandler::getRecordCacheSummaryInfo(THD *thd, TABLE_LIST *tables, COND *cond)
{
	InfoTableImpl infoTable(thd, tables, system_charset_info);

	if (storageHandler)
		storageHandler->getRecordCacheSummaryInfo(&infoTable);

	return infoTable.error;
}

ST_FIELD_INFO recordSummaryFieldInfo[]=
{
	{"TOTAL_SPACE",		4, MYSQL_TYPE_LONGLONG,		0, 0, "Total Space", SKIP_OPEN_TABLE},
	{"FREE_SPACE",		4, MYSQL_TYPE_LONGLONG,		0, 0, "Free Space", SKIP_OPEN_TABLE},
	{"FREE_SEGMENTS",	4, MYSQL_TYPE_LONG,			0, 0, "Free Segments", SKIP_OPEN_TABLE},
	{"BIG_HUNKS",		4, MYSQL_TYPE_LONG,			0, 0, "Big Hunks", SKIP_OPEN_TABLE},
	{"SMALL_HUNKS",		4, MYSQL_TYPE_LONG,			0, 0, "Small Hunks", SKIP_OPEN_TABLE},
	{"UNIQUE_SIZES",	4, MYSQL_TYPE_LONG,			0, 0, "Unique Sizes", SKIP_OPEN_TABLE},
	{0,					0, MYSQL_TYPE_STRING,		0, 0, 0, SKIP_OPEN_TABLE}
};

int NfsPluginHandler::initRecordCacheSummaryInfo(void *p)
{
	DBUG_ENTER("initRecordCacheSummaryInfo");
	ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;
	schema->fields_info = recordSummaryFieldInfo;
	schema->fill_table = NfsPluginHandler::getRecordCacheSummaryInfo;

	DBUG_RETURN(0);
}

int NfsPluginHandler::deinitRecordCacheSummaryInfo(void *p)
{
	DBUG_ENTER("deinitRecordCacheSummaryInfo");
	DBUG_RETURN(0);
}

//*****************************************************************************
//
// FALCON_TABLESPACE_IO
//
//*****************************************************************************

int NfsPluginHandler::getTableSpaceIOInfo(THD *thd, TABLE_LIST *tables, COND *cond)
{
	InfoTableImpl infoTable(thd, tables, system_charset_info);

	if (storageHandler)
		storageHandler->getIOInfo(&infoTable);

	return infoTable.error;
}

ST_FIELD_INFO tableSpaceIOFieldInfo[]=
{
	{"TABLESPACE",	  120, MYSQL_TYPE_STRING,	0, 0, "Tablespace", SKIP_OPEN_TABLE},
	{"PAGE_SIZE",		4, MYSQL_TYPE_LONG,		0, 0, "Page Size", SKIP_OPEN_TABLE},
	{"BUFFERS",			4, MYSQL_TYPE_LONG,		0, 0, "Buffers", SKIP_OPEN_TABLE},
	{"PHYSICAL_READS",	4, MYSQL_TYPE_LONG,		0, 0, "Physical Reads", SKIP_OPEN_TABLE},
	{"WRITES",			4, MYSQL_TYPE_LONG,		0, 0, "Writes", SKIP_OPEN_TABLE},
	{"LOGICAL_READS",	4, MYSQL_TYPE_LONG,		0, 0, "Logical Reads", SKIP_OPEN_TABLE},
	{"FAKES",			4, MYSQL_TYPE_LONG,		0, 0, "Fakes", SKIP_OPEN_TABLE},
	{0,					0, MYSQL_TYPE_STRING,	0, 0, 0, SKIP_OPEN_TABLE}
};

int NfsPluginHandler::initTableSpaceIOInfo(void *p)
{
	DBUG_ENTER("initTableSpaceIOInfo");
	ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;
	schema->fields_info = tableSpaceIOFieldInfo;
	schema->fill_table = NfsPluginHandler::getTableSpaceIOInfo;

	DBUG_RETURN(0);
}

int NfsPluginHandler::deinitTableSpaceIOInfo(void *p)
{
	DBUG_ENTER("deinitTableSpaceIOInfo");
	DBUG_RETURN(0);
}

//*****************************************************************************
//
// FALCON_TRANSACTIONS
//
//*****************************************************************************

int NfsPluginHandler::getTransactionInfo(THD *thd, TABLE_LIST *tables, COND *cond)
{
	InfoTableImpl infoTable(thd, tables, system_charset_info);

	if (storageHandler)
		storageHandler->getTransactionInfo(&infoTable);

	return infoTable.error;
}

ST_FIELD_INFO transactionInfoFieldInfo[]=
{
	{"STATE",			120, MYSQL_TYPE_STRING,		0, 0, "State", SKIP_OPEN_TABLE},
	{"THREAD_ID",		4, MYSQL_TYPE_LONG,			0, 0, "Thread Id", SKIP_OPEN_TABLE},
	{"ID",				4, MYSQL_TYPE_LONG,			0, 0, "Id", SKIP_OPEN_TABLE},
	{"UPDATES",			4, MYSQL_TYPE_LONG,			0, 0, "Has Updates", SKIP_OPEN_TABLE},
	{"PENDING",			4, MYSQL_TYPE_LONG,			0, 0, "Write Pending", SKIP_OPEN_TABLE},
	{"DEP",				4, MYSQL_TYPE_LONG,			0, 0, "Dependencies", SKIP_OPEN_TABLE},
	{"OLDEST",			4, MYSQL_TYPE_LONG,			0, 0, "Oldest Active", SKIP_OPEN_TABLE},
	{"RECORDS",			4, MYSQL_TYPE_LONG,			0, 0, "Has Records", SKIP_OPEN_TABLE},
	{"WAITING_FOR",		4, MYSQL_TYPE_LONG,			0, 0, "Waiting For", SKIP_OPEN_TABLE},
	{"STATEMENT",	  120, MYSQL_TYPE_STRING,		0, 0, "Statement", SKIP_OPEN_TABLE},
	{0,					0, MYSQL_TYPE_STRING,		0, 0, 0, SKIP_OPEN_TABLE}
};

int NfsPluginHandler::initTransactionInfo(void *p)
{
	DBUG_ENTER("initTransactionInfo");
	ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;
	schema->fields_info = transactionInfoFieldInfo;
	schema->fill_table = NfsPluginHandler::getTransactionInfo;

	DBUG_RETURN(0);
}

int NfsPluginHandler::deinitTransactionInfo(void *p)
{
	DBUG_ENTER("deinitTransactionInfo");
	DBUG_RETURN(0);
}

//*****************************************************************************
//
// FALCON_TRANSACTION_SUMMARY
//
//*****************************************************************************

int NfsPluginHandler::getTransactionSummaryInfo(THD *thd, TABLE_LIST *tables, COND *cond)
{
	InfoTableImpl infoTable(thd, tables, system_charset_info);

	if (storageHandler)
		storageHandler->getTransactionSummaryInfo(&infoTable);

	return infoTable.error;
}

ST_FIELD_INFO transactionInfoFieldSummaryInfo[]=
{
//	{"DATABASE",		120, MYSQL_TYPE_STRING,		0, 0, "Database", SKIP_OPEN_TABLE},
	{"COMMITTED",		4, MYSQL_TYPE_LONG,			0, 0, "Committed Transaction.", SKIP_OPEN_TABLE},
	{"ROLLED_BACK",		4, MYSQL_TYPE_LONG,			0, 0, "Transactions Rolled Back.", SKIP_OPEN_TABLE},
	{"ACTIVE",   		4, MYSQL_TYPE_LONG,			0, 0, "Active Transactions", SKIP_OPEN_TABLE},
	{"PENDING_COMMIT",	4, MYSQL_TYPE_LONG,			0, 0, "Transaction Pending Commit", SKIP_OPEN_TABLE},
	{"PENDING_COMPLETION",4, MYSQL_TYPE_LONG,		0, 0, "Transaction Pending Completion", SKIP_OPEN_TABLE},
	{0,					0, MYSQL_TYPE_STRING,		0, 0, 0, SKIP_OPEN_TABLE}
};

int NfsPluginHandler::initTransactionSummaryInfo(void *p)
{
	DBUG_ENTER("initTransactionSummaryInfo");
	ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;
	schema->fields_info = transactionInfoFieldSummaryInfo;
	schema->fill_table = NfsPluginHandler::getTransactionSummaryInfo;

	DBUG_RETURN(0);
}

int NfsPluginHandler::deinitTransactionSummaryInfo(void *p)
{
	DBUG_ENTER("deinitTransactionInfo");
	DBUG_RETURN(0);
}

//*****************************************************************************
//
// FALCON_SERIAL_LOG_INFO
//
//*****************************************************************************

int NfsPluginHandler::getSerialLogInfo(THD *thd, TABLE_LIST *tables, COND *cond)
{
	InfoTableImpl infoTable(thd, tables, system_charset_info);

	if (storageHandler)
		storageHandler->getSerialLogInfo(&infoTable);

	return infoTable.error;
}

ST_FIELD_INFO serialSerialLogFieldInfo[]=
{
//	{"DATABASE",		120, MYSQL_TYPE_STRING,		0, 0, "Database", SKIP_OPEN_TABLE},
	{"TRANSACTIONS",	4, MYSQL_TYPE_LONG,			0, 0, "Transactions", SKIP_OPEN_TABLE},
	{"BLOCKS",			8, MYSQL_TYPE_LONGLONG,		0, 0, "Blocks", SKIP_OPEN_TABLE},
	{"WINDOWS",			4, MYSQL_TYPE_LONG,			0, 0, "Windows", SKIP_OPEN_TABLE},
	{"BUFFERS",			4, MYSQL_TYPE_LONG,			0, 0, "Buffers", SKIP_OPEN_TABLE},
	{0,					0, MYSQL_TYPE_STRING,		0, 0, 0, SKIP_OPEN_TABLE}
};

int NfsPluginHandler::initSerialLogInfo(void *p)
{
	DBUG_ENTER("initSerialLogInfoInfo");
	ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;
	schema->fields_info = serialSerialLogFieldInfo;
	schema->fill_table = NfsPluginHandler::getSerialLogInfo;

	DBUG_RETURN(0);
}

int NfsPluginHandler::deinitSerialLogInfo(void *p)
{
	DBUG_ENTER("deinitSerialLogInfo");
	DBUG_RETURN(0);
}

//*****************************************************************************
//
// FALCON_VERSION
//
//*****************************************************************************

int NfsPluginHandler::getFalconVersionInfo(THD *thd, TABLE_LIST *tables, COND *cond)
{
	InfoTableImpl infoTable(thd, tables, system_charset_info);

	if (storageHandler)
		storageHandler->getFalconVersionInfo(&infoTable);

	return infoTable.error;
}

ST_FIELD_INFO falconVersionFieldInfo[]=
{
	{"Version",		32, MYSQL_TYPE_STRING,		0, 0, "Version", SKIP_OPEN_TABLE},
	{"Date",		32, MYSQL_TYPE_STRING,		0, 0, "Date", SKIP_OPEN_TABLE},
	{0,				0, MYSQL_TYPE_STRING,		0, 0, 0, SKIP_OPEN_TABLE}
};

int NfsPluginHandler::initFalconVersionInfo(void *p)
{
	DBUG_ENTER("initFalconVersionInfo");
	ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;
	schema->fields_info = falconVersionFieldInfo;
	schema->fill_table = NfsPluginHandler::getFalconVersionInfo;

	DBUG_RETURN(0);
}

int NfsPluginHandler::deinitFalconVersionInfo(void *p)
{
	DBUG_ENTER("deinitFalconVersionInfo");
	DBUG_RETURN(0);
}

//*****************************************************************************
//
// FALCON_SYNCOBJECTS
//
//*****************************************************************************

int NfsPluginHandler::getSyncInfo(THD *thd, TABLE_LIST *tables, COND *cond)
{
	InfoTableImpl infoTable(thd, tables, system_charset_info);

	if (storageHandler)
		storageHandler->getSyncInfo(&infoTable);

	return infoTable.error;
}

ST_FIELD_INFO syncInfoFieldInfo[]=
{
	{"CALLER",			120, MYSQL_TYPE_STRING,		0, 0, "Caller", SKIP_OPEN_TABLE},
	{"SHARED",			4, MYSQL_TYPE_LONG,			0, 0, "Shared", SKIP_OPEN_TABLE},
	{"EXCLUSIVE",		4, MYSQL_TYPE_LONG,			0, 0, "Exclusive", SKIP_OPEN_TABLE},
	{"WAITS",			4, MYSQL_TYPE_LONG,			0, 0, "Waits", SKIP_OPEN_TABLE},
	{"QUEUE_LENGTH",	4, MYSQL_TYPE_LONG,			0, 0, "Queue Length", SKIP_OPEN_TABLE},
	{0,					0, MYSQL_TYPE_STRING,		0, 0, 0, SKIP_OPEN_TABLE}
};

int NfsPluginHandler::initSyncInfo(void *p)
{
	DBUG_ENTER("initSyncInfo");
	ST_SCHEMA_TABLE *schema = (ST_SCHEMA_TABLE *)p;
	schema->fields_info = syncInfoFieldInfo;
	schema->fill_table = NfsPluginHandler::getSyncInfo;

	DBUG_RETURN(0);
}

int NfsPluginHandler::deinitSyncInfo(void *p)
{
	DBUG_ENTER("deinitSyncInfo");
	DBUG_RETURN(0);
}

static void updateIndexChillThreshold(MYSQL_THD thd,
                                      struct st_mysql_sys_var *var,
                                      void *var_ptr, const void *save)
{
	falcon_index_chill_threshold = *(uint *)save;
	if(storageHandler)
		storageHandler->setIndexChillThreshold(falcon_index_chill_threshold);
}

static void updateRecordChillThreshold(MYSQL_THD thd,
                                      struct st_mysql_sys_var *var,
                                      void *var_ptr, const void *save)
{
	falcon_record_chill_threshold = *(uint *)save;
	if(storageHandler)
		storageHandler->setRecordChillThreshold(falcon_record_chill_threshold);
}

static void updateErrorInject(MYSQL_THD thd, struct st_mysql_sys_var *var,
	void *var_ptr, const void *save)
{
	ERROR_INJECTOR_PARSE(*(const char**)save);
}
void StorageInterface::updateRecordMemoryMax(MYSQL_THD thd, struct st_mysql_sys_var* variable, void* var_ptr, const void* save)
{
	falcon_record_memory_max = *(ulonglong*) save;
	storageHandler->setRecordMemoryMax(falcon_record_memory_max);
}

void StorageInterface::updateRecordScavengeThreshold(MYSQL_THD thd, struct st_mysql_sys_var* variable, void* var_ptr, const void* save)
{
	falcon_record_scavenge_threshold = *(uint*) save;
	storageHandler->setRecordScavengeThreshold(falcon_record_scavenge_threshold);
}

void StorageInterface::updateRecordScavengeFloor(MYSQL_THD thd, struct st_mysql_sys_var* variable, void* var_ptr, const void* save)
{
	falcon_record_scavenge_floor = *(uint*) save;
	storageHandler->setRecordScavengeFloor(falcon_record_scavenge_floor);
}


void StorageInterface::updateDebugMask(MYSQL_THD thd, struct st_mysql_sys_var* variable, void* var_ptr, const void* save)
{
	falcon_debug_mask = *(uint*) save;
	falcon_debug_mask&= ~(LogMysqlInfo|LogMysqlWarning|LogMysqlError);
	storageHandler->deleteNfsLogger(StorageInterface::logger, NULL);
	storageHandler->addNfsLogger(falcon_debug_mask, StorageInterface::logger, NULL);
}

int StorageInterface::recover (handlerton * hton, XID *xids, uint length)
{
	DBUG_ENTER("StorageInterface::recover");

	uint count = 0;
	unsigned char xid[sizeof(XID)];

	memset(xid, 0, sizeof(XID));

	while (storageHandler->recoverGetNextLimbo(sizeof(XID), xid))
		{
		count++;
		memcpy(xids++, xid, sizeof(XID));
		
		if (count >= length)
			break;
		}

	DBUG_RETURN(count);
}

// Build a record field map for use by encode/decodeRecord()

void StorageInterface::mapFields(TABLE *srvTable)
{
	if (!srvTable)
		return;
	
	maxFields = storageShare->format->maxId;
	unmapFields();
	fieldMap = new Field*[maxFields];
	memset(fieldMap, 0, sizeof(fieldMap[0]) * maxFields);
	char nameBuffer[256];

	for (uint n = 0; n < srvTable->s->fields; ++n)
		{
		Field *field = srvTable->field[n];
		storageShare->cleanupFieldName(field->field_name, nameBuffer, sizeof(nameBuffer), false);
		int id = storageShare->getFieldId(nameBuffer);
		
		if (id >= 0)
			fieldMap[id] = field;
		}
}

void StorageInterface::unmapFields(void)
{
	if (fieldMap)
		{
		delete []fieldMap;
		fieldMap = NULL;
		}
}

static MYSQL_SYSVAR_STR(serial_log_dir, falcon_serial_log_dir,
  PLUGIN_VAR_RQCMDARG| PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC,
  "Falcon serial log file directory.",
  NULL, NULL, mysql_real_data_home);

static MYSQL_SYSVAR_STR(checkpoint_schedule, falcon_checkpoint_schedule,
  PLUGIN_VAR_RQCMDARG| PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC,
  "Falcon checkpoint schedule.",
  NULL, NULL, "7 * * * * *");

static MYSQL_SYSVAR_STR(error_inject, falcon_error_inject,
  PLUGIN_VAR_MEMALLOC,
  "Used for testing purposes (error injection)",
  NULL, &updateErrorInject, "");

static MYSQL_SYSVAR_STR(scavenge_schedule, falcon_scavenge_schedule,
  PLUGIN_VAR_RQCMDARG| PLUGIN_VAR_READONLY | PLUGIN_VAR_MEMALLOC,
  "Falcon record scavenge schedule.",
  NULL, NULL, "15,45 * * * * *");

// #define MYSQL_SYSVAR_UINT(name, varname, opt, comment, check, update, def, min, max, blk)

#define PARAMETER_UINT(_name, _text, _min, _default, _max, _flags, _update_function) \
	static MYSQL_SYSVAR_UINT(_name, falcon_##_name, \
	PLUGIN_VAR_RQCMDARG | _flags, _text, NULL, _update_function, _default, _min, _max, 0);

#define PARAMETER_BOOL(_name, _text, _default, _flags, _update_function) \
	static MYSQL_SYSVAR_BOOL(_name, falcon_##_name,\
	PLUGIN_VAR_RQCMDARG | _flags, _text, NULL, _update_function, _default);

#include "StorageParameters.h"
#undef PARAMETER_UINT
#undef PARAMETER_BOOL

static MYSQL_SYSVAR_ULONGLONG(record_memory_max, falcon_record_memory_max,
  PLUGIN_VAR_RQCMDARG, // | PLUGIN_VAR_READONLY,
  "The maximum size of the record memory cache.",
  NULL, StorageInterface::updateRecordMemoryMax, 250LL<<20, 0, (ulonglong) max_memory_address, 1LL<<20);

static MYSQL_SYSVAR_ULONGLONG(serial_log_file_size, falcon_serial_log_file_size,
  PLUGIN_VAR_RQCMDARG,
  "If serial log file grows larger than this value, it will be truncated when it is reused",
  NULL, NULL , 10LL<<20, 1LL<<20,0x7fffffffffffffffLL, 1LL<<20);

/***
static MYSQL_SYSVAR_UINT(allocation_extent, falcon_allocation_extent,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_READONLY,
  "The percentage of the current size of falcon_user.fts to use as the size of the next extension to the file.",
  NULL, NULL, 10, 0, 100, 1);
***/

static MYSQL_SYSVAR_ULONGLONG(page_cache_size, falcon_page_cache_size,
  PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
  "The amount of memory to be used for the database page cache.",
  NULL, NULL, 4LL<<20, 2LL<<20, (ulonglong) max_memory_address, 1LL<<20);

static MYSQL_THDVAR_BOOL(consistent_read, PLUGIN_VAR_OPCMDARG,
   "Enable Consistent Read Mode for Repeatable Reads",
   NULL, NULL,1);

static int getTransactionIsolation(THD * thd)
{
	int level = isolation_levels[thd_tx_isolation(thd)];

	// TRANSACTION_CONSISTENT_READ  mapped to TRANSACTION_WRITE_COMMITTED,
	// if falcon_consistent_read is not set
	if(level == TRANSACTION_CONSISTENT_READ && !THDVAR(thd,consistent_read))
		return TRANSACTION_WRITE_COMMITTED;

	return level;
}


static struct st_mysql_sys_var* falconVariables[]= {

#define PARAMETER_UINT(_name, _text, _min, _default, _max, _flags, _update_function) MYSQL_SYSVAR(_name),
#define PARAMETER_BOOL(_name, _text, _default, _flags, _update_function) MYSQL_SYSVAR(_name),

#include "StorageParameters.h"
#undef PARAMETER_UINT
#undef PARAMETER_BOOL

	MYSQL_SYSVAR(serial_log_dir),
	MYSQL_SYSVAR(checkpoint_schedule),
	MYSQL_SYSVAR(scavenge_schedule),
	//MYSQL_SYSVAR(debug_mask),
	MYSQL_SYSVAR(record_memory_max),
	//MYSQL_SYSVAR(allocation_extent),
	MYSQL_SYSVAR(page_cache_size),
	MYSQL_SYSVAR(consistent_read),
	MYSQL_SYSVAR(serial_log_file_size),
	MYSQL_SYSVAR(error_inject),
	NULL
};

static st_mysql_storage_engine falcon_storage_engine			=	{ MYSQL_HANDLERTON_INTERFACE_VERSION};
static st_mysql_information_schema falcon_system_memory_detail	=	{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};
static st_mysql_information_schema falcon_system_memory_summary =	{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};
static st_mysql_information_schema falcon_record_cache_detail	=	{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};
static st_mysql_information_schema falcon_record_cache_summary	=	{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};
static st_mysql_information_schema falcon_tablespace_io			=	{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};
static st_mysql_information_schema falcon_transactions			=	{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};
static st_mysql_information_schema falcon_transaction_summary	=	{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};
static st_mysql_information_schema falcon_syncobjects			=	{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};
static st_mysql_information_schema falcon_serial_log_info		=	{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};
static st_mysql_information_schema falcon_version				=	{ MYSQL_INFORMATION_SCHEMA_INTERFACE_VERSION};

mysql_declare_plugin(falcon)
	{
	MYSQL_STORAGE_ENGINE_PLUGIN,
	&falcon_storage_engine,
	falcon_hton_name,
	"MySQL AB",
	"Falcon storage engine",
	PLUGIN_LICENSE_GPL,
	StorageInterface::falcon_init,				/* plugin init */
	StorageInterface::falcon_deinit,				/* plugin deinit */
	0x0100,										/* 1.0 */
	falconStatus,								/* status variables */
	falconVariables,							/* system variables */
	NULL										/* config options */
	},

	{
	MYSQL_INFORMATION_SCHEMA_PLUGIN,
	&falcon_system_memory_detail,
	"FALCON_SYSTEM_MEMORY_DETAIL",
	"MySQL AB",
	"Falcon System Memory Detail.",
	PLUGIN_LICENSE_GPL,
	NfsPluginHandler::initSystemMemoryDetailInfo,	/* plugin init */
	NfsPluginHandler::deinitSystemMemoryDetailInfo,	/* plugin deinit */
	0x0005,
	NULL,										/* status variables */
	NULL,										/* system variables */
	NULL										/* config options */
	},

	{
	MYSQL_INFORMATION_SCHEMA_PLUGIN,
	&falcon_system_memory_summary,
	"FALCON_SYSTEM_MEMORY_SUMMARY",
	"MySQL AB",
	"Falcon System Memory Summary.",
	PLUGIN_LICENSE_GPL,
	NfsPluginHandler::initSystemMemorySummaryInfo,	/* plugin init */
	NfsPluginHandler::deinitSystemMemorySummaryInfo,/* plugin deinit */
	0x0005,
	NULL,										/* status variables */
	NULL,										/* system variables */
	NULL										/* config options */
	},

	{
	MYSQL_INFORMATION_SCHEMA_PLUGIN,
	&falcon_record_cache_detail,
	"FALCON_RECORD_CACHE_DETAIL",
	"MySQL AB",
	"Falcon Record Cache Detail.",
	PLUGIN_LICENSE_GPL,
	NfsPluginHandler::initRecordCacheDetailInfo,	/* plugin init */
	NfsPluginHandler::deinitRecordCacheDetailInfo,	/* plugin deinit */
	0x0005,
	NULL,										/* status variables */
	NULL,										/* system variables */
	NULL										/* config options */
	},

	{
	MYSQL_INFORMATION_SCHEMA_PLUGIN,
	&falcon_record_cache_summary,
	"FALCON_RECORD_CACHE_SUMMARY",
	"MySQL AB",
	"Falcon Record Cache Summary.",
	PLUGIN_LICENSE_GPL,
	NfsPluginHandler::initRecordCacheSummaryInfo,	/* plugin init */
	NfsPluginHandler::deinitRecordCacheSummaryInfo,	/* plugin deinit */
	0x0005,
	NULL,										/* status variables */
	NULL,										/* system variables */
	NULL										/* config options   */
	},

	{
	MYSQL_INFORMATION_SCHEMA_PLUGIN,
	&falcon_transactions,
	"FALCON_TRANSACTIONS",
	"MySQL AB",
	"Falcon Transactions.",
	PLUGIN_LICENSE_GPL,
	NfsPluginHandler::initTransactionInfo,		/* plugin init */
	NfsPluginHandler::deinitTransactionInfo,	/* plugin deinit */
	0x0005,
	NULL,										/* status variables */
	NULL,										/* system variables */
	NULL										/* config options   */
	},

	{
	MYSQL_INFORMATION_SCHEMA_PLUGIN,
	&falcon_transaction_summary,
	"FALCON_TRANSACTION_SUMMARY",
	"MySQL AB",
	"Falcon Transaction Summary.",
	PLUGIN_LICENSE_GPL,
	NfsPluginHandler::initTransactionSummaryInfo,		/* plugin init */
	NfsPluginHandler::deinitTransactionSummaryInfo,	/* plugin deinit */
	0x0005,
	NULL,										/* status variables */
	NULL,										/* system variables */
	NULL										/* config options   */
	},

	{
	MYSQL_INFORMATION_SCHEMA_PLUGIN,
	&falcon_syncobjects,
	"FALCON_SYNCOBJECTS",
	"MySQL AB",
	"Falcon SyncObjects.",
	PLUGIN_LICENSE_GPL,
	NfsPluginHandler::initSyncInfo,				/* plugin init */
	NfsPluginHandler::deinitSyncInfo,			/* plugin deinit */
	0x0005,
	NULL,										/* status variables */
	NULL,										/* system variables */
	NULL										/* config options   */
	},

	{
	MYSQL_INFORMATION_SCHEMA_PLUGIN,
	&falcon_serial_log_info,
	"FALCON_SERIAL_LOG_INFO",
	"MySQL AB",
	"Falcon Serial Log Information.",
	PLUGIN_LICENSE_GPL,
	NfsPluginHandler::initSerialLogInfo,		/* plugin init */
	NfsPluginHandler::deinitSerialLogInfo,		/* plugin deinit */
	0x0005,
	NULL,										/* status variables */
	NULL,										/* system variables */
	NULL										/* config options   */
	},

	{
	MYSQL_INFORMATION_SCHEMA_PLUGIN,
	&falcon_tablespace_io,
	"FALCON_TABLESPACE_IO",
	"MySQL AB",
	"Falcon Tablespace IO.",
	PLUGIN_LICENSE_GPL,
	NfsPluginHandler::initTableSpaceIOInfo,		/* plugin init */
	NfsPluginHandler::deinitTableSpaceIOInfo,	/* plugin deinit */
	0x0005,
	NULL,										/* status variables */
	NULL,										/* system variables */
	NULL										/* config options   */
	},

	{
	MYSQL_INFORMATION_SCHEMA_PLUGIN,
	&falcon_version,
	"FALCON_VERSION",
	"MySQL AB",
	"Falcon Database Version Number.",
	PLUGIN_LICENSE_GPL,
	NfsPluginHandler::initFalconVersionInfo,			/* plugin init */
	NfsPluginHandler::deinitFalconVersionInfo,			/* plugin deinit */
	0x0005,
	NULL,										/* status variables */
	NULL,										/* system variables */
	NULL										/* config options   */
	}

mysql_declare_plugin_end;
