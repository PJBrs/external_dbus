/* -*- mode: C; c-file-style: "gnu" -*- */
/* dbus-transport-unix.c UNIX socket subclasses of DBusTransport
 *
 * Copyright (C) 2002, 2003  Red Hat Inc.
 *
 * Licensed under the Academic Free License version 2.0
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "dbus-internals.h"
#include "dbus-connection-internal.h"
#include "dbus-transport-unix.h"
#include "dbus-transport-protected.h"
#include "dbus-watch.h"


/**
 * @defgroup DBusTransportUnix DBusTransport implementations for UNIX
 * @ingroup  DBusInternals
 * @brief Implementation details of DBusTransport on UNIX
 *
 * @{
 */

/**
 * Opaque object representing a Unix file descriptor transport.
 */
typedef struct DBusTransportUnix DBusTransportUnix;

/**
 * Implementation details of DBusTransportUnix. All members are private.
 */
struct DBusTransportUnix
{
  DBusTransport base;                   /**< Parent instance */
  int fd;                               /**< File descriptor. */
  DBusWatch *read_watch;                /**< Watch for readability. */
  DBusWatch *write_watch;               /**< Watch for writability. */

  int max_bytes_read_per_iteration;     /**< To avoid blocking too long. */
  int max_bytes_written_per_iteration;  /**< To avoid blocking too long. */

  int message_bytes_written;            /**< Number of bytes of current
                                         *   outgoing message that have
                                         *   been written.
                                         */
  DBusString encoded_outgoing;          /**< Encoded version of current
                                         *   outgoing message.
                                         */
  DBusString encoded_incoming;          /**< Encoded version of current
                                         *   incoming data.
                                         */
};

static void
free_watches (DBusTransport *transport)
{
  DBusTransportUnix *unix_transport = (DBusTransportUnix*) transport;
  
  if (unix_transport->read_watch)
    {
      if (transport->connection)
        _dbus_connection_remove_watch (transport->connection,
                                       unix_transport->read_watch);
      _dbus_watch_invalidate (unix_transport->read_watch);
      _dbus_watch_unref (unix_transport->read_watch);
      unix_transport->read_watch = NULL;
    }

  if (unix_transport->write_watch)
    {
      if (transport->connection)
        _dbus_connection_remove_watch (transport->connection,
                                       unix_transport->write_watch);
      _dbus_watch_invalidate (unix_transport->write_watch);
      _dbus_watch_unref (unix_transport->write_watch);
      unix_transport->write_watch = NULL;
    }
}

static void
unix_finalize (DBusTransport *transport)
{
  DBusTransportUnix *unix_transport = (DBusTransportUnix*) transport;
  
  free_watches (transport);

  _dbus_string_free (&unix_transport->encoded_outgoing);
  _dbus_string_free (&unix_transport->encoded_incoming);
  
  _dbus_transport_finalize_base (transport);

  _dbus_assert (unix_transport->read_watch == NULL);
  _dbus_assert (unix_transport->write_watch == NULL);
  
  dbus_free (transport);
}

static void
check_write_watch (DBusTransport *transport)
{
  DBusTransportUnix *unix_transport = (DBusTransportUnix*) transport;
  dbus_bool_t need_write_watch;

  if (transport->connection == NULL)
    return;

  if (transport->disconnected)
    {
      _dbus_assert (unix_transport->write_watch == NULL);
      return;
    }
  
  _dbus_transport_ref (transport);

  if (_dbus_transport_get_is_authenticated (transport))
    need_write_watch = transport->messages_need_sending;
  else
    need_write_watch = transport->send_credentials_pending ||
      _dbus_auth_do_work (transport->auth) == DBUS_AUTH_STATE_HAVE_BYTES_TO_SEND;

  _dbus_connection_toggle_watch (transport->connection,
                                 unix_transport->write_watch,
                                 need_write_watch);

  _dbus_transport_unref (transport);
}

static void
check_read_watch (DBusTransport *transport)
{
  DBusTransportUnix *unix_transport = (DBusTransportUnix*) transport;
  dbus_bool_t need_read_watch;

  if (transport->connection == NULL)
    return;

  if (transport->disconnected)
    {
      _dbus_assert (unix_transport->read_watch == NULL);
      return;
    }
  
  _dbus_transport_ref (transport);

  if (_dbus_transport_get_is_authenticated (transport))
    need_read_watch =
      _dbus_counter_get_value (transport->live_messages_size) < transport->max_live_messages_size;
  else
    need_read_watch = transport->receive_credentials_pending ||
      _dbus_auth_do_work (transport->auth) == DBUS_AUTH_STATE_WAITING_FOR_INPUT;

  _dbus_connection_toggle_watch (transport->connection,
                                 unix_transport->read_watch,
                                 need_read_watch);

  _dbus_transport_unref (transport);
}

static void
do_io_error (DBusTransport *transport)
{
  _dbus_transport_ref (transport);
  _dbus_transport_disconnect (transport);
  _dbus_transport_unref (transport);
}

/* return value is whether we successfully read any new data. */
static dbus_bool_t
read_data_into_auth (DBusTransport *transport,
                     dbus_bool_t   *oom)
{
  DBusTransportUnix *unix_transport = (DBusTransportUnix*) transport;
  DBusString *buffer;
  int bytes_read;
  
  *oom = FALSE;

  _dbus_auth_get_buffer (transport->auth, &buffer);
  
  bytes_read = _dbus_read (unix_transport->fd,
                           buffer, unix_transport->max_bytes_read_per_iteration);

  _dbus_auth_return_buffer (transport->auth, buffer,
                            bytes_read > 0 ? bytes_read : 0);

  if (bytes_read > 0)
    {
      _dbus_verbose (" read %d bytes in auth phase\n", bytes_read);

      return TRUE;
    }
  else if (bytes_read < 0)
    {
      /* EINTR already handled for us */

      if (errno == ENOMEM)
        {
          *oom = TRUE;
        }
      else if (errno == EAGAIN ||
               errno == EWOULDBLOCK)
        ; /* do nothing, just return FALSE below */
      else
        {
          _dbus_verbose ("Error reading from remote app: %s\n",
                         _dbus_strerror (errno));
          do_io_error (transport);
        }

      return FALSE;
    }
  else
    {
      _dbus_assert (bytes_read == 0);
      
      _dbus_verbose ("Disconnected from remote app\n");
      do_io_error (transport);

      return FALSE;
    }
}

/* Return value is whether we successfully wrote any bytes */
static dbus_bool_t
write_data_from_auth (DBusTransport *transport)
{
  DBusTransportUnix *unix_transport = (DBusTransportUnix*) transport;
  int bytes_written;
  const DBusString *buffer;

  if (!_dbus_auth_get_bytes_to_send (transport->auth,
                                     &buffer))
    return FALSE;
  
  bytes_written = _dbus_write (unix_transport->fd,
                               buffer,
                               0, _dbus_string_get_length (buffer));

  if (bytes_written > 0)
    {
      _dbus_auth_bytes_sent (transport->auth, bytes_written);
      return TRUE;
    }
  else if (bytes_written < 0)
    {
      /* EINTR already handled for us */
      
      if (errno == EAGAIN ||
          errno == EWOULDBLOCK)
        ;
      else
        {
          _dbus_verbose ("Error writing to remote app: %s\n",
                         _dbus_strerror (errno));
          do_io_error (transport);
        }
    }

  return FALSE;
}

static void
exchange_credentials (DBusTransport *transport,
                      dbus_bool_t    do_reading,
                      dbus_bool_t    do_writing)
{
  DBusTransportUnix *unix_transport = (DBusTransportUnix*) transport;

  if (do_writing && transport->send_credentials_pending)
    {
      if (_dbus_send_credentials_unix_socket (unix_transport->fd,
                                              NULL))
        {
          transport->send_credentials_pending = FALSE;
        }
      else
        {
          _dbus_verbose ("Failed to write credentials\n");
          do_io_error (transport);
        }
    }
  
  if (do_reading && transport->receive_credentials_pending)
    {
      if (_dbus_read_credentials_unix_socket (unix_transport->fd,
                                               &transport->credentials,
                                               NULL))
        {
          transport->receive_credentials_pending = FALSE;
        }
      else
        {
          _dbus_verbose ("Failed to read credentials\n");
          do_io_error (transport);
        }
    }

  if (!(transport->send_credentials_pending ||
        transport->receive_credentials_pending))
    {
      _dbus_auth_set_credentials (transport->auth,
                                  &transport->credentials);
    }
}

static dbus_bool_t
do_authentication (DBusTransport *transport,
                   dbus_bool_t    do_reading,
                   dbus_bool_t    do_writing)
{
  dbus_bool_t oom;
  
  _dbus_transport_ref (transport);

  oom = FALSE;
  
  while (!_dbus_transport_get_is_authenticated (transport) &&
         _dbus_transport_get_is_connected (transport))
    {      
      exchange_credentials (transport, do_reading, do_writing);
      
      if (transport->send_credentials_pending ||
          transport->receive_credentials_pending)
        {
          _dbus_verbose ("send_credentials_pending = %d receive_credentials_pending = %d\n",
                         transport->send_credentials_pending,
                         transport->receive_credentials_pending);
          goto out;
        }

#define TRANSPORT_SIDE(t) ((t)->is_server ? "server" : "client")
      switch (_dbus_auth_do_work (transport->auth))
        {
        case DBUS_AUTH_STATE_WAITING_FOR_INPUT:
          _dbus_verbose (" %s auth state: waiting for input\n",
                         TRANSPORT_SIDE (transport));
          if (!do_reading || !read_data_into_auth (transport, &oom))
            goto out;
          break;
      
        case DBUS_AUTH_STATE_WAITING_FOR_MEMORY:
          _dbus_verbose (" %s auth state: waiting for memory\n",
                         TRANSPORT_SIDE (transport));
          oom = TRUE;
          goto out;
          break;
      
        case DBUS_AUTH_STATE_HAVE_BYTES_TO_SEND:
          _dbus_verbose (" %s auth state: bytes to send\n",
                         TRANSPORT_SIDE (transport));
          if (!do_writing || !write_data_from_auth (transport))
            goto out;
          break;
      
        case DBUS_AUTH_STATE_NEED_DISCONNECT:
          _dbus_verbose (" %s auth state: need to disconnect\n",
                         TRANSPORT_SIDE (transport));
          do_io_error (transport);
          break;
      
        case DBUS_AUTH_STATE_AUTHENTICATED:
          _dbus_verbose (" %s auth state: authenticated\n",
                         TRANSPORT_SIDE (transport));
          break;
        }
    }
  
 out:
  check_read_watch (transport);
  check_write_watch (transport);
  _dbus_transport_unref (transport);

  if (oom)
    return FALSE;
  else
    return TRUE;
}

/* returns false on oom */
static dbus_bool_t
do_writing (DBusTransport *transport)
{
  int total;
  DBusTransportUnix *unix_transport = (DBusTransportUnix*) transport;
  dbus_bool_t oom;
  
  /* No messages without authentication! */
  if (!_dbus_transport_get_is_authenticated (transport))
    {
      _dbus_verbose ("Not authenticated, not writing anything\n");
      return TRUE;
    }

  if (transport->disconnected)
    {
      _dbus_verbose ("Not connected, not writing anything\n");
      return TRUE;
    }

#if 0
  _dbus_verbose ("do_writing(), have_messages = %d\n",
                 _dbus_connection_have_messages_to_send (transport->connection));
#endif
  
  oom = FALSE;
  total = 0;

  while (!transport->disconnected &&
         _dbus_connection_have_messages_to_send (transport->connection))
    {
      int bytes_written;
      DBusMessage *message;
      const DBusString *header;
      const DBusString *body;
      int header_len, body_len;
      int total_bytes_to_write;
      
      if (total > unix_transport->max_bytes_written_per_iteration)
        {
          _dbus_verbose ("%d bytes exceeds %d bytes written per iteration, returning\n",
                         total, unix_transport->max_bytes_written_per_iteration);
          goto out;
        }

      if (!dbus_watch_get_enabled (unix_transport->write_watch))
        {
          _dbus_verbose ("write watch disabled, not writing more stuff\n");
          goto out;
        }
      
      message = _dbus_connection_get_message_to_send (transport->connection);
      _dbus_assert (message != NULL);
      _dbus_message_lock (message);

#if 0
      _dbus_verbose ("writing message %p\n", message);
#endif
      
      _dbus_message_get_network_data (message,
                                      &header, &body);

      header_len = _dbus_string_get_length (header);
      body_len = _dbus_string_get_length (body);

      if (_dbus_auth_needs_encoding (transport->auth))
        {
          if (_dbus_string_get_length (&unix_transport->encoded_outgoing) == 0)
            {
              if (!_dbus_auth_encode_data (transport->auth,
                                           header, &unix_transport->encoded_outgoing))
                {
                  oom = TRUE;
                  goto out;
                }
              
              if (!_dbus_auth_encode_data (transport->auth,
                                           body, &unix_transport->encoded_outgoing))
                {
                  _dbus_string_set_length (&unix_transport->encoded_outgoing, 0);
                  oom = TRUE;
                  goto out;
                }
            }
          
          total_bytes_to_write = _dbus_string_get_length (&unix_transport->encoded_outgoing);

#if 0
          _dbus_verbose ("encoded message is %d bytes\n",
                         total_bytes_to_write);
#endif
          
          bytes_written =
            _dbus_write (unix_transport->fd,
                         &unix_transport->encoded_outgoing,
                         unix_transport->message_bytes_written,
                         total_bytes_to_write - unix_transport->message_bytes_written);
        }
      else
        {
          total_bytes_to_write = header_len + body_len;

#if 0
          _dbus_verbose ("message is %d bytes\n",
                         total_bytes_to_write);          
#endif
          
          if (unix_transport->message_bytes_written < header_len)
            {
              bytes_written =
                _dbus_write_two (unix_transport->fd,
                                 header,
                                 unix_transport->message_bytes_written,
                                 header_len - unix_transport->message_bytes_written,
                                 body,
                                 0, body_len);
            }
          else
            {
              bytes_written =
                _dbus_write (unix_transport->fd,
                             body,
                             (unix_transport->message_bytes_written - header_len),
                             body_len -
                             (unix_transport->message_bytes_written - header_len));
            }
        }

      if (bytes_written < 0)
        {
          /* EINTR already handled for us */
          
          if (errno == EAGAIN ||
              errno == EWOULDBLOCK)
            goto out;
          else
            {
              _dbus_verbose ("Error writing to remote app: %s\n",
                             _dbus_strerror (errno));
              do_io_error (transport);
              goto out;
            }
        }
      else
        {
          _dbus_verbose (" wrote %d bytes of %d\n", bytes_written,
                         total_bytes_to_write);
          
          total += bytes_written;
          unix_transport->message_bytes_written += bytes_written;

          _dbus_assert (unix_transport->message_bytes_written <=
                        total_bytes_to_write);
          
          if (unix_transport->message_bytes_written == total_bytes_to_write)
            {
              unix_transport->message_bytes_written = 0;
              _dbus_string_set_length (&unix_transport->encoded_outgoing, 0);

              _dbus_connection_message_sent (transport->connection,
                                             message);
            }
        }
    }

 out:
  if (oom)
    return FALSE;
  else
    return TRUE;
}

/* returns false on out-of-memory */
static dbus_bool_t
do_reading (DBusTransport *transport)
{
  DBusTransportUnix *unix_transport = (DBusTransportUnix*) transport;
  DBusString *buffer;
  int bytes_read;
  int total;
  dbus_bool_t oom;

  /* No messages without authentication! */
  if (!_dbus_transport_get_is_authenticated (transport))
    return TRUE;

  oom = FALSE;
  
  total = 0;

 again:
  
  /* See if we've exceeded max messages and need to disable reading */
  check_read_watch (transport);
  
  if (total > unix_transport->max_bytes_read_per_iteration)
    {
      _dbus_verbose ("%d bytes exceeds %d bytes read per iteration, returning\n",
                     total, unix_transport->max_bytes_read_per_iteration);
      goto out;
    }

  _dbus_assert (unix_transport->read_watch != NULL ||
                transport->disconnected);
  
  if (transport->disconnected)
    goto out;

  if (!dbus_watch_get_enabled (unix_transport->read_watch))
    return TRUE;
  
  if (_dbus_auth_needs_decoding (transport->auth))
    {
      if (_dbus_string_get_length (&unix_transport->encoded_incoming) > 0)
        bytes_read = _dbus_string_get_length (&unix_transport->encoded_incoming);
      else
        bytes_read = _dbus_read (unix_transport->fd,
                                 &unix_transport->encoded_incoming,
                                 unix_transport->max_bytes_read_per_iteration);

      _dbus_assert (_dbus_string_get_length (&unix_transport->encoded_incoming) ==
                    bytes_read);
      
      if (bytes_read > 0)
        {
          int orig_len;
          
          _dbus_message_loader_get_buffer (transport->loader,
                                           &buffer);

          orig_len = _dbus_string_get_length (buffer);
          
          if (!_dbus_auth_decode_data (transport->auth,
                                       &unix_transport->encoded_incoming,
                                       buffer))
            {
              _dbus_verbose ("Out of memory decoding incoming data\n");
              oom = TRUE;
              goto out;
            }

          _dbus_message_loader_return_buffer (transport->loader,
                                              buffer,
                                              _dbus_string_get_length (buffer) - orig_len);

          _dbus_string_set_length (&unix_transport->encoded_incoming, 0);
        }
    }
  else
    {
      _dbus_message_loader_get_buffer (transport->loader,
                                       &buffer);
      
      bytes_read = _dbus_read (unix_transport->fd,
                               buffer, unix_transport->max_bytes_read_per_iteration);
      
      _dbus_message_loader_return_buffer (transport->loader,
                                          buffer,
                                          bytes_read < 0 ? 0 : bytes_read);
    }
  
  if (bytes_read < 0)
    {
      /* EINTR already handled for us */

      if (errno == ENOMEM)
        {
          _dbus_verbose ("Out of memory in read()/do_reading()\n");
          oom = TRUE;
          goto out;
        }
      else if (errno == EAGAIN ||
               errno == EWOULDBLOCK)
        goto out;
      else
        {
          _dbus_verbose ("Error reading from remote app: %s\n",
                         _dbus_strerror (errno));
          do_io_error (transport);
          goto out;
        }
    }
  else if (bytes_read == 0)
    {
      _dbus_verbose ("Disconnected from remote app\n");
      do_io_error (transport);
      goto out;
    }
  else
    {
      _dbus_verbose (" read %d bytes\n", bytes_read);
      
      total += bytes_read;      

      if (_dbus_transport_queue_messages (transport) == DBUS_DISPATCH_NEED_MEMORY)
        {
          oom = TRUE;
          _dbus_verbose (" out of memory when queueing messages we just read in the transport\n");
          goto out;
        }
      
      /* Try reading more data until we get EAGAIN and return, or
       * exceed max bytes per iteration.  If in blocking mode of
       * course we'll block instead of returning.
       */
      goto again;
    }

 out:
  if (oom)
    return FALSE;
  else
    return TRUE;
}

static dbus_bool_t
unix_handle_watch (DBusTransport *transport,
                   DBusWatch     *watch,
                   unsigned int   flags)
{
  DBusTransportUnix *unix_transport = (DBusTransportUnix*) transport;

  _dbus_assert (watch == unix_transport->read_watch ||
                watch == unix_transport->write_watch);
  
  if (watch == unix_transport->read_watch &&
      (flags & DBUS_WATCH_READABLE))
    {
#if 1
      _dbus_verbose ("handling read watch\n");
#endif
      if (!do_authentication (transport, TRUE, FALSE))
        return FALSE;
      
      if (!do_reading (transport))
        {
          _dbus_verbose ("no memory to read\n");
          return FALSE;
        }
    }
  else if (watch == unix_transport->write_watch &&
           (flags & DBUS_WATCH_WRITABLE))
    {
#if 0
      _dbus_verbose ("handling write watch, messages_need_sending = %d\n",
                     transport->messages_need_sending);
#endif
      if (!do_authentication (transport, FALSE, TRUE))
        return FALSE;
      
      if (!do_writing (transport))
        {
          _dbus_verbose ("no memory to write\n");
          return FALSE;
        }
    }
#ifdef DBUS_ENABLE_VERBOSE_MODE
  else
    {
      if (watch == unix_transport->read_watch)
        _dbus_verbose ("asked to handle read watch with non-read condition 0x%x\n",
                       flags);
      else if (watch == unix_transport->write_watch)
        _dbus_verbose ("asked to handle write watch with non-write condition 0x%x\n",
                       flags);
      else
        _dbus_verbose ("asked to handle watch %p on fd %d that we don't recognize\n",
                       watch, dbus_watch_get_fd (watch));
    }
#endif /* DBUS_ENABLE_VERBOSE_MODE */

  if (flags & (DBUS_WATCH_HANGUP | DBUS_WATCH_ERROR))
    {
      _dbus_transport_disconnect (transport);
      return TRUE;
    }
  
  return TRUE;
}

static void
unix_disconnect (DBusTransport *transport)
{
  DBusTransportUnix *unix_transport = (DBusTransportUnix*) transport;
  
  free_watches (transport);
  
  _dbus_close (unix_transport->fd, NULL);
  unix_transport->fd = -1;
}

static dbus_bool_t
unix_connection_set (DBusTransport *transport)
{
  DBusTransportUnix *unix_transport = (DBusTransportUnix*) transport;

  _dbus_watch_set_handler (unix_transport->write_watch,
                           _dbus_connection_handle_watch,
                           transport->connection, NULL);

  _dbus_watch_set_handler (unix_transport->read_watch,
                           _dbus_connection_handle_watch,
                           transport->connection, NULL);
  
  if (!_dbus_connection_add_watch (transport->connection,
                                   unix_transport->write_watch))
    return FALSE;

  if (!_dbus_connection_add_watch (transport->connection,
                                   unix_transport->read_watch))
    {
      _dbus_connection_remove_watch (transport->connection,
                                     unix_transport->write_watch);
      return FALSE;
    }

  check_read_watch (transport);
  check_write_watch (transport);

  return TRUE;
}

static void
unix_messages_pending (DBusTransport *transport,
                       int            messages_pending)
{
  check_write_watch (transport);
}

/**
 * @todo We need to have a way to wake up the select sleep if
 * a new iteration request comes in with a flag (read/write) that
 * we're not currently serving. Otherwise a call that just reads
 * could block a write call forever (if there are no incoming
 * messages).
 */
static  void
unix_do_iteration (DBusTransport *transport,
                   unsigned int   flags,
                   int            timeout_milliseconds)
{
  DBusTransportUnix *unix_transport = (DBusTransportUnix*) transport;
  DBusPollFD poll_fd;
  int poll_res;
  int poll_timeout;

  _dbus_verbose (" iteration flags = %s%s timeout = %d read_watch = %p write_watch = %p\n",
                 flags & DBUS_ITERATION_DO_READING ? "read" : "",
                 flags & DBUS_ITERATION_DO_WRITING ? "write" : "",
                 timeout_milliseconds,
                 unix_transport->read_watch,
                 unix_transport->write_watch);
  
  /* the passed in DO_READING/DO_WRITING flags indicate whether to
   * read/write messages, but regardless of those we may need to block
   * for reading/writing to do auth.  But if we do reading for auth,
   * we don't want to read any messages yet if not given DO_READING.
   *
   * Also, if read_watch == NULL or write_watch == NULL, we don't
   * want to read/write so don't.
   */

  poll_fd.fd = unix_transport->fd;
  poll_fd.events = 0;
  
  if (_dbus_transport_get_is_authenticated (transport))
    {
      if (unix_transport->read_watch &&
          (flags & DBUS_ITERATION_DO_READING))
	poll_fd.events |= _DBUS_POLLIN;
      
      if (unix_transport->write_watch &&
          (flags & DBUS_ITERATION_DO_WRITING))
	poll_fd.events |= _DBUS_POLLOUT;
    }
  else
    {
      DBusAuthState auth_state;
      
      auth_state = _dbus_auth_do_work (transport->auth);

      if (transport->receive_credentials_pending ||
          auth_state == DBUS_AUTH_STATE_WAITING_FOR_INPUT)
	poll_fd.events |= _DBUS_POLLIN;

      if (transport->send_credentials_pending ||
          auth_state == DBUS_AUTH_STATE_HAVE_BYTES_TO_SEND)
	poll_fd.events |= _DBUS_POLLOUT;
    } 

  if (poll_fd.events)
    {
      if (flags & DBUS_ITERATION_BLOCK)
	poll_timeout = timeout_milliseconds;
      else
	poll_timeout = 0;

      /* For blocking selects we drop the connection lock here
       * to avoid blocking out connection access during a potentially
       * indefinite blocking call. The io path is still protected
       * by the io_path_cond condvar, so we won't reenter this.
       */
      if (flags & DBUS_ITERATION_BLOCK)
	_dbus_connection_unlock (transport->connection);
      
    again:
      poll_res = _dbus_poll (&poll_fd, 1, poll_timeout);

      if (poll_res < 0 && errno == EINTR)
	goto again;

      if (flags & DBUS_ITERATION_BLOCK)
	_dbus_connection_lock (transport->connection);
      
      if (poll_res >= 0)
        {
          if (poll_fd.revents & _DBUS_POLLERR)
            do_io_error (transport);
          else
            {
              dbus_bool_t need_read = (poll_fd.revents & _DBUS_POLLIN) > 0;
              dbus_bool_t need_write = (poll_fd.revents & _DBUS_POLLOUT) > 0;

              _dbus_verbose ("in iteration, need_read=%d need_write=%d\n",
                             need_read, need_write);
              do_authentication (transport, need_read, need_write);
                                 
              if (need_read && (flags & DBUS_ITERATION_DO_READING))
                do_reading (transport);
              if (need_write && (flags & DBUS_ITERATION_DO_WRITING))
                do_writing (transport);
            }
        }
      else
        {
          _dbus_verbose ("Error from _dbus_poll(): %s\n",
                         _dbus_strerror (errno));
        }
    }
}

static void
unix_live_messages_changed (DBusTransport *transport)
{
  /* See if we should look for incoming messages again */
  check_read_watch (transport);
}

static DBusTransportVTable unix_vtable = {
  unix_finalize,
  unix_handle_watch,
  unix_disconnect,
  unix_connection_set,
  unix_messages_pending,
  unix_do_iteration,
  unix_live_messages_changed
};

/**
 * Creates a new transport for the given file descriptor.  The file
 * descriptor must be nonblocking (use _dbus_set_fd_nonblocking() to
 * make it so). This function is shared by various transports that
 * boil down to a full duplex file descriptor.
 *
 * @param fd the file descriptor.
 * @param server #TRUE if this transport is on the server side of a connection
 * @param address the transport's address
 * @returns the new transport, or #NULL if no memory.
 */
DBusTransport*
_dbus_transport_new_for_fd (int               fd,
                            dbus_bool_t       server,
                            const DBusString *address)
{
  DBusTransportUnix *unix_transport;
  
  unix_transport = dbus_new0 (DBusTransportUnix, 1);
  if (unix_transport == NULL)
    return NULL;

  if (!_dbus_string_init (&unix_transport->encoded_outgoing))
    goto failed_0;

  if (!_dbus_string_init (&unix_transport->encoded_incoming))
    goto failed_1;
  
  unix_transport->write_watch = _dbus_watch_new (fd,
                                                 DBUS_WATCH_WRITABLE,
                                                 FALSE,
                                                 NULL, NULL, NULL);
  if (unix_transport->write_watch == NULL)
    goto failed_2;
  
  unix_transport->read_watch = _dbus_watch_new (fd,
                                                DBUS_WATCH_READABLE,
                                                FALSE,
                                                NULL, NULL, NULL);
  if (unix_transport->read_watch == NULL)
    goto failed_3;
  
  if (!_dbus_transport_init_base (&unix_transport->base,
                                  &unix_vtable,
                                  server, address))
    goto failed_4;
  
  unix_transport->fd = fd;
  unix_transport->message_bytes_written = 0;
  
  /* These values should probably be tunable or something. */     
  unix_transport->max_bytes_read_per_iteration = 2048;
  unix_transport->max_bytes_written_per_iteration = 2048;
  
  return (DBusTransport*) unix_transport;

 failed_4:
  _dbus_watch_unref (unix_transport->read_watch);
 failed_3:
  _dbus_watch_unref (unix_transport->write_watch);
 failed_2:
  _dbus_string_free (&unix_transport->encoded_incoming);
 failed_1:
  _dbus_string_free (&unix_transport->encoded_outgoing);
 failed_0:
  dbus_free (unix_transport);
  return NULL;
}

/**
 * Creates a new transport for the given Unix domain socket
 * path. This creates a client-side of a transport.
 *
 * @todo once we add a way to escape paths in a dbus
 * address, this function needs to do escaping.
 *
 * @param path the path to the domain socket.
 * @param abstract #TRUE to use abstract socket namespace
 * @param error address where an error can be returned.
 * @returns a new transport, or #NULL on failure.
 */
DBusTransport*
_dbus_transport_new_for_domain_socket (const char     *path,
                                       dbus_bool_t     abstract,
                                       DBusError      *error)
{
  int fd;
  DBusTransport *transport;
  DBusString address;
  
  _DBUS_ASSERT_ERROR_IS_CLEAR (error);

  if (!_dbus_string_init (&address))
    {
      dbus_set_error (error, DBUS_ERROR_NO_MEMORY, NULL);
      return NULL;
    }

  fd = -1;

  if ((abstract &&
       !_dbus_string_append (&address, "unix:abstract=")) ||
      (!abstract &&
       !_dbus_string_append (&address, "unix:path=")) ||
      !_dbus_string_append (&address, path))
    {
      dbus_set_error (error, DBUS_ERROR_NO_MEMORY, NULL);
      goto failed_0;
    }
  
  fd = _dbus_connect_unix_socket (path, abstract, error);
  if (fd < 0)
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      goto failed_0;
    }

  _dbus_fd_set_close_on_exec (fd);
  
  _dbus_verbose ("Successfully connected to unix socket %s\n",
                 path);

  transport = _dbus_transport_new_for_fd (fd, FALSE, &address);
  if (transport == NULL)
    {
      dbus_set_error (error, DBUS_ERROR_NO_MEMORY, NULL);
      goto failed_1;
    }
  
  _dbus_string_free (&address);
  
  return transport;

 failed_1:
  _dbus_close (fd, NULL);
 failed_0:
  _dbus_string_free (&address);
  return NULL;
}

/**
 * Creates a new transport for the given hostname and port.
 *
 * @param host the host to connect to
 * @param port the port to connect to
 * @param error location to store reason for failure.
 * @returns a new transport, or #NULL on failure.
 */
DBusTransport*
_dbus_transport_new_for_tcp_socket (const char     *host,
                                    dbus_int32_t    port,
                                    DBusError      *error)
{
  int fd;
  DBusTransport *transport;
  DBusString address;
  
  _DBUS_ASSERT_ERROR_IS_CLEAR (error);

  if (!_dbus_string_init (&address))
    {
      dbus_set_error (error, DBUS_ERROR_NO_MEMORY, NULL);
      return NULL;
    }
  
  if (!_dbus_string_append (&address, "tcp:host=") ||
      !_dbus_string_append (&address, host) ||
      !_dbus_string_append (&address, ",port=") ||
      !_dbus_string_append_int (&address, port))
    {
      _dbus_string_free (&address);
      dbus_set_error (error, DBUS_ERROR_NO_MEMORY, NULL);
      return NULL;
    }
  
  fd = _dbus_connect_tcp_socket (host, port, error);
  if (fd < 0)
    {
      _DBUS_ASSERT_ERROR_IS_SET (error);
      _dbus_string_free (&address);
      return NULL;
    }

  _dbus_fd_set_close_on_exec (fd);
  
  _dbus_verbose ("Successfully connected to tcp socket %s:%d\n",
                 host, port);
  
  transport = _dbus_transport_new_for_fd (fd, FALSE, &address);
  if (transport == NULL)
    {
      dbus_set_error (error, DBUS_ERROR_NO_MEMORY, NULL);
      _dbus_close (fd, NULL);
      _dbus_string_free (&address);
      fd = -1;
    }

  _dbus_string_free (&address);
  
  return transport;
}

/** @} */

