/*--------------------------------------------------------------------------
 * LuaSec 0.4.1
 * Copyright (C) 2006-2011 Bruno Silvestre
 *
 *--------------------------------------------------------------------------*/

#include <errno.h>
#include <string.h>

#if defined(WIN32)
#include <Winsock2.h>
#endif

#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <openssl/x509_vfy.h>
#include <openssl/err.h>

#include <lua.h>
#include <lauxlib.h>

#include <luasocket/io.h>
#include <luasocket/buffer.h>
#include <luasocket/timeout.h>
#include <luasocket/socket.h>

#include "x509.h"
#include "ssl.h"

/**
 * Underline socket error.
 */
static int lsec_socket_error()
{
#if defined(WIN32)
  return WSAGetLastError();
#else
  return errno;
#endif
}

/**
 * Map error code into string.
 */
static const char *ssl_ioerror(void *ctx, int err)
{
  if (err == LSEC_IO_SSL) {
    p_ssl ssl = (p_ssl) ctx;
    switch(ssl->error) {
    case SSL_ERROR_NONE: return "No error";
    case SSL_ERROR_ZERO_RETURN: return "closed";
    case SSL_ERROR_WANT_READ: return "wantread";
    case SSL_ERROR_WANT_WRITE: return "wantwrite";
    case SSL_ERROR_WANT_CONNECT: return "'connect' not completed";
    case SSL_ERROR_WANT_ACCEPT: return "'accept' not completed";
    case SSL_ERROR_WANT_X509_LOOKUP: return "Waiting for callback";
    case SSL_ERROR_SYSCALL: return "System error";
    case SSL_ERROR_SSL: return ERR_reason_error_string(ERR_get_error());
    default: return "Unknown SSL error";
    }
  }
  return socket_strerror(err);
}

/**
 * Close the connection before the GC collect the object.
 */
static int meth_destroy(lua_State *L)
{
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  if (ssl->state == LSEC_STATE_CONNECTED) {
    socket_setblocking(&ssl->sock);
    SSL_shutdown(ssl->ssl);
  }
  if (ssl->sock != SOCKET_INVALID) {
    socket_destroy(&ssl->sock);
  }
  ssl->state = LSEC_STATE_CLOSED;
  if (ssl->ssl) {
    /* Clear the registry */
    luaL_getmetatable(L, "SSL:Verify:Registry");
    lua_pushlightuserdata(L, (void*)ssl->ssl);
    lua_pushnil(L);
    lua_settable(L, -3);
    /* Destroy the object */
    SSL_free(ssl->ssl);
    ssl->ssl = NULL;
  }
  return 0;
}

/**
 * Perform the TLS/SSL handshake
 */
static int handshake(p_ssl ssl)
{
  int err;
  p_timeout tm = timeout_markstart(&ssl->tm);
  if (ssl->state == LSEC_STATE_CLOSED)
    return IO_CLOSED;
  for ( ; ; ) {
    ERR_clear_error();
    err = SSL_do_handshake(ssl->ssl);
    ssl->error = SSL_get_error(ssl->ssl, err);
    switch (ssl->error) {
    case SSL_ERROR_NONE:
      ssl->state = LSEC_STATE_CONNECTED;
      return IO_DONE;
    case SSL_ERROR_WANT_READ:
      err = socket_waitfd(&ssl->sock, WAITFD_R, tm);
      if (err == IO_TIMEOUT) return LSEC_IO_SSL;
      if (err != IO_DONE)    return err;
      break;
    case SSL_ERROR_WANT_WRITE:
      err = socket_waitfd(&ssl->sock, WAITFD_W, tm);
      if (err == IO_TIMEOUT) return LSEC_IO_SSL;
      if (err != IO_DONE)    return err;
      break;
    case SSL_ERROR_SYSCALL:
      if (ERR_peek_error())  {
        ssl->error = SSL_ERROR_SSL;
        return LSEC_IO_SSL;
      }
      if (err == 0)
        return IO_CLOSED;
      return lsec_socket_error();
    default:
      return LSEC_IO_SSL;
    }
  }
  return IO_UNKNOWN;
}

/**
 * Send data
 */
static int ssl_send(void *ctx, const char *data, size_t count, size_t *sent,
   p_timeout tm)
{
  int err;
  p_ssl ssl = (p_ssl)ctx;
  if (ssl->state != LSEC_STATE_CONNECTED)
    return IO_CLOSED;
  *sent = 0;
  for ( ; ; ) {
    ERR_clear_error();
    err = SSL_write(ssl->ssl, data, (int)count);
    ssl->error = SSL_get_error(ssl->ssl, err);
    switch (ssl->error) {
    case SSL_ERROR_NONE:
      *sent = err;
      return IO_DONE;
    case SSL_ERROR_WANT_READ: 
      err = socket_waitfd(&ssl->sock, WAITFD_R, tm);
      if (err == IO_TIMEOUT) return LSEC_IO_SSL;
      if (err != IO_DONE)    return err;
      break;
    case SSL_ERROR_WANT_WRITE:
      err = socket_waitfd(&ssl->sock, WAITFD_W, tm);
      if (err == IO_TIMEOUT) return LSEC_IO_SSL;
      if (err != IO_DONE)    return err;
      break;
    case SSL_ERROR_SYSCALL:
      if (ERR_peek_error())  {
        ssl->error = SSL_ERROR_SSL;
        return LSEC_IO_SSL;
      }
      if (err == 0)
        return IO_CLOSED;
      return lsec_socket_error();
    default:
      return LSEC_IO_SSL;
    }
  }
  return IO_UNKNOWN;
}

/**
 * Receive data
 */
static int ssl_recv(void *ctx, char *data, size_t count, size_t *got,
  p_timeout tm)
{
  int err;
  p_ssl ssl = (p_ssl)ctx;
  if (ssl->state != LSEC_STATE_CONNECTED)
    return IO_CLOSED;
  *got = 0;
  for ( ; ; ) {
    ERR_clear_error();
    err = SSL_read(ssl->ssl, data, (int)count);
    ssl->error = SSL_get_error(ssl->ssl, err);
    switch (ssl->error) {
    case SSL_ERROR_NONE:
      *got = err;
      return IO_DONE;
    case SSL_ERROR_ZERO_RETURN:
      *got = err;
      return IO_CLOSED;
    case SSL_ERROR_WANT_READ:
      err = socket_waitfd(&ssl->sock, WAITFD_R, tm);
      if (err == IO_TIMEOUT) return LSEC_IO_SSL;
      if (err != IO_DONE)    return err;
      break;
    case SSL_ERROR_WANT_WRITE:
      err = socket_waitfd(&ssl->sock, WAITFD_W, tm);
      if (err == IO_TIMEOUT) return LSEC_IO_SSL;
      if (err != IO_DONE)    return err;
      break;
    case SSL_ERROR_SYSCALL:
      if (ERR_peek_error())  {
        ssl->error = SSL_ERROR_SSL;
        return LSEC_IO_SSL;
      }
      if (err == 0)
        return IO_CLOSED;
      return lsec_socket_error();
    default:
      return LSEC_IO_SSL;
    }
  }
  return IO_UNKNOWN;
}

/**
 * Create a new TLS/SSL object and mark it as new.
 */
static int meth_create(lua_State *L)
{
  p_ssl ssl;
  int mode = lsec_getmode(L, 1);
  SSL_CTX *ctx = lsec_checkcontext(L, 1);

  if (mode == LSEC_MODE_INVALID) {
    lua_pushnil(L);
    lua_pushstring(L, "invalid mode");
    return 2;
  }
  ssl = (p_ssl)lua_newuserdata(L, sizeof(t_ssl));
  if (!ssl) {
    lua_pushnil(L);
    lua_pushstring(L, "error creating SSL object");
    return 2;
  }
  ssl->ssl = SSL_new(ctx);
  if (!ssl->ssl) {
    lua_pushnil(L);
    lua_pushstring(L, "error creating SSL object");
    return 2;
  }
  ssl->state = LSEC_STATE_NEW;
  SSL_set_fd(ssl->ssl, (int)SOCKET_INVALID);
  SSL_set_mode(ssl->ssl, SSL_MODE_ENABLE_PARTIAL_WRITE | 
    SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);
#if defined(SSL_MODE_RELEASE_BUFFERS)
  SSL_set_mode(ssl->ssl, SSL_MODE_RELEASE_BUFFERS);
#endif
  if (mode == LSEC_MODE_SERVER)
    SSL_set_accept_state(ssl->ssl);
  else
    SSL_set_connect_state(ssl->ssl);

  io_init(&ssl->io, (p_send)ssl_send, (p_recv)ssl_recv, 
    (p_error) ssl_ioerror, ssl);
  timeout_init(&ssl->tm, -1, -1);
  buffer_init(&ssl->buf, &ssl->io, &ssl->tm);

  luaL_getmetatable(L, "SSL:Connection");
  lua_setmetatable(L, -2);
  return 1;
}

/**
 * Buffer send function
 */
static int meth_send(lua_State *L) {
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  return buffer_meth_send(L, &ssl->buf);
}

/**
 * Buffer receive function
 */
static int meth_receive(lua_State *L) {
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  return buffer_meth_receive(L, &ssl->buf);
}

/**
 * Get the buffer's statistics.
 */
static int meth_getstats(lua_State *L) {
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  return buffer_meth_getstats(L, &ssl->buf);
}

/**
 * Set the buffer's statistics.
 */
static int meth_setstats(lua_State *L) {
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  return buffer_meth_setstats(L, &ssl->buf);
}

/**
 * Select support methods
 */
static int meth_getfd(lua_State *L)
{
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  lua_pushnumber(L, ssl->sock);
  return 1;
}

/**
 * Set the TLS/SSL file descriptor.
 * Call it *before* the handshake.
 */
static int meth_setfd(lua_State *L)
{
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  if (ssl->state != LSEC_STATE_NEW)
    luaL_argerror(L, 1, "invalid SSL object state");
  ssl->sock = luaL_checkint(L, 2);
  socket_setnonblocking(&ssl->sock);
  SSL_set_fd(ssl->ssl, (int)ssl->sock);
  return 0;
}

/**
 * Lua handshake function.
 */
static int meth_handshake(lua_State *L)
{
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  int err = handshake(ssl);
  if (err == IO_DONE) {
    lua_pushboolean(L, 1);
    return 1;
  }
  lua_pushboolean(L, 0);
  lua_pushstring(L, ssl_ioerror((void*)ssl, err));
  return 2;
}

/**
 * Close the connection.
 */
static int meth_close(lua_State *L)
{
  meth_destroy(L);
  return 0;
}

/**
 * Set timeout.
 */
static int meth_settimeout(lua_State *L)
{
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  return timeout_meth_settimeout(L, &ssl->tm);
}

/**
 * Check if there is data in the buffer.
 */
static int meth_dirty(lua_State *L)
{
  int res = 0;
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  if (ssl->state != LSEC_STATE_CLOSED)
    res = !buffer_isempty(&ssl->buf) || SSL_pending(ssl->ssl);
  lua_pushboolean(L, res);
  return 1;
}

/**
 * Return the state information about the SSL object.
 */
static int meth_want(lua_State *L)
{
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  int code = (ssl->state == LSEC_STATE_CLOSED)
             ? SSL_NOTHING
             : SSL_want(ssl->ssl);
  switch(code) {
  case SSL_NOTHING: lua_pushstring(L, "nothing"); break;
  case SSL_READING: lua_pushstring(L, "read"); break;
  case SSL_WRITING: lua_pushstring(L, "write"); break;
  case SSL_X509_LOOKUP: lua_pushstring(L, "x509lookup"); break;
  }
  return 1;
}
  
/**
 * Return the compression method used.
 */
static int meth_compression(lua_State *L)
{
  const COMP_METHOD *comp;
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  if (ssl->state != LSEC_STATE_CONNECTED) {
    lua_pushnil(L);
    lua_pushstring(L, "closed");
    return 2;
  }
  comp = SSL_get_current_compression(ssl->ssl);
  if (comp)
    lua_pushstring(L, SSL_COMP_get_name(comp));
  else
    lua_pushnil(L);
  return 1;
}

/**
 * Return the nth certificate of the peer's chain.
 */
static int meth_getpeercertificate(lua_State *L)
{
  int n;
  X509 *cert;
  STACK_OF(X509) *certs;
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  if (ssl->state != LSEC_STATE_CONNECTED) {
    lua_pushnil(L);
    lua_pushstring(L, "closed");
    return 2;
  }
  /* Default to the first cert */ 
  n = luaL_optint(L, 2, 1);                           
  /* This function is 1-based, but OpenSSL is 0-based */
  --n;
  if (n < 0) {
    lua_pushnil(L);
    lua_pushliteral(L, "invalid certificate index");
    return 2;
  }
  if (n == 0) {
    cert = SSL_get_peer_certificate(ssl->ssl);
    if (cert)
      lsec_pushx509(L, cert);
    else
      lua_pushnil(L);
    return 1;
  }
  /* In a server-context, the stack doesn't contain the peer cert,
   * so adjust accordingly.
   */
  if (ssl->ssl->server)
    --n;
  certs = SSL_get_peer_cert_chain(ssl->ssl);
  if (n >= sk_X509_num(certs)) {
    lua_pushnil(L);
    return 1;
  }
  cert = sk_X509_value(certs, n);
  /* Increment the reference counting of the object. */
  /* See SSL_get_peer_certificate() source code.     */
  CRYPTO_add(&cert->references, 1, CRYPTO_LOCK_X509);
  lsec_pushx509(L, cert);
  return 1;
}

/**
 * Return the chain of certificate of the peer.
 */
static int meth_getpeerchain(lua_State *L)
{
  int i;
  int idx = 1;
  int n_certs;
  X509 *cert;
  STACK_OF(X509) *certs;
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  if (ssl->state != LSEC_STATE_CONNECTED) {
    lua_pushnil(L);
    lua_pushstring(L, "closed");
    return 2;
  }
  lua_newtable(L);
  if (ssl->ssl->server) {
    lsec_pushx509(L, SSL_get_peer_certificate(ssl->ssl));
    lua_rawseti(L, -2, idx++);
  }
  certs = SSL_get_peer_cert_chain(ssl->ssl);
  n_certs = sk_X509_num(certs);
  for (i = 0; i < n_certs; i++) {
    cert = sk_X509_value(certs, i);
    /* Increment the reference counting of the object. */
    /* See SSL_get_peer_certificate() source code.     */
    CRYPTO_add(&cert->references, 1, CRYPTO_LOCK_X509);
    lsec_pushx509(L, cert);
    lua_rawseti(L, -2, idx++);
  }
  return 1;
}

/**
 * Copy the table src to the table dst.
 */
static void copy_error_table(lua_State *L, int src, int dst)
{
  lua_pushnil(L); 
  while (lua_next(L, src) != 0) {
    if (lua_istable(L, -1)) {
      /* Replace the table with its copy */
      lua_newtable(L);
      copy_error_table(L, dst+2, dst+3);
      lua_remove(L, dst+2);
    }
    lua_pushvalue(L, -2);
    lua_pushvalue(L, -2);
    lua_rawset(L, dst);
    /* Remove the value and leave the key */
    lua_pop(L, 1);
  }
}

/**
 * Return the verification state of the peer chain.
 */
static int meth_getpeerverification(lua_State *L)
{
  long err;
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  if (ssl->state != LSEC_STATE_CONNECTED) {
    lua_pushboolean(L, 0);
    lua_pushstring(L, "closed");
    return 2;
  }
  err = SSL_get_verify_result(ssl->ssl);
  if (err == X509_V_OK) {
    lua_pushboolean(L, 1);
    return 1;
  }
  luaL_getmetatable(L, "SSL:Verify:Registry");
  lua_pushlightuserdata(L, (void*)ssl->ssl);
  lua_gettable(L, -2);
  if (lua_isnil(L, -1))
    lua_pushstring(L, X509_verify_cert_error_string(err));
  else {
    /* Copy the table of errors to avoid modifications */
    lua_newtable(L);
    copy_error_table(L, lua_gettop(L)-1, lua_gettop(L));
  }
  lua_pushboolean(L, 0);
  lua_pushvalue(L, -2);
  return 2;
}

/**
 * Get the latest "Finished" message sent out.
 */
static int meth_getfinished(lua_State *L)
{
  size_t len = 0;
  char *buffer = NULL;
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  if (ssl->state != LSEC_STATE_CONNECTED) {
    lua_pushnil(L);
    lua_pushstring(L, "closed");
    return 2;
  }
  if ((len = SSL_get_finished(ssl->ssl, NULL, 0)) == 0)
    return 0;
  buffer = (char*)malloc(len);
  if (!buffer) {
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }
  SSL_get_finished(ssl->ssl, buffer, len);
  lua_pushlstring(L, buffer, len);
  free(buffer);
  return 1;
}

/**
 * Gets the latest "Finished" message received.
 */
static int meth_getpeerfinished(lua_State *L)
{
  size_t len = 0;
  char *buffer = NULL;
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  if (ssl->state != LSEC_STATE_CONNECTED) {
    lua_pushnil(L);
    lua_pushstring(L, "closed");
    return 0;
  }
  if ((len = SSL_get_peer_finished(ssl->ssl, NULL, 0)) == 0)
    return 0;
  buffer = (char*)malloc(len);
  if (!buffer) {
    lua_pushnil(L);
    lua_pushstring(L, "out of memory");
    return 2;
  }
  SSL_get_peer_finished(ssl->ssl, buffer, len);
  lua_pushlstring(L, buffer, len);
  free(buffer);
  return 1;
}

/**
 * Object information -- tostring metamethod
 */
static int meth_tostring(lua_State *L)
{
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  lua_pushfstring(L, "SSL connection: %p%s", ssl,
    ssl->state == LSEC_STATE_CLOSED ? " (closed)" : "");
  return 1;
}

/**
 * Add a method in the SSL metatable.
 */
static int meth_setmethod(lua_State *L)
{
  luaL_getmetatable(L, "SSL:Connection");
  lua_pushstring(L, "__index");
  lua_gettable(L, -2);
  lua_pushvalue(L, 1);
  lua_pushvalue(L, 2);
  lua_settable(L, -3);
  return 0;
}

/**
 * Return information about the connection.
 */
static int meth_info(lua_State *L)
{
  int bits = 0;
  int algbits = 0;
  char buf[256] = {0};
  const SSL_CIPHER *cipher;
  p_ssl ssl = (p_ssl)luaL_checkudata(L, 1, "SSL:Connection");
  cipher = SSL_get_current_cipher(ssl->ssl);
  if (!cipher)
    return 0;
  SSL_CIPHER_description(cipher, buf, sizeof(buf));
  bits = SSL_CIPHER_get_bits(cipher, &algbits);
  lua_pushstring(L, buf);
  lua_pushnumber(L, bits);
  lua_pushnumber(L, algbits);
  return 3;
}

static int meth_copyright(lua_State *L)
{
  lua_pushstring(L, "LuaSec 0.4.1 - Copyright (C) 2006-2011 Bruno Silvestre"
#if defined(WITH_LUASOCKET)
                    "\nLuaSocket 2.0.2 - Copyright (C) 2004-2007 Diego Nehab"
#endif
  );
  return 1;
}

/*---------------------------------------------------------------------------*/

/**
 * SSL methods 
 */
static luaL_Reg methods[] = {
  {"close",               meth_close},
  {"getfd",               meth_getfd},
  {"getfinished",         meth_getfinished},
  {"getpeercertificate",  meth_getpeercertificate},
  {"getpeerchain",        meth_getpeerchain},
  {"getpeerverification", meth_getpeerverification},
  {"getpeerfinished",     meth_getpeerfinished},
  {"getstats",            meth_getstats},
  {"setstats",            meth_setstats},
  {"dirty",               meth_dirty},
  {"dohandshake",         meth_handshake},
  {"receive",             meth_receive},
  {"send",                meth_send},
  {"settimeout",          meth_settimeout},
  {"want",                meth_want},
  {NULL,                  NULL}
};

/**
 * SSL metamethods.
 */
static luaL_Reg meta[] = {
  {"__gc",       meth_destroy},
  {"__tostring", meth_tostring},
  {NULL, NULL}
};

/**
 * SSL functions. 
 */
static luaL_Reg funcs[] = {
  {"compression", meth_compression},
  {"create",      meth_create},
  {"info",        meth_info},
  {"setfd",       meth_setfd},
  {"setmethod",   meth_setmethod},
  {"copyright",   meth_copyright},
  {NULL,          NULL}
};

/**
 * Initialize modules.
 */
#if (LUA_VERSION_NUM == 501)
LSEC_API int luaopen_ssl_core(lua_State *L)
{
  /* Initialize SSL */
  if (!SSL_library_init()) {
    lua_pushstring(L, "unable to initialize SSL library");
    lua_error(L);
  }
  OpenSSL_add_all_algorithms();
  SSL_load_error_strings();

#if defined(WITH_LUASOCKET)
  /* Initialize internal library */
  socket_open();
#endif
   
  /* Register the functions and tables */
  luaL_newmetatable(L, "SSL:Connection");
  luaL_register(L, NULL, meta);

  lua_newtable(L);
  luaL_register(L, NULL, methods);
  lua_setfield(L, -2, "__index");

  luaL_register(L, "ssl.core", funcs);
  lua_pushnumber(L, SOCKET_INVALID);
  lua_setfield(L, -2, "invalidfd");

  return 1;
}
#else
LSEC_API int luaopen_ssl_core(lua_State *L)
{
  /* Initialize SSL */
  if (!SSL_library_init()) {
    lua_pushstring(L, "unable to initialize SSL library");
    lua_error(L);
  }
  OpenSSL_add_all_algorithms();
  SSL_load_error_strings();

#if defined(WITH_LUASOCKET)
  /* Initialize internal library */
  socket_open();
#endif

  /* Register the functions and tables */
  luaL_newmetatable(L, "SSL:Connection");
  luaL_setfuncs(L, meta, 0);

  lua_newtable(L);
  luaL_setfuncs(L, methods, 0);
  lua_setfield(L, -2, "__index");

  lua_newtable(L);
  luaL_setfuncs(L, funcs, 0);
  lua_pushnumber(L, SOCKET_INVALID);
  lua_setfield(L, -2, "invalidfd");

  return 1;
}
#endif
