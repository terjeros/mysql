#ifndef _BACKUP_LOGGER_H
#define _BACKUP_LOGGER_H

#include "mysql_priv.h"
#include <backup_stream.h>
#include <backup/error.h>
#include <backup/backup_progress.h>


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

   enum enum_type { BACKUP, RESTORE } m_type;
   enum { CREATED, READY, RUNNING, DONE } m_state;

   Logger(THD*);
   ~Logger();
   int init(enum_type, const LEX_STRING, const char*);

   int report_error(int error_code, ...);
   int report_error(log_level::value level, int error_code, ...);
   int report_error(const char *format, ...);
   int write_message(log_level::value level, const char *msg, ...);

   void report_start(time_t);
   void report_stop(time_t, bool);
   void report_state(enum_backup_state);
   void report_vp_time(time_t);
   void report_binlog_pos(const st_bstream_binlog_pos&);
   void report_driver(const char*);
   void report_stats_pre(const Image_info&);
   void report_stats_post(const Image_info&);
   
   void save_errors();
   void stop_save_errors();
   void clear_saved_errors();
   MYSQL_ERROR *last_saved_error();

 protected:

  /// Thread in which this logger is used.
  THD *m_thd;

  /**
    Id of the backup or restore operation.
    
    This id is used in the backup progress and log tables to identify the
    operation. Value of @c m_op_id is meaningful only after a successful 
    call to @c init(), when @m_state != CREATED.
   */ 
  ulonglong m_op_id;

  int v_report_error(log_level::value, int, va_list);
  int v_write_message(log_level::value, int, const char*, va_list);
  int write_message(log_level::value level , int error_code, const char *msg);

 private:

  List<MYSQL_ERROR> errors;  ///< Used to store saved errors.
  bool m_save_errors;        ///< Flag telling if errors should be saved.
};

inline
Logger::Logger(THD *thd) 
  :m_type(BACKUP), m_state(CREATED),
   m_thd(thd), m_op_id(0), m_save_errors(FALSE)
{}

inline
Logger::~Logger()
{
  clear_saved_errors();
}

/**
  Initialize logger for backup or restore operation.
  
  A new id for that operation is assigned and stored in @c m_op_id
  member.
  
  @param[in]  type  type of operation (backup or restore)
  @param[in]  path  location of the backup image
  @param[in]  query backup or restore query starting the operation
  
  @returns 0 on success, error code otherwise.

  @todo Decide what to do if @c report_ob_init() signals errors.
 */ 
inline
int Logger::init(enum_type type, const LEX_STRING path, const char *query)
{
  if (m_state != CREATED)
    return 0;

  m_type= type;
  m_state= READY;

  // TODO: how to detect and report errors in report_ob_init()?
  m_op_id= report_ob_init(m_thd, m_thd->id, BUP_STARTING, 
                          type == BACKUP ? OP_BACKUP : OP_RESTORE, 
                          0, "", path.str, query);  
  DEBUG_SYNC(m_thd, "after_backup_log_init");
  return 0;
}


inline
int Logger::write_message(log_level::value level, const char *msg, ...)
{
  va_list args;

  va_start(args, msg);
  int res= v_write_message(level, 0, msg, args);
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
  errors.delete_elements();
}

/// Return a pointer to most recent saved error.
inline
MYSQL_ERROR *Logger::last_saved_error()
{ 
  return errors.is_empty() ? NULL : errors.head();
}

/// Report start of an operation.
inline
void Logger::report_start(time_t when)
{
  DBUG_ASSERT(m_state == READY);
  m_state= RUNNING;
  
  report_error(log_level::INFO, m_type == BACKUP ? ER_BACKUP_BACKUP_START
                                                 : ER_BACKUP_RESTORE_START);  
  report_ob_time(m_thd, m_op_id, when, 0);
  report_state(BUP_RUNNING);
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

  report_error(log_level::INFO, m_type == BACKUP ? ER_BACKUP_BACKUP_DONE
                                                 : ER_BACKUP_RESTORE_DONE);  
  report_ob_time(m_thd, m_op_id, 0, when);
  report_state(success ? BUP_COMPLETE : BUP_ERRORS);
  m_state= DONE;
}

/** 
  Report change of the state of operation
 
  For possible states see definition of @c enum_backup_state in 
  backup_progress.h

  @todo Consider reporting state changes in the server error log (as info
  entries).
 */
inline
void Logger::report_state(enum_backup_state state)
{
  DBUG_ASSERT(m_state == RUNNING);
  
  // TODO: info about state change in the log?
  report_ob_state(m_thd, m_op_id, state);
}

/// Report validity point creation time.
inline
void Logger::report_vp_time(time_t when)
{
  DBUG_ASSERT(m_state == RUNNING);
  
  report_ob_vp_time(m_thd, m_op_id, when);
}

/** 
  Report binlog position at validity point.

  @todo Write binlog position also to server's error log (as info entry).
 */
inline
void Logger::report_binlog_pos(const st_bstream_binlog_pos &pos)
{
  DBUG_ASSERT(m_state == RUNNING);
  
  // TODO: write to the log
  report_ob_binlog_info(m_thd, m_op_id, pos.pos, pos.file);
}

/// Report name of a driver used in backup/restore operation.
inline
void Logger::report_driver(const char *name)
{
  DBUG_ASSERT(m_state == READY || m_state == RUNNING);
  
  report_ob_engines(m_thd, m_op_id, name);
}

} // backup namespace

#endif
