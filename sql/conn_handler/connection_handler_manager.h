/*
   Copyright (c) 2013, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/

#ifndef CONNECTION_HANDLER_MANAGER_INCLUDED
#define CONNECTION_HANDLER_MANAGER_INCLUDED

#include "my_global.h"          // uint
#include "connection_handler.h" // Connection_handler
#include "mysqld.h"             // LOCK_connection_count

class Channel_info;
class THD;


/**
  Callback functions to notify interested connection handlers
  of events like begining of wait and end of wait and post-kill
  notification events.
*/
struct Connection_handler_callback
{
  void (*thd_wait_begin)(THD* thd, int wait_type);
  void (*thd_wait_end)(THD* thd);
  void (*post_kill_notification)(THD* thd);
};


/**
  This is a singleton class that provides various connection management
  related functionalities, most importantly dispatching new connections
  to the currently active Connection_handler.
*/
class Connection_handler_manager
{
  // Singleton instance to Connection_handler_manager
  static Connection_handler_manager* m_instance;

  // Pointer to current connection handler in use
  Connection_handler* m_connection_handler;
  // Pointer to saved connection handler
  Connection_handler* m_saved_connection_handler;
  // Saved scheduler_type
  ulong m_saved_thread_handling;

  /**
    Increment connection count if max_connections is not exceeded.

    @retval   true if max_connections is not exceeded else false.
  */
  static bool check_and_incr_conn_count();

  /**
    Constructor to instantiate an instance of this class.
  */
  Connection_handler_manager(Connection_handler *connection_handler)
  : m_connection_handler(connection_handler),
    m_saved_connection_handler(NULL),
    m_saved_thread_handling(0)
  { }

  ~Connection_handler_manager()
  {
    delete m_connection_handler;
    if (m_saved_connection_handler)
      delete m_saved_connection_handler;
  }

  /* Make this class non-copyable */
  Connection_handler_manager(const Connection_handler_manager&);
  Connection_handler_manager& operator=(const Connection_handler_manager&);

public:
  /**
    thread_handling enumeration.

    The default of --thread-handling is the first one in the
    thread_handling_names array, this array has to be consistent with
    the order in this array, so to change default one has to change the
    first entry in this enum and the first entry in the
    thread_handling_names array.

    @note The last entry of the enumeration is also used to mark the
    thread handling as dynamic. In this case the name of the thread
    handling is fetched from the name of the plugin that implements it.
  */
  enum scheduler_types
  {
    SCHEDULER_ONE_THREAD_PER_CONNECTION=0,
    SCHEDULER_NO_THREADS,
    SCHEDULER_TYPES_COUNT
  };

  // Status variables related to connection management
  static ulong aborted_connects;
  static uint connection_count;          // Protected by LOCK_connection_count
  static ulong max_used_connections;
  static ulong thread_created;           // Protected by LOCK_thread_created
  // System variable
  static ulong thread_handling;
  // Callback for lock wait and post-kill notification events
  static Connection_handler_callback* callback;
  // Saved callback
  static Connection_handler_callback* saved_callback;

  /**
    Singleton method to return an instance of this class.
  */
  static Connection_handler_manager* get_instance()
  {
    DBUG_ASSERT(m_instance != NULL);
    return m_instance;
  }

  /**
    Initialize the connection handler manager.
    Must be called before get_instance() can be used.

    @return true if initialization failed, false otherwise.
  */
  static bool init();

  /**
    Destroy the singleton instance.
  */
  static void destroy_instance();

  /**
    Check if the current number of connections are below or equal
    the value given by the max_connections server system variable.

    @return true if a new connection can be accepted, false otherwise.
  */
  static bool valid_connection_count()
  {
    mysql_mutex_lock(&LOCK_connection_count);
    bool count_ok= (Connection_handler_manager::connection_count <= max_connections);
    mysql_mutex_unlock(&LOCK_connection_count);
    return count_ok;
  }

  /**
    @return Maximum number of threads that can be created by the current
            connection handler.
  */
  uint get_max_threads() const
  { return m_connection_handler->get_max_threads(); }

  /**
    Dynamically load a connection handler implemented as a plugin.
    The current connection handler will be saved so that it can
    later be restored by unload_connection_handler().
  */
  void load_connection_handler(Connection_handler* conn_handler);

  /**
    Unload the connection handler previously loaded by
    load_connection_handler(). The previous connection handler will
    be restored.

    @return true if unload failed (no previous connection handler was found).
  */
  bool unload_connection_handler();

  /**
    Process a new incoming connection.

    @param channel_info    Pointer to Channel_info object containing
                           connection channel information.
  */
  void process_new_connection(Channel_info* channel_info);

  void remove_connection(THD *thd);
};


/////////////////////////////////////////////////
// Functions needed by plugins (thread pool)
/////////////////////////////////////////////////

/**
  Create a THD object from channel_info.

  @note If creation fails, ER_OUT_OF_RESOURCES will be be reported.

  @param channel_info     Pointer to Channel_info object or NULL if
                          creation failed.
*/
THD* create_thd(Channel_info* channel_info);

void destroy_channel_info(Channel_info* channel_info);

void dec_connection_count();

void inc_thread_created();

void inc_aborted_connects();

#endif // CONNECTION_HANDLER_MANAGER_INCLUDED.
