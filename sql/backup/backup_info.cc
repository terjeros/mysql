/**
  @file

  Implementation of @c Backup_info class. Method @c find_backup_engine()
  implements algorithm for selecting backup engine used to backup
  given table.
 */

#include "../mysql_priv.h"

#include "backup_info.h"
#include "backup_kernel.h"
#include "be_native.h"
#include "be_default.h"
#include "be_snapshot.h"

/// Return storage engine of a given table.
static
storage_engine_ref get_storage_engine(THD *thd, const backup::Table_ref &tbl)
{
  storage_engine_ref se= NULL;
  char path[FN_REFLEN];

  const char *db= tbl.db().name().ptr();
  const char *name= tbl.name().ptr();

  ::build_table_filename(path, sizeof(path), db, name, "", 0);

  ::TABLE *table= ::open_temporary_table(thd, path, db, name,
                    FALSE /* don't link to thd->temporary_tables */,
                    OTM_OPEN);
  if (table)
  {
    se= plugin_ref_to_se_ref(table->s->db_plugin);
    ::intern_close_table(table);
    my_free(table, MYF(0));
  }
  
  return se;
}

/// Determine if a given storage engine has native backup support.
static
bool has_native_backup(storage_engine_ref se)
{
  handlerton *hton= se_hton(se);

  return hton && hton->get_backup_engine;
}

/**
  Find backup engine which can backup data of a given table.

  @param[in] tbl  the table to be backed-up

  @returns pointer to a Snapshot_info instance representing 
  snapshot to which the given table can be added. 
 */
backup::Snapshot_info* 
Backup_info::find_backup_engine(const backup::Table_ref &tbl)
{
  using namespace backup;

  Table_ref::describe_buf buf;
  Snapshot_info *snap= NULL;
  
  DBUG_ENTER("Backup_info::find_backup_engine");

  // See if table has native backup engine

  storage_engine_ref se= get_storage_engine(m_ctx.m_thd, tbl);
  
  if (!se)
  {
    m_ctx.fatal_error(ER_NO_STORAGE_ENGINE, tbl.describe(buf));
    DBUG_RETURN(NULL);
  }
  
  snap= native_snapshots[se];
  
  if (!snap)
    if (has_native_backup(se))
    {
      Native_snapshot *nsnap= new Native_snapshot(m_ctx, se);
      DBUG_ASSERT(nsnap);
      snapshots.push_front(nsnap);
      native_snapshots.insert(se, nsnap);

      /*
        Question: Can native snapshot for a given storage engine not accept
        a table using that engine? If yes, then what to do in that case - error 
        or try other (default) snapshots?
       */     
      DBUG_ASSERT(nsnap->accept(tbl, se));
      snap= nsnap;
    }
  
  /* 
    If we couldn't locate native snapshot for that table - iterate over
    all existing snapshots and see if one of them can accept the table.
    
    The order on the snapshots list determines the preferred backup method 
    for a table. The snapshots for the built-in backup engines are always 
    present at the end of this list so that they can be selected as a last
    resort.
  */
    
  if (!snap)
  {
    List_iterator<Snapshot_info> it(snapshots);
    
    while ((snap= it++))
      if (snap->accept(tbl, se))
        break;
  }

  if (!snap)
    m_ctx.fatal_error(ER_BACKUP_NO_BACKUP_DRIVER,tbl.describe(buf));
  
  DBUG_RETURN(snap);
}

/*************************************************

   Implementation of Backup_info class

 *************************************************/

/*
  Definition of Backup_info::Ts_hash_node structure used by Backup_info::ts_hash
  HASH.
 */ 

struct Backup_info::Ts_hash_node
{
  const String *name;	///< Name of the tablespace.
  Ts *it;               ///< Catalogue entry holding the tablespace (if exists).

  Ts_hash_node(const String*);

  static uchar* get_key(const uchar *record, size_t *key_length, my_bool);
  static void free(void *record);
};

inline
Backup_info::Ts_hash_node::Ts_hash_node(const String *name) :name(name), it(NULL)
{}

void Backup_info::Ts_hash_node::free(void *record)
{
  delete (Ts_hash_node*)record;
}

uchar* Backup_info::Ts_hash_node::get_key(const uchar *record, 
                                          size_t *key_length, 
                                          my_bool)
{
  Ts_hash_node *n= (Ts_hash_node*)record;

  // ts_hash entries are indexed by tablespace name.

  if (n->name && key_length)
    *key_length= n->name->length();

  return (uchar*)(n->name->ptr());
}



/**
  Create @c Backup_info instance and prepare it for populating with objects.
 
  Snapshots created by the built-in backup engines are added to @c snapshots
  list to be used in the backup engine selection algorithm in 
  @c find_backup_engine().
 */
Backup_info::Backup_info(Backup_restore_ctx &ctx)
  :m_ctx(ctx), m_state(Backup_info::ERROR), native_snapshots(8)
{
  using namespace backup;

  Snapshot_info *snap;

  bzero(m_snap, sizeof(m_snap));

  hash_init(&ts_hash, &::my_charset_bin, 16, 0, 0,
            Ts_hash_node::get_key, Ts_hash_node::free, MYF(0));

  /* 
    Create default and CS snapshot objects and add them to the snapshots list.
    Note that the default snapshot should be the last element on that list, as a
    "catch all" entry. 
   */

  snap= new CS_snapshot(m_ctx); // reports errors

  if (!snap || !snap->is_valid())
    return;

  snapshots.push_back(snap);

  snap= new Default_snapshot(m_ctx);  // reports errors

  if (!snap || !snap->is_valid())
    return;

  snapshots.push_back(snap);
  
  m_state= CREATED;
}

Backup_info::~Backup_info()
{
  using namespace backup;

  close();

  // delete Snapshot_info instances.

  Snapshot_info *snap;
  List_iterator<Snapshot_info> it(snapshots);

  while ((snap= it++))
    delete snap;

  hash_free(&ts_hash);  
}

/**
  Close @c Backup_info object after populating it with items.

  After this call the @c Backup_info object is ready for use as a catalogue
  for backup stream functions such as @c bstream_wr_preamble().
 */
int Backup_info::close()
{
  if (!is_valid())
    return ERROR;

  if (m_state == CLOSED)
    return 0;

  // report backup drivers used in the image
  
  for (ushort n=0; n < snap_count(); ++n)
    m_ctx.report_driver(m_snap[n]->name());
  
  m_state= CLOSED;
  return 0;
}  

/**
  Add tablespace to backup catalogue.

  @param[in]	obj		sever object representing the tablespace
  
  If tablespace is already present in the catalogue, the existing catalogue entry
  is returned. Otherwise a new entry is created and tablespace info stored in it.
  
  @return Pointer to (the new or existing) catalogue entry holding info about the
  tablespace.  
 */ 
backup::Image_info::Ts* Backup_info::add_ts(obs::Obj *obj)
{
  const String *name;

  DBUG_ASSERT(obj);
  name= obj->get_name();
  DBUG_ASSERT(name);

  /* 
    Check if tablespace with that name is already present in the catalogue using
    ts_hash.
  */

  Ts_hash_node n0(name);
  size_t klen;
  uchar  *key= Ts_hash_node::get_key((const uchar*)&n0, &klen, TRUE);

  Ts_hash_node *n1= (Ts_hash_node*) hash_search(&ts_hash, key, klen);

  // if tablespace was found, return the catalogue entry stored in the hash
  if (n1)
    return n1->it;

  // otherwise create a new catalogue entry

  ulong pos= ts_count();

  Ts *ts= Image_info::add_ts(*name, pos);

  if (!ts)
  {
    m_ctx.fatal_error(ER_BACKUP_CATALOG_ADD_TS, name);
    return NULL;
  }

  // store pointer to the server object instance

  ts->m_obj_ptr= obj;

  // add new entry to ts_hash

  n1= new Ts_hash_node(n0);

  if (!n1)
  {
    m_ctx.fatal_error(ER_OUT_OF_RESOURCES);
    return NULL;
  }

  n1->it= ts;
  my_hash_insert(&ts_hash, (uchar*)n1);

  return ts;  
}

/**
  Select database object for backup.
  
  The object is added to the backup catalogue as an instance of 
  @c Image_info::Db class. A pointer to the obj::Obj instance is saved there for
  later usage.
  
  @returns Pointer to the @c Image_info::Db instance or NULL if database could
  not be added.
 */ 
backup::Image_info::Db* Backup_info::add_db(obs::Obj *obj)
{
  ulong pos= db_count();
  
  DBUG_ASSERT(obj);

  const ::String *name= obj->get_name();
  
  DBUG_ASSERT(name);  

  Db *db= Image_info::add_db(*name, pos);
  
  if (!db)
  {
    m_ctx.fatal_error(ER_BACKUP_CATALOG_ADD_DB, name->ptr());
    return NULL;
  }

  db->m_obj_ptr= obj;

  return db;  
}

/**
  Select given databases for backup.

  @param[in]  list of databases to be backed-up

  For each database, all objects stored in that database are also added to
  the image.

  @returns 0 on success, error code otherwise.
 */
int Backup_info::add_dbs(List< ::LEX_STRING > &dbs)
{
  using namespace obs;

  List_iterator< ::LEX_STRING > it(dbs);
  ::LEX_STRING *s;
  String unknown_dbs; // comma separated list of databases which don't exist

  while ((s= it++))
  {
    backup::String db_name(*s);
    
    if (is_internal_db_name(&db_name))
    {
      m_ctx.fatal_error(ER_BACKUP_CANNOT_INCLUDE_DB, db_name.c_ptr());
      goto error;
    }
    
    obs::Obj *obj= get_database(&db_name); // reports errors

    if (obj && !check_db_existence(&db_name))
    {    
      if (!unknown_dbs.is_empty()) // we just compose unknown_dbs list
      {
        delete obj;
        continue;
      }
      
      Db *db= add_db(obj);  // reports errors

      if (!db)
      {
        delete obj;
        goto error;
      }

      if (add_db_items(*db))  // reports errors
        goto error;
    }
    else if (obj)
    {
      if (!unknown_dbs.is_empty())
        unknown_dbs.append(",");
      unknown_dbs.append(*obj->get_name());
      
      delete obj;
    }
    else
      goto error; // error was reported in get_database()
  }

  if (!unknown_dbs.is_empty())
  {
    m_ctx.fatal_error(ER_BAD_DB_ERROR, unknown_dbs.c_ptr());
    goto error;
  }

  return 0;

 error:

  m_state= ERROR;
  return backup::ERROR;
}

/**
  Select all existing databases for backup.

  For each database, all objects stored in that database are also added to
  the image. The internal databases are skipped.

  @returns 0 on success, error code otherwise.
*/
int Backup_info::add_all_dbs()
{
  using namespace obs;

  int res= 0;
  ObjIterator *dbit= get_databases(m_ctx.m_thd);
  
  if (!dbit)
  {
    m_ctx.fatal_error(ER_BACKUP_LIST_DBS);
    return ERROR;
  }
  
  obs::Obj *obj;
  
  while ((obj= dbit->next()))
  {
    // skip internal databases
    if (is_internal_db_name(obj->get_name()))
    {
      DBUG_PRINT("backup",(" Skipping internal database %s", 
                           obj->get_name()->ptr()));
      delete obj;
      continue;
    }

    DBUG_PRINT("backup", (" Found database %s", obj->get_name()->ptr()));

    Db *db= add_db(obj);  // reports errors

    if (!db)
    {
      res= ERROR;
      delete obj;
      goto finish;
    }

    if (add_db_items(*db))  // reports errors
    {
      res= ERROR;
      goto finish;
    }
  }

  DBUG_PRINT("backup", ("No more databases in I_S"));

 finish:

  delete dbit;

  if (res)
    m_state= ERROR;

  return res;
}


/**
  Store in Backup_image all objects enumerated by the iterator.

  @param[in]  db  database to which objects belong - this database must already
                  be in the catalogue
  @param[in] type type of objects (only objects of the same type can be added)
  @param[in] it   iterator enumerationg objects to be added

  @returns 0 on success, error code otherwise.
 */
int Backup_info::add_objects(Db &db, const obj_type type, obs::ObjIterator &it)
{
  obs::Obj *obj;
  
  while ((obj= it.next()))
    if (!add_db_object(db, type, obj)) // reports errors
    {
      delete obj;
      return ERROR;
    }

  return 0;
}

/**
  Add to image all objects belonging to a given database.

  @returns 0 on success, error code otherwise.
 */
int Backup_info::add_db_items(Db &db)
{
  using namespace obs;

  // Add tables.

  ObjIterator *it= get_db_tables(m_ctx.m_thd, &db.name()); 

  if (!it)
  {
    m_ctx.fatal_error(ER_BACKUP_LIST_DB_TABLES, db.name().ptr());
    return ERROR;
  }
  
  int res= 0;
  obs::Obj *obj= NULL;

  while ((obj= it->next()))
  {
    DBUG_PRINT("backup", ("Found table %s for database %s",
                           obj->get_name()->ptr(), db.name().ptr()));

    /*
      add_table() method selects/creates a snapshot to which this table is added.
      The backup engine is choosen in Backup_info::find_backup_engine() method.
    */
    Table *tbl= add_table(db, obj); // reports errors

    if (!tbl)
    {
      delete obj;
      goto error;
    }

    // If this table uses a tablespace, add this tablespace to the catalogue.

    obj= get_tablespace_for_table(m_ctx.m_thd, &db.name(), &tbl->name());

    if (obj)
    {
      Ts *ts= add_ts(obj); // reports errors

      if (!ts)
      {
        delete obj;
        goto error;
      }
    }
  }

  // Add other objects.

  delete it;  
  it= get_db_stored_procedures(m_ctx.m_thd, &db.name());
  
  if (!it)
  {
    m_ctx.fatal_error(ER_BACKUP_LIST_DB_SROUT, db.name().ptr());
    goto error;
  }
  
  if (add_objects(db, BSTREAM_IT_SPROC, *it))
    goto error;

  delete it;
  it= get_db_stored_functions(m_ctx.m_thd, &db.name());

  if (!it)
  {
    m_ctx.fatal_error(ER_BACKUP_LIST_DB_SROUT, db.name().ptr());
    goto error;
  }
  
  if (add_objects(db, BSTREAM_IT_SFUNC, *it))
    goto error;

  delete it;
  it= get_db_views(m_ctx.m_thd, &db.name());

  if (!it)
  {
    m_ctx.fatal_error(ER_BACKUP_LIST_DB_VIEWS, db.name().ptr());
    goto error;
  }
  
  if (add_objects(db, BSTREAM_IT_VIEW, *it))
    goto error;

  delete it;
  it= get_db_events(m_ctx.m_thd, &db.name());

  if (!it)
  {
    m_ctx.fatal_error(ER_BACKUP_LIST_DB_EVENTS, db.name().ptr());
    goto error;
  }
  
  if (add_objects(db, BSTREAM_IT_EVENT, *it))
    goto error;
  
  delete it;
  it= get_db_triggers(m_ctx.m_thd, &db.name());

  if (!it)
  {
    m_ctx.fatal_error(ER_BACKUP_LIST_DB_TRIGGERS, db.name().ptr());
    goto error;
  }
  
  if (add_objects(db, BSTREAM_IT_TRIGGER, *it))
    goto error;
  
  goto finish;

 error:

  res= res ? res : ERROR;
  m_state= ERROR;
  
 finish:

  delete it;
  return res;
}

namespace {

/**
  Implementation of @c Table_ref which gets table identity from a server
  object instance - to be used in @c Backup_info::add_table().
 */ 
class Tbl: public backup::Table_ref
{
 public:

   Tbl(obs::Obj *obj) :backup::Table_ref(*obj->get_db_name(), *obj->get_name())
   {}

   ~Tbl()
   {}
};

} // anonymous namespace

/**
  Select table object for backup.

  @param[in]  dbi   database to which the table belongs
  @param[in]  obj   table object

  The object is added to the backup image's catalogue as an instance of
  @c Image_info::Table class. A pointer to the obj::Obj instance is saved for 
  later usage. This method picks the best available backup engine for the table 
  using @c find_backup_engine() method.

  @todo Correctly handle temporary tables.

  @returns Pointer to the @c Image_info::Table class instance or NULL if table
  could not be added.
*/
backup::Image_info::Table* Backup_info::add_table(Db &dbi, obs::Obj *obj)
{
  Table *tbl= NULL;
  
  DBUG_ASSERT(obj);

  Tbl t(obj);
  // TODO: skip table if it is a tmp one
  
  backup::Snapshot_info *snap= find_backup_engine(t); // reports errors

  if (!snap)
    return NULL;

  // add table to the catalogue

  ulong pos= snap->table_count();
  
  tbl= Image_info::add_table(dbi, t.name(), *snap, pos);
  
  if (!tbl)
  {
    m_ctx.fatal_error(ER_BACKUP_CATALOG_ADD_TABLE, 
                      dbi.name().ptr(), t.name().ptr());
    return NULL;
  }

  tbl->m_obj_ptr= obj;

  DBUG_PRINT("backup",(" table %s backed-up with %s engine (snapshot %d)",
                      t.name().ptr(), snap->name(), snap->m_num));
  return tbl;
}

/**
  Select a per database object for backup.

  This method is used for objects other than tables - tables are handled
  by @c add_table(). The object is added at first available position. Pointer
  to @c obj is stored for later usage.

  @param[in] db   object's database - must already be in the catalogue
  @param[in] type type of the object
  @param[in] obj  the object

  @returns Pointer to @c Image_info::Dbobj instance storing information 
  about the object or NULL in case of error.  
 */
backup::Image_info::Dbobj* 
Backup_info::add_db_object(Db &db, const obj_type type, obs::Obj *obj)
{
  int error= 0;
  ulong pos= db.obj_count();

  DBUG_ASSERT(obj);
  const ::String *name= obj->get_name();
  DBUG_ASSERT(name);

  switch (type) {

  // Databases and tables should not be passed to this function.  
  case BSTREAM_IT_DB:     DBUG_ASSERT(FALSE); break;
  case BSTREAM_IT_TABLE:  DBUG_ASSERT(FALSE); break;

  case BSTREAM_IT_VIEW:   error= ER_BACKUP_CATALOG_ADD_VIEW; break;
  case BSTREAM_IT_SPROC:  error= ER_BACKUP_CATALOG_ADD_SROUT; break;
  case BSTREAM_IT_SFUNC:  error= ER_BACKUP_CATALOG_ADD_SROUT; break;
  case BSTREAM_IT_EVENT:  error= ER_BACKUP_CATALOG_ADD_EVENT; break;
  case BSTREAM_IT_TRIGGER: error= ER_BACKUP_CATALOG_ADD_TRIGGER; break;
  
  // Only known types of objects should be added to the catalogue.
  default: DBUG_ASSERT(FALSE);

  }

  Dbobj *o= Image_info::add_db_object(db, type, *name, pos);
  
  if (!o)
  {
    m_ctx.fatal_error(error, db.name().ptr(), name->ptr());
    return NULL;
  }

  o->m_obj_ptr= obj;

  DBUG_PRINT("backup",("Added object %s of type %d from database %s (pos=%lu)",
                       name->ptr(), type, db.name().ptr(), pos));
  return o;
}

