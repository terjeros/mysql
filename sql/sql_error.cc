/* Copyright (C) 1995-2002 MySQL AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA */

/**********************************************************************
This file contains the implementation of error and warnings related

  - Whenever an error or warning occurred, it pushes it to a warning list
    that the user can retrieve with SHOW WARNINGS or SHOW ERRORS.

  - For each statement, we return the number of warnings generated from this
    command.  Note that this can be different from @@warning_count as
    we reset the warning list only for questions that uses a table.
    This is done to allow on to do:
    INSERT ...;
    SELECT @@warning_count;
    SHOW WARNINGS;
    (If we would reset after each command, we could not retrieve the number
     of warnings)

  - When client requests the information using SHOW command, then 
    server processes from this list and returns back in the form of 
    resultset.

    Supported syntaxes:

    SHOW [COUNT(*)] ERRORS [LIMIT [offset,] rows]
    SHOW [COUNT(*)] WARNINGS [LIMIT [offset,] rows]
    SELECT @@warning_count, @@error_count;

***********************************************************************/

#include "sql_error.h"
#include "mysql_priv.h"
#include "sp_rcontext.h"

/* Store a new message in an error object. */

void MYSQL_ERROR::set_msg(MEM_ROOT *warn_root, const char *msg_arg)
{
  msg= strdup_root(warn_root, msg_arg);
}


Warning_info::Warning_info(ulonglong warn_id_arg)
  :m_statement_warn_count(0),
  m_current_row_for_warning(1),
  m_warn_id(warn_id_arg)
{
  /* Initialize sub structures */
  init_sql_alloc(&m_warn_root, WARN_ALLOC_BLOCK_SIZE, WARN_ALLOC_PREALLOC_SIZE);
  m_warn_list.empty();
  bzero((char*) m_warn_count, sizeof(m_warn_count));
}


Warning_info::~Warning_info()
{
  free_root(&m_warn_root,MYF(0));
}


/**
  Reset the warning information of this connection.
*/

void Warning_info::clear_warning_info(ulonglong warn_id_arg)
{
  m_warn_id= warn_id_arg;
  free_root(&m_warn_root, MYF(0));
  bzero((char*) m_warn_count, sizeof(m_warn_count));
  m_warn_list.empty();
  m_statement_warn_count= 0;
  m_current_row_for_warning= 1; /* Start counting from the first row */
}


/**
  Add a warning to the list of warnings. Increment the respective
  counters.
*/

void Warning_info::push_warning(THD *thd,
                                MYSQL_ERROR::enum_warning_level level,
                                uint code, const char *msg)
{
  if (m_warn_list.elements < thd->variables.max_error_count)
  {
    MYSQL_ERROR *err;
    if ((err= new (&m_warn_root) MYSQL_ERROR(&m_warn_root, level, code, msg)))
      m_warn_list.push_back(err, &m_warn_root);
  }
  m_warn_count[(uint) level]++;
  m_statement_warn_count++;
}


/*
  Push the warning/error to error list if there is still room in the list

  SYNOPSIS
    push_warning()
    thd			Thread handle
    level		Severity of warning (note, warning, error ...)
    code		Error number
    msg			Clear error message
*/

void push_warning(THD *thd, MYSQL_ERROR::enum_warning_level level, 
                          uint code, const char *msg)
{
  DBUG_ENTER("push_warning");
  DBUG_PRINT("enter", ("code: %d, msg: %s", code, msg));

  DBUG_ASSERT(code != 0);
  DBUG_ASSERT(msg != NULL);

  if (level == MYSQL_ERROR::WARN_LEVEL_NOTE &&
      !(thd->options & OPTION_SQL_NOTES))
    DBUG_VOID_RETURN;

  if (! thd->spcont)
    thd->warning_info->opt_clear_warning_info(thd->query_id);

  thd->got_warning= 1;

  /* Abort if we are using strict mode and we are not using IGNORE */
  if ((int) level >= (int) MYSQL_ERROR::WARN_LEVEL_WARN &&
      thd->really_abort_on_warning())
  {
    /* Avoid my_message() calling push_warning */
    bool no_warnings_for_error= thd->no_warnings_for_error;
    sp_rcontext *spcont= thd->spcont;

    thd->no_warnings_for_error= 1;
    thd->spcont= NULL;

    thd->killed= THD::KILL_BAD_DATA;
    my_message(code, msg, MYF(0));

    thd->spcont= spcont;
    thd->no_warnings_for_error= no_warnings_for_error;
    /* Store error in error list (as my_message() didn't do it) */
    level= MYSQL_ERROR::WARN_LEVEL_ERROR;
  }

  if (thd->handle_error(level, code, msg))
    DBUG_VOID_RETURN;

  if (thd->spcont &&
      thd->spcont->handle_error(thd, level, code))
  {
    DBUG_VOID_RETURN;
  }
  query_cache_abort(&thd->query_cache_tls);

  thd->warning_info->push_warning(thd, level, code, msg);

  DBUG_VOID_RETURN;
}

/*
  Push the warning/error to error list if there is still room in the list

  SYNOPSIS
    push_warning_printf()
    thd			Thread handle
    level		Severity of warning (note, warning, error ...)
    code		Error number
    msg			Clear error message
*/

void push_warning_printf(THD *thd, MYSQL_ERROR::enum_warning_level level,
			 uint code, const char *format, ...)
{
  va_list args;
  char    warning[ERRMSGSIZE+20];
  DBUG_ENTER("push_warning_printf");
  DBUG_PRINT("enter",("warning: %u", code));

  DBUG_ASSERT(code != 0);
  DBUG_ASSERT(format != NULL);

  va_start(args,format);
  my_vsnprintf(warning, sizeof(warning), format, args);
  va_end(args);
  push_warning(thd, level, code, warning);
  DBUG_VOID_RETURN;
}


/*
  Send all notes, errors or warnings to the client in a result set

  SYNOPSIS
    mysqld_show_warnings()
    thd			Thread handler
    levels_to_show	Bitmap for which levels to show

  DESCRIPTION
    Takes into account the current LIMIT

  RETURN VALUES
    FALSE ok
    TRUE  Error sending data to client
*/

const LEX_STRING warning_level_names[]=
{
  { C_STRING_WITH_LEN("Note") },
  { C_STRING_WITH_LEN("Warning") },
  { C_STRING_WITH_LEN("Error") },
  { C_STRING_WITH_LEN("?") }
};

bool mysqld_show_warnings(THD *thd, ulong levels_to_show)
{  
  List<Item> field_list;
  DBUG_ENTER("mysqld_show_warnings");

  field_list.push_back(new Item_empty_string("Level", 7));
  field_list.push_back(new Item_return_int("Code",4, MYSQL_TYPE_LONG));
  field_list.push_back(new Item_empty_string("Message",MYSQL_ERRMSG_SIZE));

  if (thd->protocol->send_result_set_metadata(&field_list,
                                 Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
    DBUG_RETURN(TRUE);

  MYSQL_ERROR *err;
  SELECT_LEX *sel= &thd->lex->select_lex;
  SELECT_LEX_UNIT *unit= &thd->lex->unit;
  ha_rows idx= 0;
  Protocol *protocol=thd->protocol;

  unit->set_limit(sel);

  List_iterator_fast<MYSQL_ERROR> it(thd->warning_info->warn_list());
  while ((err= it++))
  {
    /* Skip levels that the user is not interested in */
    if (!(levels_to_show & ((ulong) 1 << err->level)))
      continue;
    if (++idx <= unit->offset_limit_cnt)
      continue;
    if (idx > unit->select_limit_cnt)
      break;
    protocol->prepare_for_resend();
    protocol->store(warning_level_names[err->level].str,
		    warning_level_names[err->level].length, system_charset_info);
    protocol->store((uint32) err->code);
    protocol->store(err->msg, strlen(err->msg), system_charset_info);
    if (protocol->write())
      DBUG_RETURN(TRUE);
  }
  my_eof(thd);
  DBUG_RETURN(FALSE);
}
