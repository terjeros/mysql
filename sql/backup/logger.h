#ifndef _BACKUP_LOGGER_H
#define _BACKUP_LOGGER_H

#include "mysql_priv.h"
#include <backup_stream.h>
#include <backup/error.h>
#include "si_logs.h"
#include "rpl_mi.h"

namespace backup {

/// Logging levels for messages generated by backup system
struct log_level {
 enum value {
    INFO=    MYSQL_ERROR::WARN_LEVEL_NOTE,
    WARNING= MYSQL_ERROR::WARN_LEVEL_WARN,
    ERROR=   MYSQL_ERROR::WARN_LEVEL_ERROR
 };

};

class Image_info;

/**
  This class exposes API used to output various messages during backup/restore
  process.

  Destination of the messages is determined by the implementation. Currently
  messages are:
  - for errors, reported to the online backup progress table,
  - for errors and warning, pushed on client's error stack,
  - written to error log and trace file (if enabled)
  

  Messages are intended for a DBA and thus should be localized. A message should
  be registered in errmsg.txt database and have assigned an error code. Printing
  message corresponding to an error code is done using @c report_error() methods.
 */
class Logger
{
 public:

   enum enum_type { BACKUP = 1, RESTORE } m_type;
   enum { CREATED, READY, RUNNING, DONE } m_state;

   Logger(THD*);
   ~Logger();
   int init(enum_type type, const char *query);

   int report_error(int error_code, ...);
   int report_error(log_level::value level, int error_code, ...);
   int report_error(const char *format, ...);
   int write_message(log_level::value level, const char *msg, ...);

   void report_start(time_t);
   void report_stop(time_t, bool);
   void report_state(enum_backup_state);
   void report_vp_time(time_t, bool);
   void report_binlog_pos(const st_bstream_binlog_pos&);
   void report_master_binlog_pos(const st_bstream_binlog_pos&);
   void report_driver(const char *driver);
   void report_backup_file(char * path);
   void report_stats_pre(const Image_info&);
   void report_stats_post(const Image_info&);
   ulonglong get_op_id() const 
   {
     DBUG_ASSERT(backup_log);
     return backup_log->get_backup_id(); 
   }
   
   void save_errors();
   void stop_save_errors();
   void clear_saved_errors();
   util::SAVED_MYSQL_ERROR *last_saved_error();
   bool push_errors(bool);

 protected:

  /// Thread in which this logger is used.
  THD *m_thd;

  int v_report_error(log_level::value, int, va_list);
  int v_write_message(log_level::value, int, const char*, va_list);
  int write_message(log_level::value level , int error_code, const char *msg);

 private:
  // Prevent copying/assigments
  Logger(const Logger&);
  Logger& operator=(const Logger&);

  util::SAVED_MYSQL_ERROR error;   ///< Used to store saved errors.
  bool m_save_errors;        ///< Flag telling if errors should be saved.
  bool m_push_errors;        ///< Should errors be pushed on warning stack?

  Backup_log *backup_log;    ///< Backup log interface class.
};

inline
Logger::Logger(THD *thd) 
  :m_type(BACKUP), m_state(CREATED),
   m_thd(thd), m_save_errors(FALSE), m_push_errors(TRUE), backup_log(0)
{
  clear_saved_errors();
}
 

inline
Logger::~Logger()
{
  clear_saved_errors();
  delete backup_log;
}

/// Report unregistered message.
inline
int Logger::write_message(log_level::value level, const char *msg, ...)
{
  va_list args;

  va_start(args, msg);
  int res= v_write_message(level, ER_UNKNOWN_ERROR, msg, args);
  va_end(args);

  return res;
}

/// Reports error with log_level::ERROR.
inline
int Logger::report_error(int error_code, ...)
{
  va_list args;

  va_start(args, error_code);
  int res= v_report_error(log_level::ERROR, error_code, args);
  va_end(args);

  return res;
}

/// Reports error with registered error description string.
inline
int Logger::report_error(log_level::value level, int error_code, ...)
{
  va_list args;

  va_start(args, error_code);
  int res= v_report_error(level, error_code, args);
  va_end(args);

  return res;
}

/// Reports error with given description.
inline
int Logger::report_error(const char *format, ...)
{
  va_list args;

  va_start(args, format);
  int res= v_write_message(log_level::ERROR, 0, format, args);
  va_end(args);

  return res;
}

///  Request that all reported errors are saved in the logger.
inline
void Logger::save_errors()
{
  if (m_save_errors)
    return;
  clear_saved_errors();
  m_save_errors= TRUE;
}

/// Stop saving errors.
inline
void Logger::stop_save_errors()
{
  if (!m_save_errors)
    return;
  m_save_errors= FALSE;
}

/// Delete all saved errors to free resources.
inline
void Logger::clear_saved_errors()
{
  memset(&error, 0, sizeof(error));
}

/// Return a pointer to most recent saved error.
inline
util::SAVED_MYSQL_ERROR *Logger::last_saved_error()
{ 
  return error.code ? &error : NULL;
}

/// Report start of an operation.
inline
void Logger::report_start(time_t when)
{
  DBUG_ASSERT(m_state == READY);
  DBUG_ASSERT(backup_log);
  m_state= RUNNING;
  
  report_error(log_level::INFO, m_type == BACKUP ? ER_BACKUP_BACKUP_START
                                                 : ER_BACKUP_RESTORE_START);  
  backup_log->start(when);
  backup_log->state(BUP_RUNNING);
}

/**
  Report end of the operation.
  
  @param[in] success indicates if the operation ended successfuly
 */
inline
void Logger::report_stop(time_t when, bool success)
{
  if (m_state == DONE)
    return;

  DBUG_ASSERT(m_state == RUNNING);
  DBUG_ASSERT(backup_log);

  report_error(log_level::INFO, m_type == BACKUP ? ER_BACKUP_BACKUP_DONE
                                                 : ER_BACKUP_RESTORE_DONE);  

  backup_log->stop(when);
  backup_log->state(success ? BUP_COMPLETE : BUP_ERRORS);
  backup_log->write_history();
  m_state= DONE;
}

/** 
  Report change of the state of operation
 
  For possible states see definition of @c enum_backup_state 

  @todo Consider reporting state changes in the server error log (as info
  entries).
 */
inline
void Logger::report_state(enum_backup_state state)
{
  DBUG_ASSERT(m_state == RUNNING || m_state == READY);
  DBUG_ASSERT(backup_log);
 
  backup_log->state(state);
}

/** 
  Report validity point creation time.

  @param[IN] when   the time of validity point
  @param[IN] report determines if VP time should be also reported in the
                    backup_progress log
*/
inline
void Logger::report_vp_time(time_t when, bool report)
{
  DBUG_ASSERT(m_state == RUNNING);
  DBUG_ASSERT(backup_log);
  backup_log->vp_time(when, report);
}

/** 
  Report binlog position at validity point.

  @todo Write binlog position also to server's error log (as info entry).
 */
inline
void Logger::report_binlog_pos(const st_bstream_binlog_pos &pos)
{
  DBUG_ASSERT(m_state == RUNNING);
  DBUG_ASSERT(backup_log);
  backup_log->binlog_pos(pos.pos);
  backup_log->binlog_file(pos.file);
}

/**
  Report master's binlog information.

  @todo Write this information to the backup image file.
*/
inline
void Logger::report_master_binlog_pos(const st_bstream_binlog_pos &pos)
{
  if (active_mi)
  {
    backup_log->master_binlog_pos(pos.pos);
    backup_log->master_binlog_file(pos.file);
    backup_log->write_master_binlog_info();
  }
}

/**
  Report driver.
*/
inline 
void Logger::report_driver(const char *driver) 
{ 
  DBUG_ASSERT(m_state == RUNNING);
  DBUG_ASSERT(backup_log);
  backup_log->add_driver(driver); 
}

/** 
  Report backup file and path.
*/
inline
void Logger::report_backup_file(char *path)
{ 
  DBUG_ASSERT(m_state == RUNNING);
  DBUG_ASSERT(backup_log);
  backup_log->backup_file(path); 
}

/**
  Initialize logger for backup or restore operation.
  
  A new id for that operation is assigned and stored in @c m_op_hist
  member.
  
  @param[in]  type  type of operation (backup or restore)
  @param[in]  query backup or restore query starting the operation
    
  @returns 0 on success, error code otherwise.

  @todo Decide what to do if @c initialize() signals errors.
  @todo Add code to get the user comment from command.
*/ 
inline
int Logger::init(enum_type type, const char *query)
{
  if (m_state != CREATED)
    return 0;

  m_type= type;
  m_state= READY;
  backup_log = new Backup_log(m_thd, (enum_backup_operation)type, query);
  backup_log->state(BUP_STARTING);
  DEBUG_SYNC(m_thd, "after_backup_log_init");
  return 0;
}

} // backup namespace

#endif
