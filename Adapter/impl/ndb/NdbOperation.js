/*
 Copyright (c) 2012, Oracle and/or its affiliates. All rights
 reserved.
 
 This program is free software; you can redistribute it and/or
 modify it under the terms of the GNU General Public License
 as published by the Free Software Foundation; version 2 of
 the License.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 02110-1301  USA
 */

/*global udebug, path, build_dir, spi_doc_dir, assert */

"use strict";

var adapter       = require(path.join(build_dir, "ndb_adapter.node")).ndb,
    encoders      = require("./NdbTypeEncoders.js").defaultForType,
    doc           = require(path.join(spi_doc_dir, "DBOperation"));


/* Constructors.
   All of these use prototypes directly from the documentation.
*/
var DBResult = function() {};
DBResult.prototype = doc.DBResult;

var DBOperationError = function() {};
DBOperationError.prototype = doc.DBOperationError;

var DBOperation = function(opcode, tx, tableHandler) {
  assert(doc.OperationCodes.indexOf(opcode) !== -1);
  assert(tx);
  assert(tableHandler);

  this.opcode       = opcode;
  this.transaction  = tx;
  this.tableHandler = tableHandler;
  this.buffers      = {};  
  this.state        = doc.OperationStates[0];  // DEFINED
  this.result       = new DBResult();
};
DBOperation.prototype = doc.DBOperation;

DBOperation.prototype.prepare = function(ndbTransaction) {
  udebug.log("NdbOperation prepare " + this.opcode);
  var helperSpec = {}, helper;
  switch(this.opcode) {
    case 'insert':
      helperSpec.row_record = this.tableHandler.dbTable.record;
      helperSpec.row_buffer = this.buffers.row;
      break;
    case 'delete': 
      helperSpec.key_record = this.index.record;
      helperSpec.key_buffer = this.buffers.key;
      helperSpec.row_record = this.tableHandler.dbTable.record;
      break;
    case 'read':
      helperSpec.key_record = this.index.record;
      helperSpec.key_buffer = this.buffers.key;
      helperSpec.row_record = this.tableHandler.dbTable.record;
      helperSpec.row_buffer = this.buffers.row;
      break; 
  }

  helper = adapter.impl.DBOperationHelper(helperSpec);
  udebug.log("NdbOperation prepare: got helper");
  
  switch(this.opcode) {
    case 'insert':
      this.ndbop = helper.insertTuple(ndbTransaction);
      break;
    case 'delete':
      this.ndbop = helper.deleteTuple(ndbTransaction);
      break;
    case 'read':
      this.ndbop = helper.readTuple(ndbTransaction);
      break;
  }

  this.state = doc.OperationStates[1];  // PREPARED
};


function encodeKeyBuffer(op) {
  udebug.log("NdbOperation encodeKeyBuffer");
  var i, offset, value, encoder, record, nfields, col;
  var indexHandler = op.tableHandler.getIndexHandler(op.keys);
  if(indexHandler) {
    op.index = indexHandler.dbIndex;
  }
  else {
    udebug.log("NdbOperation encodeKeyBuffer NO_INDEX");
    return;
  }

  record = op.index.record;
  op.buffers.key = new Buffer(record.getBufferSize());

  nfields = indexHandler.getMappedFieldCount();
  col = indexHandler.getColumnMetadata();
  for(i = 0 ; i < nfields ; i++) {
    value = indexHandler.get(op.keys, i);  
    if(value) {
      offset = record.getColumnOffset(i);
      encoder = encoders[col[i].ndbTypeId];
      encoder.write(col[i], value, op.buffers.key, offset);
    }
    else {
      udebug.log("NdbOperation encodeKeyBuffer "+i+" NULL.");
      record.setNull(i, op.buffers.key);
    }
  }
}

function encodeRowBuffer(op) {
  udebug.log("NdbOperation encodeRowBuffer");
  var i, offset, encoder, value;
  // FIXME: Get the mapped record, not the table record
  var record = op.tableHandler.dbTable.record;
  var row_buffer_size = record.getBufferSize();
  var nfields = op.tableHandler.getMappedFieldCount();
  udebug.log("NdbOperation encodeRowBuffer nfields", nfields);
  var col = op.tableHandler.getColumnMetadata();
  
  // do this earlier? 
  op.buffers.row = new Buffer(row_buffer_size);
  
  for(i = 0 ; i < nfields ; i++) {  
    value = op.tableHandler.get(op.row, i);
    if(value) {
      offset = record.getColumnOffset(i);
      encoder = encoders[col[i].ndbTypeId];
      encoder.write(col[i], value, op.buffers.row, offset);
      record.setNotNull(i, op.buffers.row);
    }
    else {
      udebug.log("NdbOperation encodeRowBuffer "+ i + " NULL.");
      record.setNull(i, op.buffers.row);
    }
  }
}

function readResultRow(op) {
  udebug.log("NdbOperation readResultRow");
  var i, offset, encoder, value;
  var dbt             = op.tableHandler;
  // FIXME: Get the mapped record, not the table record
  var record          = dbt.dbTable.record;
  var nfields         = dbt.getMappedFieldCount();
  var col             = dbt.getColumnMetadata();
  var resultRow       = dbt.newResultObject();
  
  for(i = 0 ; i < nfields ; i++) {
    offset  = record.getColumnOffset(i);
    encoder = encoders[col[i].ndbTypeId];
    assert(encoder);
    value   = encoder.read(col[i], op.buffers.row, offset);
    dbt.set(resultRow, i, value);
  }

  op.result.value = resultRow;
  // TODO: set op.result.success and error objects
}

var completeOpHandler = 
 { "read"   : readResultRow,
   "insert" : null,
   "update" : null,
   "write"  : null,
   "delete" : null
 };

function completeExecutedOps(txOperations) {
  udebug.log("NdbOperation completeExecutedOps", txOperations.length);
  var i, op, handler;
  for(i = 0; i < txOperations.length ; i++) {
    op = txOperations[i];
    if(op.result !== null) {
      op.result = new DBResult();
      handler = completeOpHandler[op.opcode];
      if(handler) {
        handler(op);
      }
    }
  }
  udebug.log("NdbOperation completeExecutedOps done");
}

function newReadOperation(tx, tableHandler, keys, lockMode) {
  udebug.log("NdbOperation getReadOperation");
  assert(doc.LockModes.indexOf(lockMode) !== -1);
  var op = new DBOperation("read", tx, tableHandler);
  var record = op.tableHandler.dbTable.record;
  op.lockMode = lockMode;
  op.keys = keys;
  encodeKeyBuffer(op);  

  /* The row buffer for a read must be allocated here, before execution */
  op.buffers.row = new Buffer(record.getBufferSize());
  return op;
}


function newInsertOperation(tx, tableHandler, row) {
  udebug.log("NdbOperation newInsertOperation");
  var op = new DBOperation("insert", tx, tableHandler);
  op.row = row;
  encodeRowBuffer(op);  
  return op;
}


function newDeleteOperation(tx, tableHandler, keys) {
  udebug.log("NdbOperation newDeleteOperation");
  var op = new DBOperation("delete", tx, tableHandler);
  op.keys = keys;
  encodeKeyBuffer(op);
  return op;
}


exports.DBOperation = DBOperation;
exports.newReadOperation   = newReadOperation;
exports.newInsertOperation = newInsertOperation;
exports.newDeleteOperation = newDeleteOperation;
exports.completeExecutedOps = completeExecutedOps;
