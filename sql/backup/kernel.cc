/**
  @file

  @brief Implementation of the backup kernel API.

  @section s1 How to use backup kernel API to perform backup and restore operations
  
  To perform backup or restore operation an appropriate context must be created.
  This involves creating required resources and correctly setting up the server.
  When operation is completed or interrupted, the context must be destroyed and
  all preparations reversed.
  
  All this is accomplished by creating an instance of Backup_create_ctx class and
  then using its methods to perform the operation. When the instance is 
  destroyed, the required clean-up is performed.
  
  This is how backup is performed using the context object:
  @code
  {
  
   Backup_restore_ctx context(thd); // create context instance
   Backup_info *info= context.prepare_for_backup(location); // prepare for backup
  
   // select objects to backup
   info->add_all_dbs();
   or
   info->add_dbs(<list of db names>);
  
   info->close(); // indicate that selection is done
  
   context.do_backup(); // perform backup
   
   context.close(); // explicit clean-up
  
  } // if code jumps here, context destructor will do the clean-up automatically
  @endcode
  
  Similar code will be used for restore (bit simpler as we don't support 
  selective restores yet):
  @code
  {
  
   Backup_restore_ctx context(thd); // create context instance
   Restore_info *info= context.prepare_for_restore(location); // prepare for restore
  
   context.do_restore(); // perform restore
   
   context.close(); // explicit clean-up
  
  } // if code jumps here, context destructor will do the clean-up automatically
  @endcode

  @todo Use internal table name representation when passing tables to
        backup/restore drivers.
  @todo Handle other types of meta-data in Backup_info methods.
  @todo Handle item dependencies when adding new items.
  @todo Handle other kinds of backup locations (far future).
*/

#include "../mysql_priv.h"
#include "../si_objects.h"

#include "backup_kernel.h"
#include "logger.h"
#include "stream.h"
#include "debug.h"
#include "be_native.h"
#include "be_default.h"
#include "be_snapshot.h"
#include "ddl_blocker.h"
#include "backup_progress.h"


/** 
  Global Initialization for online backup system.
 
  @note This function is called in the server initialization sequence, just
  after it loads all its plugins.
 */
int backup_init()
{
  pthread_mutex_init(&Backup_restore_ctx::run_lock, MY_MUTEX_INIT_FAST);
  return 0;
}

/**
  Global clean-up for online backup system.
  
  @note This function is called in the server shut-down sequences, just before
  it shuts-down all its plugins.
 */
void backup_shutdown()
{
  pthread_mutex_destroy(&Backup_restore_ctx::run_lock);
}

/*
  Forward declarations of functions used for sending response from BACKUP/RESTORE
  statement.
 */ 
static int send_error(Backup_restore_ctx &context, int error_code, ...);
static int send_reply(Backup_restore_ctx &context);


/**
  Call backup kernel API to execute backup related SQL statement.

  @param lex  results of parsing the statement.

  @note This function sends response to the client (ok, result set or error).

  @returns 0 on success, error code otherwise.
 */

int
execute_backup_command(THD *thd, LEX *lex)
{
  int res= 0;
  
  DBUG_ENTER("execute_backup_command");
  DBUG_ASSERT(thd && lex);

  BACKUP_BREAKPOINT("backup_command");

  using namespace backup;

  Backup_restore_ctx context(thd); // reports errors
  
  if (!context.is_valid())
    DBUG_RETURN(send_error(context, ER_BACKUP_CONTEXT_CREATE));

  switch (lex->sql_command) {

  case SQLCOM_BACKUP:
  {
    // prepare for backup operation
    
    Backup_info *info= context.prepare_for_backup(lex->backup_dir);
                                                              // reports errors

    if (!info || !info->is_valid())
      DBUG_RETURN(send_error(context, ER_BACKUP_BACKUP_PREPARE));

    BACKUP_BREAKPOINT("bp_running_state");

    // select objects to backup

    if (lex->db_list.is_empty())
    {
      context.write_message(log_level::INFO, "Backing up all databases");
      res= info->add_all_dbs(); // backup all databases
    }
    else
    {
      context.write_message(log_level::INFO, "Backing up selected databases");
      res= info->add_dbs(lex->db_list); // backup databases specified by user
    }

    info->close(); // close catalogue after filling it with objects to backup

    if (res || !info->is_valid())
      DBUG_RETURN(send_error(context, ER_BACKUP_BACKUP_PREPARE));

    if (info->db_count() == 0)
    {
      context.fatal_error(ER_BACKUP_NOTHING_TO_BACKUP);
      DBUG_RETURN(send_error(context, ER_BACKUP_NOTHING_TO_BACKUP));
    }

    // perform backup

    res= context.do_backup();
 
    if (res)
      DBUG_RETURN(send_error(context, ER_BACKUP_BACKUP));

    BACKUP_BREAKPOINT("bp_complete_state");
    break;
  }

  case SQLCOM_RESTORE:
  {
    Restore_info *info= context.prepare_for_restore(lex->backup_dir);
    
    if (!info || !info->is_valid())
      DBUG_RETURN(send_error(context, ER_BACKUP_RESTORE_PREPARE));
    
    BACKUP_BREAKPOINT("bp_running_state");

    res= context.do_restore();      

    if (res)
      DBUG_RETURN(send_error(context, ER_BACKUP_RESTORE));
    
    break;
  }

  default:
     /*
       execute_backup_command() should be called with correct command id
       from the parser. If not, we fail on this assertion.
      */
     DBUG_ASSERT(FALSE);

  } // switch(lex->sql_command)

  if (context.close())
    DBUG_RETURN(send_error(context, ER_BACKUP_CONTEXT_REMOVE));

  // All seems OK - send positive reply to client

  DBUG_RETURN(send_reply(context));
}

/**
  Report errors.

  Current implementation reports the last error saved in the logger if it exist.
  Otherwise it reports error given by @c error_code.

  @returns 0 on success, error code otherwise.
 */
int send_error(Backup_restore_ctx &log, int error_code, ...)
{
  MYSQL_ERROR *error= log.last_saved_error();

  if (error && !util::report_mysql_error(log.m_thd, error, error_code))
  {
    if (error->code)
      error_code= error->code;
  }
  else // there are no error information in the logger - report error_code
  {
    char buf[ERRMSGSIZE + 20];
    va_list args;
    va_start(args, error_code);

    my_vsnprintf(buf, sizeof(buf), ER_SAFE(error_code), args);
    my_printf_error(error_code, buf, MYF(0));

    va_end(args);
  }

  if (log.backup::Logger::m_state == backup::Logger::RUNNING)
    log.report_stop(my_time(0), FALSE); // FASLE = no success
  return error_code;
}


/**
  Send positive reply after a backup/restore operation.

  Currently the id of the operation is returned. It can be used to select
  correct entries form the backup progress tables.
*/
int send_reply(Backup_restore_ctx &context)
{
  Protocol *protocol= context.m_thd->protocol;    // client comms
  List<Item> field_list;                // list of fields to send
  char buf[255];                        // buffer for llstr

  DBUG_ENTER("send_reply");

  /*
    Send field list.
  */
  field_list.push_back(new Item_empty_string(STRING_WITH_LEN("backup_id")));
  protocol->send_fields(&field_list, Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF);

  /*
    Send field data.
  */
  protocol->prepare_for_resend();
  llstr(context.op_id(), buf);
  protocol->store(buf, system_charset_info);
  protocol->write();

  my_eof(context.m_thd);
  DBUG_RETURN(0);
}


namespace backup {

/**
  This class provides memory allocation services for backup stream library.

  An instance of this class is created during preparations for backup/restore
  operation. When it is deleted, all allocated memory is freed.
*/
class Mem_allocator
{
 public:

  Mem_allocator();
  ~Mem_allocator();

  void* alloc(size_t);
  void  free(void*);

 private:

  struct node;
  node *first;  ///< Pointer to the first segment in the list.
};


} // backup namespace


/*************************************************

   Implementation of Backup_restore_ctx class

 *************************************************/

// static members

bool Backup_restore_ctx::is_running= FALSE;
pthread_mutex_t Backup_restore_ctx::run_lock;
backup::Mem_allocator *Backup_restore_ctx::mem_alloc= NULL;


Backup_restore_ctx::Backup_restore_ctx(THD *thd)
 :m_state(CREATED), m_thd(thd), m_thd_options(thd->options),
 m_error(0), m_remove_loc(FALSE), m_stream(NULL), m_catalog(NULL)
{
  /*
    Check for progress tables.
  */
  if (check_ob_progress_tables(thd))
    m_error= ER_BACKUP_PROGRESS_TABLES;
}

Backup_restore_ctx::~Backup_restore_ctx()
{
  close();
  
  delete m_catalog;  
  delete m_stream;
}

/**
  Do preparations common to backup and restore operations.
  
  It is checked if another operation is in progress and if yes then
  error is reported. Otherwise the current operation is registered so that
  no other can be started. All preparations common to backup and restore 
  operations are done. In particular, all changes to meta data are blocked
  with DDL blocker.

  @returns 0 on success, error code otherwise.
 */ 
int Backup_restore_ctx::prepare(LEX_STRING location)
{
  if (m_error)
    return m_error;
  
  // Prepare error reporting context.
  
  mysql_reset_errors(m_thd, 0);
  m_thd->no_warnings_for_error= FALSE;
  save_errors();  


  /*
    Check access for SUPER rights. If user does not have SUPER, fail with error.
  */
  if (check_global_access(m_thd, SUPER_ACL))
  {
    fatal_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, "SUPER");
    return m_error;
  }

  /*
    Check if another BACKUP/RESTORE is running and if not, register 
    this operation.
   */

  pthread_mutex_lock(&run_lock);

  if (!is_running)
    is_running= TRUE;
  else
    fatal_error(ER_BACKUP_RUNNING);

  pthread_mutex_unlock(&run_lock);

  if (m_error)
    return m_error;

  // check if location is valid (we assume it is a file path)

  bool bad_filename= (location.length == 0);
  
  /*
    On some systems certain file names are invalid. We use 
    check_if_legal_filename() function from mysys to detect this.
   */ 
#if defined(__WIN__) || defined(__EMX__)  

  bad_filename = bad_filename || check_if_legal_filename(location.str);
  
#endif

  if (bad_filename)
  {
    fatal_error(ER_BAD_PATH, location.str);
    return m_error;
  }

  m_path= location.str;

  // create new instance of memory allocator for backup stream library

  using namespace backup;

  delete mem_alloc;
  mem_alloc= new Mem_allocator();

  if (!mem_alloc)
  {
    fatal_error(ER_OUT_OF_RESOURCES);
    return m_error;
  }

  // Freeze all meta-data. 

  if (obs::ddl_blocker_enable(m_thd))
  {
    fatal_error(ER_DDL_BLOCK);
    return m_error;
  }

  return 0;
}

/**
  Prepare for backup operation.
  
  @param[in] location   path to the file where backup image should be stored
  
  @returns Pointer to a @c Backup_info instance which can be used for selecting
  which objects to backup. NULL if an error was detected.
  
  @note This function reports errors.

  @note It is important that changes of meta-data are blocked as part of the
  preparations. The set of server objects and their definitions should not
  change after the backup context has been prepared and before the actual backup
  is performed using @c do_backup() method.
 */ 
Backup_info* Backup_restore_ctx::prepare_for_backup(LEX_STRING location)
{
  using namespace backup;
  
  if (m_error)
    return NULL;
  
  if (Logger::init(m_thd, BACKUP, location))
  {
    fatal_error(ER_BACKUP_LOGGER_INIT);
    return NULL;
  }

  time_t when= my_time(0);
  report_start(when);
  
  /*
    Do preparations common to backup and restore operations. After call
    to prepare() all meta-data changes are blocked.
   */ 
  if (prepare(location))
    return NULL;

  backup::String path(location);
  
  /*
    Open output stream.
   */

  Output_stream *s= new Output_stream(*this, path);
  
  if (!s)
  {
    fatal_error(ER_OUT_OF_RESOURCES);
    return NULL;
  }
  
  if (!s->open())
  {
    fatal_error(ER_BACKUP_WRITE_LOC, path.ptr());
    return NULL;
  }

  m_stream= s;

  /*
    Create backup catalogue.
   */

  Backup_info *info= new Backup_info(*this); // reports errors

  if (!info)
  {
    fatal_error(ER_OUT_OF_RESOURCES);
    return NULL;
  }

  if (!info->is_valid())
    return NULL;

  info->save_start_time(when);
  m_catalog= info;
  m_state= PREPARED_FOR_BACKUP;
  
  return info;
}

/**
  Prepare for restore operation.
  
  @param[in] location   path to the file where backup image is stored
  
  @returns Pointer to a @c Restore_info instance containing catalogue of the
  backup image (read from the image). NULL if errors were detected.
  
  @note This function reports errors.
 */ 
Restore_info* Backup_restore_ctx::prepare_for_restore(LEX_STRING location)
{
  using namespace backup;  

  if (m_error)
    return NULL;
  
  if (Logger::init(m_thd, RESTORE, location))
  {
    fatal_error(ER_BACKUP_LOGGER_INIT);
    return NULL;
  }

  time_t when= my_time(0);  
  report_start(when);

  /*
    Do preparations common to backup and restore operations. After this call
    changes of meta-data are blocked.
   */ 
  if (prepare(location))
    return NULL;
  
  /*
    Open input stream.
   */

  backup::String path(location);
  Input_stream *s= new Input_stream(*this, path);
  
  if (!s)
  {
    fatal_error(ER_OUT_OF_RESOURCES);
    return NULL;
  }
  
  if (!s->open())
  {
    fatal_error(ER_BACKUP_READ_LOC, path.ptr());
    return NULL;
  }

  m_stream= s;

  /*
    Create restore catalogue.
   */

  Restore_info *info= new Restore_info(*this);  // reports errors

  if (!info)
  {
    fatal_error(ER_OUT_OF_RESOURCES);
    return NULL;
  }

  if (!info->is_valid())
    return NULL;

  info->save_start_time(when);
  m_catalog= info;

  /*
    Read catalogue from the input stream.
   */

  if (read_header(*info, *s))
  {
    fatal_error(ER_BACKUP_READ_HEADER);
    return NULL;
  }

  if (s->next_chunk() != BSTREAM_OK)
  {
    fatal_error(ER_BACKUP_NEXT_CHUNK);
    return NULL;
  }

  if (read_catalog(*info, *s))
  {
    fatal_error(ER_BACKUP_READ_HEADER);
    return NULL;
  }

  if (s->next_chunk() != BSTREAM_OK)
  {
    fatal_error(ER_BACKUP_NEXT_CHUNK);
    return NULL;
  }

  m_state= PREPARED_FOR_RESTORE;

  return info;
}

/**
  Destroy a backup/restore context.
  
  This should reverse all settings made when context was created and prepared.
  If it was requested, the backup/restore location is removed. Also, the backup
  stream memory allocator is shut down. Any other allocated resources are 
  deleted in the destructor. Changes to meta-data are unblocked.
  
  @returns 0 or error code if error was detected.
  
  @note This function reports errors.
 */ 
int Backup_restore_ctx::close()
{
  if (m_state == CLOSED)
    return 0;

  using namespace backup;

  time_t when= my_time(0);

  // unfreeze meta-data

  obs::ddl_blocker_disable();

  // restore thread options

  m_thd->options= m_thd_options;

  // close stream

  if (m_stream)
    m_stream->close();

  if (m_catalog)
    m_catalog->save_end_time(when);

  // destroy backup stream's memory allocator (this frees memory)

  delete mem_alloc;
  mem_alloc= NULL;
  
  // deregister this operation

  pthread_mutex_lock(&run_lock);
  is_running= FALSE;
  pthread_mutex_unlock(&run_lock);

  /* 
    Remove the location, if asked for.
    
    Important: This is done only for backup operation - RESTORE should never
    remove the specified backup image!
   */
  if (m_remove_loc && m_state == PREPARED_FOR_BACKUP)
  {
    int res= my_delete(m_path, MYF(0));

    /*
      Ignore ENOENT error since it is ok if the file doesn't exist.
     */
    if (res && my_errno != ENOENT)
    {
      report_error(ER_CANT_DELETE_FILE, m_path, my_errno);
      if (!m_error)
        m_error= ER_CANT_DELETE_FILE;
    }
  }

  // We report completion of the operation only if no errors were detected.

  if (!m_error)
    report_stop(when, TRUE);

  m_state= CLOSED;
  return m_error;
}

/**
  Create backup archive.
  
  @pre @c prepare_for_backup() method was called.

  @returns 0 on success, error code otherwise.
*/
int Backup_restore_ctx::do_backup()
{
  DBUG_ENTER("do_backup");

  // This function should not be called when context is not valid
  DBUG_ASSERT(is_valid());
  DBUG_ASSERT(m_state == PREPARED_FOR_BACKUP);
  DBUG_ASSERT(m_thd);
  DBUG_ASSERT(m_stream);
  DBUG_ASSERT(m_catalog);
  
  using namespace backup;

  Output_stream &s= *static_cast<Output_stream*>(m_stream);
  Backup_info   &info= *static_cast<Backup_info*>(m_catalog);

  BACKUP_BREAKPOINT("backup_meta");

  report_stats_pre(info);

  DBUG_PRINT("backup",("Writing preamble"));

  if (write_preamble(info, s))
  {
    fatal_error(ER_BACKUP_WRITE_HEADER);
    DBUG_RETURN(m_error);
  }

  DBUG_PRINT("backup",("Writing table data"));

  BACKUP_BREAKPOINT("backup_data");

  if (write_table_data(m_thd, *this, info, s)) // reports errors
    DBUG_RETURN(send_error(*this, ER_BACKUP_BACKUP));

  DBUG_PRINT("backup",("Writing summary"));

  if (write_summary(info, s))
  {
    fatal_error(ER_BACKUP_WRITE_SUMMARY);
    DBUG_RETURN(m_error);
  }

  report_stats_post(info);

  DBUG_PRINT("backup",("Backup done."));
  BACKUP_BREAKPOINT("backup_done");

  DBUG_RETURN(0);
}


/**
  Restore objects saved in backup image.

  @pre @c prepare_for_restore() method was called.

  @returns 0 on success, error code otherwise.

  @todo Remove the @c reset_diagnostic_area() hack.
*/
int Backup_restore_ctx::do_restore()
{
  DBUG_ENTER("do_restore");

  DBUG_ASSERT(is_valid());
  DBUG_ASSERT(m_state == PREPARED_FOR_RESTORE);
  DBUG_ASSERT(m_thd);
  DBUG_ASSERT(m_stream);
  DBUG_ASSERT(m_catalog);

  using namespace backup;

  Input_stream &s= *static_cast<Input_stream*>(m_stream);
  Restore_info &info= *static_cast<Restore_info*>(m_catalog);

  report_stats_pre(info);

  DBUG_PRINT("restore", ("Restoring meta-data"));

  disable_fkey_constraints();

  if (read_meta_data(info, s))
  {
    fatal_error(ER_BACKUP_READ_META);
    DBUG_RETURN(m_error);
  }

  s.next_chunk();

  DBUG_PRINT("restore",("Restoring table data"));

  /* 
    FIXME: this call is here because object services doesn't clean the
    statement execution context properly, which leads to assertion failure.
    It should be fixed inside object services implementation and then the
    following line should be removed.
   */
  m_thd->main_da.reset_diagnostics_area();

  // Here restore drivers are created to restore table data
  if (restore_table_data(m_thd, *this, info, s)) // reports errors
    DBUG_RETURN(send_error(*this, ER_BACKUP_RESTORE));

  DBUG_PRINT("restore",("Done."));

  if (read_summary(info, s))
  {
    fatal_error(ER_BACKUP_READ_SUMMARY);
    DBUG_RETURN(m_error);
  }

  report_stats_post(info);

  DBUG_RETURN(0);
}


namespace backup {

/*************************************************

    Implementation of Mem_allocator class.

 *************************************************/

/// All allocated memory segments are linked into a list using this structure.
struct Mem_allocator::node
{
  node *prev;
  node *next;
};

Mem_allocator::Mem_allocator() :first(NULL)
{}

/// Deletes all allocated segments which have not been freed explicitly.
Mem_allocator::~Mem_allocator()
{
  node *n= first;

  while (n)
  {
    first= n->next;
    my_free(n, MYF(0));
    n= first;
  }
}

/**
  Allocate memory segment of given size.

  Extra memory is allocated for @c node structure which holds pointers
  to previous and next segment in the segments list. This is used when
  deallocating allocated memory in the destructor.
*/
void* Mem_allocator::alloc(size_t howmuch)
{
  void *ptr= my_malloc(sizeof(node) + howmuch, MYF(0));

  if (!ptr)
    return NULL;

  node *n= (node*)ptr;
  ptr= n + 1;

  n->prev= NULL;
  n->next= first;
  if (first)
    first->prev= n;
  first= n;

  return ptr;
}

/**
  Explicit deallocation of previously allocated segment.

  The @c ptr should contain an address which was obtained from
  @c Mem_allocator::alloc().

  The deallocated fragment is removed from the allocated fragments list.
*/
void Mem_allocator::free(void *ptr)
{
  if (!ptr)
    return;

  node *n= ((node*)ptr) - 1;

  if (first == n)
    first= n->next;

  if (n->prev)
    n->prev->next= n->next;

  if (n->next)
    n->next->prev= n->prev;

  my_free(n, MYF(0));
}

} // backup namespace


/*************************************************

               CATALOGUE SERVICES

 *************************************************/

/**
  Memory allocator for backup stream library.

  @pre A backup/restore context has been created and prepared for the 
  operation (one of @c Backup_restore_ctx::prepare_for_backup() or 
  @c Backup_restore_ctx::prepare_for_restore() have been called).
 */
extern "C"
bstream_byte* bstream_alloc(unsigned long int size)
{
  using namespace backup;

  DBUG_ASSERT(Backup_restore_ctx::mem_alloc);

  return (bstream_byte*)Backup_restore_ctx::mem_alloc->alloc(size);
}

/**
  Memory deallocator for backup stream library.
*/
extern "C"
void bstream_free(bstream_byte *ptr)
{
  using namespace backup;

  if (Backup_restore_ctx::mem_alloc)
    Backup_restore_ctx::mem_alloc->free(ptr);
}

/**
  Prepare restore catalogue for populating it with items read from
  backup image.

  At this point we know the list of table data snapshots present in the image
  (it was read from image's header). Here we create @c Snapshot_info object
  for each of them.

  @rerturns 0 on success, error code otherwise.
*/
extern "C"
int bcat_reset(st_bstream_image_header *catalogue)
{
  using namespace backup;

  uint n;

  DBUG_ASSERT(catalogue);
  Restore_info *info= static_cast<Restore_info*>(catalogue);

  /*
    Iterate over the list of snapshots read from the backup image (and stored
    in snapshot[] array in the catalogue) and for each snapshot create a 
    corresponding Snapshot_info instance. A pointer to this instance is stored
    in m_snap[] array.
   */ 

  for (n=0; n < info->snap_count(); ++n)
  {
    st_bstream_snapshot_info *snap= &info->snapshot[n];

    DBUG_PRINT("restore",("Creating info for snapshot no. %d", n));

    switch (snap->type) {

    case BI_NATIVE:
    {
      backup::LEX_STRING name_lex(snap->engine.name.begin, snap->engine.name.end);
      storage_engine_ref se= get_se_by_name(name_lex);
      handlerton *hton= se_hton(se);

      if (!se || !hton)
      {
        info->m_ctx.fatal_error(ER_BACKUP_CANT_FIND_SE, name_lex.str);
        return BSTREAM_ERROR;
      }

      if (!hton->get_backup_engine)
      {
        info->m_ctx.fatal_error(ER_BACKUP_NO_NATIVE_BE, name_lex.str);
        return BSTREAM_ERROR;
      }

      info->m_snap[n]= new Native_snapshot(info->m_ctx, snap->version, se);
                                                              // reports errors
      break;
    }

    case BI_CS:
      info->m_snap[n]= new CS_snapshot(info->m_ctx, snap->version);
                                                              // reports errors
      break;

    case BI_DEFAULT:
      info->m_snap[n]= new Default_snapshot(info->m_ctx, snap->version);
                                                              // reports errors
      break;

    default:
      // note: we use convention that snapshots are counted starting from 1.
      info->m_ctx.fatal_error(ER_BACKUP_UNKNOWN_BE, n + 1);
      return BSTREAM_ERROR;
    }

    if (!info->m_snap[n])
    {
      info->m_ctx.fatal_error(ER_OUT_OF_RESOURCES);
      return BSTREAM_ERROR;
    }

    info->m_snap[n]->m_num= n + 1;
    info->m_ctx.report_driver(info->m_snap[n]->name());
  }

  return BSTREAM_OK;
}

/**
  Called after reading backup image's catalogue and before processing
  metadata and table data.

  Nothing to do here.
*/
extern "C"
int bcat_close(st_bstream_image_header *catalogue)
{
  return BSTREAM_OK;
}

/**
  Add item to restore catalogue.

  @todo Report errors.
*/
extern "C"
int bcat_add_item(st_bstream_image_header *catalogue, 
                  struct st_bstream_item_info *item)
{
  using namespace backup;

  Restore_info *info= static_cast<Restore_info*>(catalogue);

  backup::String name_str(item->name.begin, item->name.end);

  DBUG_PRINT("restore",("Adding item %s of type %d (pos=%ld)",
                        item->name.begin,
                        item->type,
                        item->pos));

  switch (item->type) {

  case BSTREAM_IT_DB:
  {
    Image_info::Db *db= info->add_db(name_str, item->pos); // reports errors

    if (!db)
      return BSTREAM_ERROR;

    return BSTREAM_OK;
  }

  case BSTREAM_IT_TABLE:
  {
    st_bstream_table_info *it= (st_bstream_table_info*)item;

    DBUG_PRINT("restore",(" table's snapshot no. is %d", it->snap_num));

    Snapshot_info *snap= info->m_snap[it->snap_num];

    if (!snap)
    {
      /* 
        This can happen only if the snapshot number is too big - if we failed
        to create one of the snapshots listed in image's header we would stop
        with error earlier.
       */
      DBUG_ASSERT(it->snap_num >= info->snap_count());
      info->m_ctx.fatal_error(ER_BACKUP_WRONG_TABLE_BE, it->snap_num + 1);
      return BSTREAM_ERROR;
    }

    Image_info::Db *db= info->get_db(it->base.db->base.pos); // reports errors

    if (!db)
      return BSTREAM_ERROR;

    DBUG_PRINT("restore",(" table's database is %s", db->name().ptr()));

    Image_info::Table *tbl= info->add_table(*db, name_str, *snap, item->pos); 
                                                             // reports errors
    
    if (!tbl)
      return BSTREAM_ERROR;

    return BSTREAM_OK;
  }

  case BSTREAM_IT_VIEW:
  case BSTREAM_IT_SPROC:
  case BSTREAM_IT_SFUNC:
  case BSTREAM_IT_EVENT:
  case BSTREAM_IT_TRIGGER:
  {
    st_bstream_dbitem_info *it= (st_bstream_dbitem_info*)item;
    
    DBUG_ASSERT(it->db);
    
    Image_info::Db *db= (Image_info::Db*) info->get_db(it->db->base.pos);
  
    DBUG_ASSERT(db);
    
    Image_info::Dbobj *it1= info->add_db_object(*db, item->type, name_str,
                                                item->pos);
  
    if (!it1)
      return BSTREAM_ERROR;
    
    return BSTREAM_OK;
  }   

  default:
    return BSTREAM_OK;

  } // switch (item->type)
}

/*****************************************************************

   Iterators

 *****************************************************************/

static uint cset_iter;  ///< Used to implement trivial charset iterator.
static uint null_iter;  ///< Used to implement trivial empty iterator.

/// Return pointer to an instance of iterator of a given type.
extern "C"
void* bcat_iterator_get(st_bstream_image_header *catalogue, unsigned int type)
{
  DBUG_ASSERT(catalogue);

  Backup_info *info= static_cast<Backup_info*>(catalogue);

  switch (type) {

  case BSTREAM_IT_PERTABLE: // per-table objects
    return &null_iter;

  case BSTREAM_IT_CHARSET:  // character sets
    cset_iter= 0;
    return &cset_iter;

  case BSTREAM_IT_USER:     // users
    return &null_iter;

  case BSTREAM_IT_PERDB:    // per-db objects, except tables
  {
    Backup_info::Perdb_iterator *it= info->get_perdb();
  
    if (!it)
    {
      info->m_ctx.fatal_error(ER_BACKUP_LIST_PERDB);
      return NULL;
    }

    return it;
  }

  case BSTREAM_IT_GLOBAL:   // all global objects
    // note: only global items (for which meta-data is stored) are databases
  case BSTREAM_IT_DB:       // all databases
  {
    Backup_info::Db_iterator *it= info->get_dbs();
  
    if (!it)
    {
      info->m_ctx.fatal_error(ER_BACKUP_LIST_DBS);
      return NULL;
    }

    return it;
  }

  default:
    return NULL;

  }
}

/// Return next item pointed by a given iterator and advance it to the next positon.
extern "C"
struct st_bstream_item_info*
bcat_iterator_next(st_bstream_image_header *catalogue, void *iter)
{
  using namespace backup;

  /* If this is the null iterator, return NULL immediately */
  if (iter == &null_iter)
    return NULL;

  static bstream_blob name= {NULL, NULL};

  /*
    If it is cset iterator then cset_iter variable contains iterator position.
    We return only 2 charsets: the utf8 charset used to encode all strings and
    the default server charset.
  */
  if (iter == &cset_iter)
  {
    switch (cset_iter) {
      case 0: name.begin= (backup::byte*)my_charset_utf8_bin.csname; break;
      case 1: name.begin= (backup::byte*)system_charset_info->csname; break;
      default: name.begin= NULL; break;
    }

    name.end= name.begin ? name.begin + strlen((char*)name.begin) : NULL;
    cset_iter++;

    return name.begin ? (st_bstream_item_info*)&name : NULL;
  }

  /*
    In all other cases assume that iter points at instance of
    @c Image_info::Iterator and use this instance to get next item.
   */
  const Image_info::Obj *ptr= (*(Image_info::Iterator*)iter)++;

  return ptr ? (st_bstream_item_info*)(ptr->info()) : NULL;
}

extern "C"
void  bcat_iterator_free(st_bstream_image_header *catalogue, void *iter)
{
  /*
    Do nothing for the null and cset iterators, but delete the
    @c Image_info::Iterator object otherwise.
  */
  if (iter == &null_iter)
    return;

  if (iter == &cset_iter)
    return;

  delete (backup::Image_info::Iterator*)iter;
}

/* db-items iterator */

/** 
  Return pointer to an iterator for iterating over objects inside a given 
  database.
 */
extern "C"
void* bcat_db_iterator_get(st_bstream_image_header *catalogue,
                           st_bstream_db_info *dbi)
{
  DBUG_ASSERT(catalogue);
  DBUG_ASSERT(dbi);
  
  Backup_info *info= static_cast<Backup_info*>(catalogue);
  Backup_info::Db *db = info->get_db(dbi->base.pos);

  if (!db)
  {
    info->m_ctx.fatal_error(ER_BACKUP_UNKNOWN_OBJECT);
    return NULL;
  }

  Backup_info::Dbobj_iterator *it= info->get_db_objects(*db);

  if (!it)
  {
    info->m_ctx.fatal_error(ER_BACKUP_LIST_DB_TABLES);
    return NULL;
  }

  return it;
}

extern "C"
struct st_bstream_dbitem_info*
bcat_db_iterator_next(st_bstream_image_header *catalogue,
                      st_bstream_db_info *db,
                      void *iter)
{
  const backup::Image_info::Obj *ptr= (*(backup::Image_info::Iterator*)iter)++;

  return ptr ? (st_bstream_dbitem_info*)ptr->info() : NULL;
}

extern "C"
void  bcat_db_iterator_free(st_bstream_image_header *catalogue,
                            st_bstream_db_info *db,
                            void *iter)
{
  delete (backup::Image_info::Dbobj_iterator*)iter;
}


/*****************************************************************

   Services for backup stream library related to meta-data
   manipulation.

 *****************************************************************/

/**
  Create given item using serialization data read from backup image.

  @todo Decide what to do if unknown item type is found. Right now we
  bail out.
 */ 
extern "C"
int bcat_create_item(st_bstream_image_header *catalogue,
                     struct st_bstream_item_info *item,
                     bstream_blob create_stmt,
                     bstream_blob other_meta_data)
{
  using namespace backup;
  using namespace obs;

  DBUG_ASSERT(catalogue);
  DBUG_ASSERT(item);

  Restore_info *info= static_cast<Restore_info*>(catalogue);
  int create_err= 0;

  switch (item->type) {
  
  case BSTREAM_IT_DB:     create_err= ER_BACKUP_CANT_RESTORE_DB; break;
  case BSTREAM_IT_TABLE:  create_err= ER_BACKUP_CANT_RESTORE_TABLE; break;
  case BSTREAM_IT_VIEW:   create_err= ER_BACKUP_CANT_RESTORE_VIEW; break;
  case BSTREAM_IT_SPROC:  create_err= ER_BACKUP_CANT_RESTORE_SROUT; break;
  case BSTREAM_IT_SFUNC:  create_err= ER_BACKUP_CANT_RESTORE_SROUT; break;
  case BSTREAM_IT_EVENT:  create_err= ER_BACKUP_CANT_RESTORE_EVENT; break;
  case BSTREAM_IT_TRIGGER: create_err= ER_BACKUP_CANT_RESTORE_TRIGGER; break;
  
  /*
    TODO: Decide what to do when we come across unknown item:
    break the restore process as it is done now or continue
    with a warning?
  */

  default:
    info->m_ctx.fatal_error(ER_BACKUP_UNKNOWN_OBJECT_TYPE);
    return BSTREAM_ERROR;    
  }

  Image_info::Obj *obj= find_obj(*info, *item);

  if (!obj)
  {
    info->m_ctx.fatal_error(ER_BACKUP_UNKNOWN_OBJECT);
    return BSTREAM_ERROR;
  }

  backup::String sdata(create_stmt.begin, create_stmt.end);

  DBUG_PRINT("restore",("Creating item of type %d pos %ld: %s",
                         item->type, item->pos, sdata.ptr()));
  /*
    Note: The instance created by Image_info::Obj::materialize() is deleted
    when *info is destroyed.
   */ 
  obs::Obj *sobj= obj->materialize(0, sdata);

  Image_info::Obj::describe_buf buf;

  if (!sobj)
  {
    info->m_ctx.fatal_error(create_err, obj->describe(buf));
    return BSTREAM_ERROR;
  }

  if (sobj->execute(::current_thd))
  {
    info->m_ctx.fatal_error(create_err, obj->describe(buf));
    return BSTREAM_ERROR;
  }
  
  return BSTREAM_OK;
}

/**
  Get serialization string for a given object.
  
  The catalogue should contain @c Image_info::Obj instance corresponding to the
  object described by @c item. This instance should contain pointer to 
  @c obs::Obj instance which can be used for getting the serialization string.

  @todo Decide what to do with the serialization string buffer - is it 
  acceptable to re-use a single buffer as it is done now?
 */ 
extern "C"
int bcat_get_item_create_query(st_bstream_image_header *catalogue,
                               struct st_bstream_item_info *item,
                               bstream_blob *stmt)
{
  using namespace backup;
  using namespace obs;

  DBUG_ASSERT(catalogue);
  DBUG_ASSERT(item);
  DBUG_ASSERT(stmt);

  Backup_info *info= static_cast<Backup_info*>(catalogue);

  int meta_err;

  switch (item->type) {
  
  case BSTREAM_IT_DB:     meta_err= ER_BACKUP_GET_META_DB; break;
  case BSTREAM_IT_TABLE:  meta_err= ER_BACKUP_GET_META_TABLE; break;
  case BSTREAM_IT_VIEW:   meta_err= ER_BACKUP_GET_META_VIEW; break;
  case BSTREAM_IT_SPROC:  meta_err= ER_BACKUP_GET_META_SROUT; break;
  case BSTREAM_IT_SFUNC:  meta_err= ER_BACKUP_GET_META_SROUT; break;
  case BSTREAM_IT_EVENT:  meta_err= ER_BACKUP_GET_META_EVENT; break;
  case BSTREAM_IT_TRIGGER: meta_err= ER_BACKUP_GET_META_TRIGGER; break;
  
  /*
    This can't happen - the item was obtained from the backup kernel.
  */
  default: DBUG_ASSERT(FALSE);
  }

  Image_info::Obj *obj= find_obj(*info, *item);

  /*
    The catalogue should contain the specified object and it should have 
    a corresponding server object instance.
   */ 
  DBUG_ASSERT(obj);
  DBUG_ASSERT(obj->m_obj_ptr);
  
  /*
    Note: Using single buffer here means that the string returned by
    this function will live only until the next call. This should be fine
    given the current ussage of the function inside the backup stream library.
    
    TODO: document this or find better solution for string storage.
   */ 
  
  ::String *buf= &(info->serialization_buf);
  buf->length(0);

  if (obj->m_obj_ptr->serialize(::current_thd, buf))
  {
    Image_info::Obj::describe_buf dbuf;

    info->m_ctx.fatal_error(meta_err, obj->describe(dbuf));
    return BSTREAM_ERROR;    
  }

  stmt->begin= (backup::byte*)buf->ptr();
  stmt->end= stmt->begin + buf->length();

  return BSTREAM_OK;
}

/**
  Get extra meta-data (if any) for a given object.
 
  @note Extra meta-data is not used currently.
 */ 
extern "C"
int bcat_get_item_create_data(st_bstream_image_header *catalogue,
                            struct st_bstream_item_info *item,
                            bstream_blob *data)
{
  /* We don't use any extra data now */
  return BSTREAM_ERROR;
}


/*************************************************

                 Helper functions

 *************************************************/

namespace backup {

/** 
  Build linked @c TABLE_LIST list from a list stored in @c Table_list object.
 
  @note The order of tables in the returned list is different than in the 
  input list (reversed).

  @todo Decide what to do if errors are detected. For example, how to react
  if memory for TABLE_LIST structure could not be allocated?
 */
TABLE_LIST *build_table_list(const Table_list &tables, thr_lock_type lock)
{
  TABLE_LIST *tl= NULL;

  for( uint tno=0; tno < tables.count() ; tno++ )
  {
    TABLE_LIST *ptr= (TABLE_LIST*)my_malloc(sizeof(TABLE_LIST), MYF(MY_WME));
    DBUG_ASSERT(ptr);  // FIXME: report error instead
    bzero(ptr, sizeof(TABLE_LIST));

    Table_ref tbl= tables[tno];

    ptr->alias= ptr->table_name= const_cast<char*>(tbl.name().ptr());
    ptr->db= const_cast<char*>(tbl.db().name().ptr());
    ptr->lock_type= lock;

    // and add it to the list

    ptr->next_global= ptr->next_local=
      ptr->next_name_resolution_table= tl;
    tl= ptr;
    tl->table= ptr->table;
  }

  return tl;
}

} // backup namespace
