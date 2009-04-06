/* Copyright (C) 2006 MySQL AB, 2008 Sun Microsystems, Inc.

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

// Transaction.h: interface for the Transaction class.
//
//////////////////////////////////////////////////////////////////////

#if !defined(AFX_TRANSACTION_H__02AD6A4D_A433_11D2_AB5B_0000C01D2301__INCLUDED_)
#define AFX_TRANSACTION_H__02AD6A4D_A433_11D2_AB5B_0000C01D2301__INCLUDED_

#if _MSC_VER >= 1000
#pragma once
#endif // _MSC_VER >= 1000

#include "SyncObject.h"
#include "SerialLog.h"
#include "SavePoint.h"
#include "TransactionState.h"

static const int NO_TRANSACTION = 0;

class RecordVersion;
class Database;
class Transaction;
class Connection;
class Table;
class IO;
class DeferredIndex;
class Index;
class Bitmap;
class Record;
class InfoTable;
class Thread;
class TransactionManager;

static const int LOCAL_SAVE_POINTS = 5;

static const int FOR_READING = 0;
static const int FOR_WRITING = 1;

// flags for getRelativeStates()
#define WAIT_IF_ACTIVE		1
#define DO_NOT_WAIT			2

class Transaction
{
public:
	Transaction(Connection *connection, TransId seq);

	State		getRelativeState(Record* record, uint32 flags);
	State		getRelativeState (TransactionState* ts, uint32 flags);
	void		removeRecordNoLock (RecordVersion *record);
	void		removeRecord(RecordVersion *record);
	void		removeRecord (RecordVersion *record, RecordVersion **ptr);
	void		commitRecords();
	bool		visible (Transaction *transaction, TransId transId, int forWhat);
	bool		visible(const TransactionState* transState, int forWhat) const;
	bool		needToLock(Record* record);
	void		addRecord (RecordVersion *record);
	void		prepare(int xidLength, const UCHAR *xid);
	void		rollback();
	void		commit();
	void		release();
	void		addRef();
	bool		waitForTransaction (TransactionState* ts);
	State		waitForTransaction (TransactionState* ts, bool *deadlock);
	void		dropTable(Table* table);
	void		truncateTable(Table* table);
	bool		hasRecords(Table* table);
	void		writeComplete(void);
	int			createSavepoint();
	void		releaseSavepoint(int savepointId);
	void		releaseSavepoints(void);
	void		rollbackSavepoint (int savepointId);
	void		add(DeferredIndex* deferredIndex);
	void		initialize(Connection* cnct, TransId seq);
	bool		isXidEqual(int testLength, const UCHAR* test);
	void		releaseRecordLocks(void);
	void		chillRecords();
	int			thaw(RecordVersion* record);
	void		thaw(DeferredIndex* deferredIndex);
	void		print(void);
	void		printBlockage(void);
	void		getInfo(InfoTable* infoTable);
	void		fullyCommitted(void);
	void		releaseCommittedTransaction(void);
	void		commitNoUpdates(void);
	void		validateRecords(void);
	void		printBlocking(int level);
	void		releaseDeferredIndexes(void);
	void		releaseDeferredIndexes(Table* table);
	void		backlogRecords(void);
	bool		committedBefore(TransId transactionId);


	inline bool isActive()
		{
		return transactionState->state == Active || transactionState->state == Limbo;
		}

	State getState() const
		{
		return static_cast<State>(transactionState->state);
		}

	Connection		*connection;
	Database		*database;
	TransactionManager	*transactionManager;
	TransId			transactionId;  // used also as startEvent by dep.mgr.
	TransId			blockedBy;
	int				curSavePointId;
	Transaction		*next;			// next in database
	Transaction		*prior;			// next in database
	SavePoint		*savePoints;
	SavePoint		*freeSavePoints;
	SavePoint		localSavePoints[LOCAL_SAVE_POINTS];
	DeferredIndex	*deferredIndexes;
	Thread			*thread;
	Record			*blockingRecord;
	Bitmap			*backloggedRecords;
	time_t			startTime;
	int				deferredIndexCount;
	int				isolationLevel;
	int				xidLength;
	int				mySqlThreadId;
	UCHAR			*xid;
	bool			commitTriggers;
	bool			systemTransaction;
	bool			hasUpdates;
	bool			writePending;
	bool			seenGopher;
	//bool			pendingPageWrites;
	bool			hasLocks;
	SyncObject		syncObject;
	SyncObject		syncDeferredIndexes;
	SyncObject		syncRecords;
	SyncObject		syncSavepoints;
	uint64			totalRecordData;	// total bytes of record data for this transaction (unchilled + thawed)
	uint32			totalRecords;		// total record count
	uint32			chilledRecords;		// total chilled record count
	uint32			chilledBytes;		// current bytes chilled
	uint32			thawedRecords;		// total thawed record count
	uint32			thawedBytes;		// current bytes thawed
	uint32			debugThawedRecords;
	uint32			debugThawedBytes;
	uint32			committedRecords;	// committed record count
	uint32			deletedRecords;		// active deleted records (exclusive of backllogged ones)
	RecordVersion	**chillPoint;		// points to a pointer to the first non-chilled record
	int				scanIndexCount;
	TransactionState* transactionState;

	volatile INTERLOCK_TYPE	useCount;
	volatile INTERLOCK_TYPE	inList;

protected:
	RecordVersion	*firstRecord;
	RecordVersion	*lastRecord;

	virtual ~Transaction();
};

#endif // !defined(AFX_TRANSACTION_H__02AD6A4D_A433_11D2_AB5B_0000C01D2301__INCLUDED_)
