/*
 * If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2017 RDK Management
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

/**
* @defgroup audioserver
* @{
* @defgroup audsrv
* @{
**/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>

#include "audsrv-logger.h"
#include "audsrv-protocol.h"
#include "audsrv-conn.h"

#include "audioserver-soc.h"

#include <vector>

#define FILE_LOCK_SUFFIX ".lock"
#define FILE_LOCK_SUFFIX_LEN (5)
#define FILE_LOCK_FLAGS (O_CREAT|O_CLOEXEC)
#define FILE_LOCK_MODE (S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP)

#define AUDSRV_MAX_MSG (1024)
#define AUDSRV_MAX_CAPTURE_DATA_SIZE (30*1024)
#define AUDSRV_SENDBUFFSIZE (80*1024)

#define LEVEL_DENOMINATOR (1000000)

typedef struct _AudsrvContext AudsrvContext;

typedef struct _AudsrvClient
{
   AudsrvContext *ctx;
   int fdSocket;
   struct ucred ucred;
   AudsrvConn *conn;
   pthread_t threadId;
   pthread_mutex_t mutex;
   bool clientStarted;
   bool clientReady;
   bool clientAbort;
   bool stopRequested;
   AudSrvCaptureParameters captureParams;
   AudSrvSocClient soc;
   unsigned sessionType;
   char sessionName[AUDSRV_MAX_SESSION_NAME_LEN+1];
} AudsrvClient;

typedef struct _AudsrvContext
{
   char *serverName;
   struct sockaddr_un addr;
   char lockName[sizeof(addr.sun_path)+FILE_LOCK_SUFFIX_LEN];
   int fdLock;
   int fdSocket;
   
   AudSrvSoc soc;
   
   pthread_mutex_t mutex;
   std::vector<AudsrvClient*> clients;
   std::vector<AudsrvClient*> clientsSessionEvent;

} AudsrvContext;

static AudsrvContext* audsrv_create_server_context( const char *name );
static void audsrv_destroy_server_context( AudsrvContext* ctx );
static bool audsrv_create_server_socket( AudsrvContext *ctx );
static AudsrvClient* audsrv_create_client( AudsrvContext *ctx, int fd );
static void audsrv_destroy_client( AudsrvContext *ctx, AudsrvClient *client );
static void* audsrv_client_thread( void *arg );
static int audsrv_process_message( AudsrvClient *client );
static int audsrv_process_init( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_audioinfo( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_basetime( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_play( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_stop( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_pause( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_unpause( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_flush( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_audiosync( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_audiodata( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_audiodatahandle( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_mute( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_unmute( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_volume( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_enable_session_event( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_disable_session_event( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_enableeos( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_disableeos( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_startcapture( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_stopcapture( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_enumsessions( AudsrvClient *client, unsigned msglen, unsigned version );
static int audsrv_process_getstatus( AudsrvClient *client, unsigned msglen, unsigned version );
static void audsrv_eos_callback( void *userData );
static void audsrv_first_audio_callback( void *userData );
static void audsrv_pts_error_callback( void *userData, unsigned count );
static void audsrv_underflow_callback( void *userData, unsigned count, unsigned bufferedBytes, unsigned queuedFrames );
static void audsrv_capture_callback( void *userData, AudSrvCaptureParameters *params, unsigned char *data, int datalen );
static void audsrv_distribute_session_event( AudsrvContext *ctx, int event, AudsrvClient *clientSubject );
static void audsrv_send_session_event( AudsrvClient *client, int event, AudsrvClient *clientSubject );
static bool audsrv_send_eos_detected( AudsrvClient *client );
static bool audsrv_send_first_audio( AudsrvClient *client );
static bool audsrv_send_pts_error( AudsrvClient *client, unsigned count );
static bool audsrv_send_underflow( AudsrvClient *client, unsigned count, unsigned bufferedBytes, unsigned queuedFrames );
static bool audsrv_send_capture_params( AudsrvClient *client );
static bool audsrv_send_capture_data( AudsrvClient *client, unsigned char *data, int datalen );
static bool audsrv_send_capture_done( AudsrvClient *client );
static bool audsrv_send_enum_session_results( AudsrvClient *client, unsigned long long token, int sessionCount, unsigned char *data, int datalen );
static bool audsrv_send_getstatus_results( AudsrvClient *client, unsigned long long token, AudSrvSessionStatus *status );

static bool g_running= false;

static long long getCurrentTimeMicro()
{
   struct timeval tv;
   long long utcCurrentTimeMicro;

   gettimeofday(&tv,0);
   utcCurrentTimeMicro= tv.tv_sec*1000000LL+tv.tv_usec;

   return utcCurrentTimeMicro;
}

static void signalHandler(int signum)
{
   printf("signalHandler: signum %d\n", signum);
   g_running= false;
}

static AudsrvContext* audsrv_create_server_context( const char *name )
{
   AudsrvContext *ctx= 0;

   ctx= (AudsrvContext*)calloc( 1, sizeof(AudsrvContext) );
   TRACE1("audsrv_create_server_context: ctx %p", ctx);
   if ( ctx )
   {
      ctx->fdLock= -1;
      ctx->fdSocket= -1;
      ctx->serverName= strdup( name );
      pthread_mutex_init( &ctx->mutex, 0 );
      ctx->clients= std::vector<AudsrvClient*>();
      ctx->clientsSessionEvent= std::vector<AudsrvClient*>();
      if ( !ctx->serverName )
      {
         ERROR("audsrv_create_server_context: unable to duplicate server name");
         goto error;
      }
      
      ctx->soc= AudioServerSocOpen();
      if ( !ctx->soc )
      {
         ERROR("audsrv_create_server_context: unable to open audioserver-soc");
         goto error;
      }
      
      return ctx;
   }   
   
error:
   audsrv_destroy_server_context( ctx );
   
   return 0;   
}

static void audsrv_destroy_server_context( AudsrvContext* ctx )
{
   TRACE1("audsrv_destroy_server_context: ctx %p", ctx);
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutex );
      
      while( ctx->clients.size() > 0 )
      {
         AudsrvClient *client= ctx->clients.back();
         audsrv_destroy_client( ctx, client );
         ctx->clients.pop_back();
      }
      
      while( ctx->clientsSessionEvent.size() > 0 )
      {
         ctx->clientsSessionEvent.pop_back();
      }
      
      if ( ctx->fdSocket >= 0 )
      {
         close(ctx->fdSocket);
         ctx->fdSocket= -1;
      }

      if ( ctx->addr.sun_path )
      {
         (void)unlink( ctx->addr.sun_path );
         ctx->addr.sun_path[0]= '\0';
      }
      
      if ( ctx->fdLock >= 0 )
      {
         close(ctx->fdLock);
         ctx->fdLock= -1;
      }

      if ( ctx->lockName )
      {
         (void)unlink( ctx->lockName );
         ctx->lockName[0]= '\0';
      }

      if ( ctx->serverName )
      {
         free( ctx->serverName );
         ctx->serverName= 0;
      }
      
      if ( ctx->soc )
      {
         AudioServerSocClose( ctx->soc );
         ctx->soc= 0;
      }

      pthread_mutex_unlock( &ctx->mutex );

      pthread_mutex_destroy( &ctx->mutex );
      
      free( ctx );
   }
}

static bool audsrv_create_server_socket( AudsrvContext *ctx )
{
   bool result= false;
   const char *workDir;
   int pathSize, maxPathSize;
   socklen_t addrSize;
   int rc;
   
   workDir= getenv("XDG_RUNTIME_DIR");
   if ( !workDir )
   {
      ERROR("XDG_RUNTIME_DIR is not set");
      goto exit;
   }
   
   ctx->addr.sun_family= AF_LOCAL;
   maxPathSize= (int)sizeof(ctx->addr.sun_path);
   pathSize= snprintf(ctx->addr.sun_path, 
                      maxPathSize,
			             "%s/%s", 
			             workDir, 
			             ctx->serverName) + 1;
   if ( pathSize < 0 )
   {
      ERROR("error formulating socket name");
      goto exit;
   }
   
   if ( pathSize > maxPathSize )
   {
      ERROR("full socket name length (%d) exceeds allowed length of %d bytes", pathSize, maxPathSize );
      goto exit;
   }
   
   TRACE1("full socket name (%s)", ctx->addr.sun_path );

   snprintf( ctx->lockName, 
             sizeof(ctx->lockName),
		       "%s%s", 
		       ctx->addr.sun_path, 
		       FILE_LOCK_SUFFIX);

   ctx->fdLock= open(ctx->lockName, FILE_LOCK_FLAGS, FILE_LOCK_MODE );
   if ( ctx->fdLock < 0 )
   {
      ERROR("unable to create lock file (%s) errno %d", ctx->lockName, errno );
   }
   
   rc= flock(ctx->fdLock, LOCK_EX|LOCK_NB);
   if ( rc < 0 )
   {
      ERROR("unable to lock: check if another server is running with the name %s", ctx->serverName );
      goto exit;
   }

   // We've got the lock, delete any existing socket file
   (void)unlink(ctx->addr.sun_path);

   ctx->fdSocket= socket( PF_LOCAL, SOCK_STREAM|SOCK_CLOEXEC, 0 );
   if ( ctx->fdSocket < 0 )
   {
      ERROR("unable to open socket: errno %d", errno );
      goto exit;
   }
   
   addrSize= offsetof(struct sockaddr_un, sun_path) + pathSize;
   
   rc= bind(ctx->fdSocket, (struct sockaddr *)&ctx->addr, addrSize );
   if ( rc < 0 )
   {
      ERROR("bind failed for socket: errno %d", errno );
      goto exit;
   }
   
   rc= listen(ctx->fdSocket, 1);
   if ( rc < 0 )
   {
      ERROR("listen failed for socket: errno %d", errno );
      goto exit;
   }
   
   result= true;

exit:

   if ( !result )
   {
      if ( ctx->fdSocket >= 0 )
      {
         close(ctx->fdSocket);
         ctx->fdSocket= -1;
      }

      if ( ctx->addr.sun_path )
      {
         (void)unlink( ctx->addr.sun_path );
         ctx->addr.sun_path[0]= '\0';
      }
      
      if ( ctx->fdLock >= 0 )
      {
         close(ctx->fdLock);
         ctx->fdLock= -1;
      }

      if ( ctx->lockName )
      {
         (void)unlink( ctx->lockName );
         ctx->lockName[0]= '\0';
      }
   }
      
   return result;
}

static void showUsage()
{
   printf("usage:\n");
   printf(" audioserver [options]\n" );
   printf("where [options] are:\n" );
   printf("  --name <server name> : audio server name to use\n" );
   printf("  --help : show usage\n" );
   printf("  -? : show usage\n" );
   printf("\n" );}

int main( int argc, const char **argv )
{
   int nRC= 0;
   int len;
   bool error= false;
   const char *serverName= "audsrv0";
   AudsrvContext *ctx= 0;
   
   printf( "audioserver\n");

   for( int i= 1; i < argc; ++i )
   {
      len= strlen(argv[i]);

      if ( (len == 6) && !strncmp( (const char*)argv[i], "--name", len) )
      {
         if ( i < argc-1 )
         {
            ++i;
            serverName= argv[i];
         }
         else
         {
            error= true;
            printf("--name option missing name\n" );
         }
      }
      else
      if ( ((len == 2) && 
            !strncmp( (const char*)argv[i], "-?", len)) ||
           ((len == 6) &&
             !strncmp( (const char*)argv[i], "--help", len)) )
      {
         showUsage();
         goto exit;
      }
      else
      {
         printf("ignoring unrecognized argument (%s)\n", argv[i] );
      }
   }
   
   if ( !error )
   {
      struct sigaction sigint;

      sigint.sa_handler= signalHandler;
      sigemptyset(&sigint.sa_mask);
      sigint.sa_flags= SA_RESETHAND;
      sigaction(SIGINT, &sigint, NULL);

      char *env= getenv( "AUDSRV_DEBUG" );
      if ( env )
      {
         int level= atoi( env );
         audsrv_set_log_level( level );
      }
      
      INFO("using server name: %s", serverName);
   
      ctx= audsrv_create_server_context( serverName );
      if ( !ctx )
      {
         ERROR("unable allocate server context");
         goto exit;
      }
      
      if ( audsrv_create_server_socket( ctx ) )
      {
         g_running= true;      
         while( g_running )
         {
            int fd;
            struct sockaddr_un addr;
            socklen_t addrLen= sizeof(addr);
            
            INFO("listening for connections...");
            fd= accept4( ctx->fdSocket, (struct sockaddr *)&addr, &addrLen, SOCK_CLOEXEC );
            if ( fd >= 0 )
            {
               INFO("received connection on fd %d", fd);
               if ( g_running )
               {
                  AudsrvClient *client= audsrv_create_client( ctx, fd );                  
                  if ( client )
                  {
                     INFO("created client %p for fd %d", client, fd);
                     pthread_mutex_lock( &ctx->mutex );
                     ctx->clients.push_back( client );
                     pthread_mutex_unlock( &ctx->mutex );
                  }
                  else
                  {
                     ERROR("unable to create client for fd %d", fd);
                  }
               }
               else
               {
                  close( fd );
               }
            }
         }
      }
   }

exit:

   if ( ctx )
   {
      audsrv_destroy_server_context( ctx );
   }

   return nRC;
}

static AudsrvClient* audsrv_create_client( AudsrvContext *ctx, int fd )
{
   AudsrvClient *client= 0;
   int rc;
   bool error= true;
   socklen_t optlen;
   
   client= (AudsrvClient*)calloc( 1, sizeof(AudsrvClient) );
   if ( client )
   {
      pthread_mutex_init( &client->mutex, 0 );
      client->ctx= ctx;
      client->fdSocket= fd;
      client->captureParams.version= (unsigned)-1;
      
      optlen= sizeof(struct ucred);
      rc= getsockopt( client->fdSocket, SOL_SOCKET, SO_PEERCRED, 
                      &client->ucred, &optlen );
      if ( rc < 0 )
      {
         ERROR("unable to get client credentials from socket: errno %d", errno );
      }
      else
      {
         client->conn= audsrv_conn_init( client->fdSocket, AUDSRV_MAX_MSG, AUDSRV_SENDBUFFSIZE );
         if ( !client->conn )
         {
            ERROR("unable to initialize connection");
         }
         else
         {
            rc= pthread_create( &client->threadId, NULL, audsrv_client_thread, client );
            if ( !rc )
            {
               bool ready, abort;
               
               INFO("client %p pid %d connected", client, client->ucred.pid);
               
               for( int i= 0; i < 500; ++i )
               {
                  pthread_mutex_lock( &client->mutex );
                  ready= client->clientReady;
                  abort= client->clientAbort;
                  pthread_mutex_unlock( &client->mutex );

                  if ( ready )
                  {
                     TRACE1("client %p ready", client);
                     break;
                  }
                  
                  if ( abort )
                  {
                     TRACE1("client %p aborting", client);
                     break;
                  }

                  usleep( 10000 );
               }
               
               if ( ready )
               {
                  error= false;
               }
               else
               {
                  ERROR("client thread failed to become ready");
               }         
            }
            else
            {
               ERROR("error creating thread for client %p fd %d", client, fd );
            }
         }
      }      
   }
   else
   {
      ERROR("unable to allocate new client");
   }
   
   if ( error )
   {
      pthread_mutex_lock( &ctx->mutex );
      audsrv_destroy_client( ctx, client );
      pthread_mutex_unlock( &ctx->mutex );
      client= 0;
   }
   
   return client;
}

static void audsrv_destroy_client( AudsrvContext *ctx, AudsrvClient *client )
{
   if ( client )
   {
      if ( client->fdSocket >= 0 )
      {
         TRACE1("audsrv_destroy_client: shutting down socket fd %d for client %p", client->fdSocket, client );
         shutdown( client->fdSocket, SHUT_RDWR );
      }

      if ( client->clientStarted )
      {
         client->stopRequested= true;
         pthread_mutex_unlock( &ctx->mutex );
         TRACE1("audsrv_destroy_client: requested stop, calling join for client %p", client);
         pthread_join( client->threadId, NULL );
         TRACE1("audsrv_destroy_client: join complete for client %p", client);
         pthread_mutex_lock( &ctx->mutex );
      }
      
      if ( client->soc )
      {
         AudioServerSocCloseClient( client->soc );
         client->soc= 0;
      }

      if ( client->conn )
      {
         audsrv_conn_term( client->conn );
         client->conn= 0;
      }      
      
      if ( client->fdSocket >= 0 )
      {
         TRACE1("audsrv_destroy_client: closing fd %d for client %p", client->fdSocket, client );
         close( client->fdSocket );
         client->fdSocket= -1;
      }
      
      pthread_mutex_destroy( &client->mutex );
      
      free( client );
   }
}

static void* audsrv_client_thread( void *arg )
{
   AudsrvClient *client= (AudsrvClient*)arg;
   
   TRACE1( "audsrv_client_thread: enter: client %p fd %d pid %d", client, client->fdSocket, client->ucred.pid );
   
   client->clientStarted= true;

   pthread_mutex_lock( &client->mutex );
   client->clientReady= true;
   pthread_mutex_unlock( &client->mutex );
   
   while( !client->stopRequested && !client->clientAbort )
   {
      int consumed;
      
      int len= audsrv_conn_recv( client->conn );

      if ( client->conn->peerDisconnected && !client->stopRequested )
      {
         INFO("audsrv_client_thread: client %p pid %d disconnected", client, client->ucred.pid);
      }

      if ( client->conn->count >= AUDSRV_MSG_HDR_LEN )
      {
         TRACE2("audsrv_client_thread: client %p received %d bytes", client, client->conn->count );
                  
         do
         {
            consumed= audsrv_process_message( client );
         }
         while( consumed > 0 );
      }
      else if ( client->conn->peerDisconnected )
      {
         break;
      }
   }
   
   if ( client->soc )
   {   
      AudioServerSocCloseClient( client->soc );      
      client->soc= 0;
   }
   else if ( client->sessionType != AUDSRV_SESSION_Observer )
   {
      ERROR("audsrv_client_thread: enter: client %p fd %d pid %d: unable to open soc client, aborting", client, client->fdSocket, client->ucred.pid );
   }

exit:
   TRACE1( "audsrv_client_thread: exit: client %p fd %d", client, client->fdSocket );
   
   if ( !client->stopRequested && client->conn->peerDisconnected )
   {
      AudsrvContext *ctx= client->ctx;
      pthread_mutex_lock( &ctx->mutex );
      for( std::vector<AudsrvClient*>::iterator it= ctx->clientsSessionEvent.begin();
           it != ctx->clientsSessionEvent.end();
           ++it )
      {
         if ( client == (*it) )
         {
            ctx->clientsSessionEvent.erase( it );
            break;
         }
      }
      pthread_mutex_unlock( &ctx->mutex );

      audsrv_distribute_session_event( client->ctx, AUDSRV_SESSIONEVENT_Removed, client );

      pthread_mutex_lock( &ctx->mutex );
      for( std::vector<AudsrvClient*>::iterator it= ctx->clients.begin();
           it != ctx->clients.end();
           ++it )
      {
         if ( client == (*it) )
         {
            ctx->clients.erase( it );
            audsrv_destroy_client( ctx, client );
            break;
         }
      }
      pthread_mutex_unlock( &ctx->mutex );
   }

   client->clientStarted= false;
   
   return NULL;
}

static int audsrv_process_message( AudsrvClient *client )
{
   int consumed= 0;
   int avail= client->conn->count;
   unsigned msglen, msgid, version;
   
   if ( avail < AUDSRV_MSG_HDR_LEN )
   {
      goto exit;
   }
   
   msglen= audsrv_conn_peek_u32( client->conn );
   TRACE2("audsrv_process_message: peeked msglen %d", msglen);
   if ( msglen+AUDSRV_MSG_HDR_LEN > client->conn->count )
   {
      goto exit;
   }
   
   audsrv_conn_skip( client->conn, 4 );
   msgid= audsrv_conn_get_u32( client->conn );
   version= audsrv_conn_get_u32( client->conn );
   TRACE2("audsrv_process_message: msglen %d msgid %d version %d", msglen, msgid, version);
   
   consumed += AUDSRV_MSG_HDR_LEN;
      
   switch( msgid )
   {
      case AUDSRV_MSG_Init:
         consumed += audsrv_process_init( client, msglen, version );
         break;
         
      case AUDSRV_MSG_AudioInfo:
         consumed += audsrv_process_audioinfo( client, msglen, version );
         break;

      case AUDSRV_MSG_Play:
         consumed += audsrv_process_play( client, msglen, version );
         break;

      case AUDSRV_MSG_Basetime:
         consumed += audsrv_process_basetime( client, msglen, version );
         break;

      case AUDSRV_MSG_Stop:
         consumed += audsrv_process_stop( client, msglen, version );
         break;

      case AUDSRV_MSG_Pause:
         consumed += audsrv_process_pause( client, msglen, version );
         break;

      case AUDSRV_MSG_UnPause:
         consumed += audsrv_process_unpause( client, msglen, version );
         break;

      case AUDSRV_MSG_Flush:
         consumed += audsrv_process_flush( client, msglen, version );
         break;
         
      case AUDSRV_MSG_AudioSync:
         consumed += audsrv_process_audiosync( client, msglen, version );
         break;
         
      case AUDSRV_MSG_AudioData:
         consumed += audsrv_process_audiodata( client, msglen, version );
         break;
         
      case AUDSRV_MSG_AudioDataHandle:
         consumed += audsrv_process_audiodatahandle( client, msglen, version );
         break;
 
      case AUDSRV_MSG_Mute:
         consumed += audsrv_process_mute( client, msglen, version );
         break;
 
      case AUDSRV_MSG_UnMute:
         consumed += audsrv_process_unmute( client, msglen, version );
         break;
        
      case AUDSRV_MSG_Volume:
         consumed += audsrv_process_volume( client, msglen, version );
         break;

      case AUDSRV_MSG_EnableSessionEvent:
         consumed += audsrv_process_enable_session_event( client, msglen, version );
         break;

      case AUDSRV_MSG_DisableSessionEvent:
         consumed += audsrv_process_disable_session_event( client, msglen, version );
         break;

      case AUDSRV_MSG_EnableEOS:
         consumed += audsrv_process_enableeos( client, msglen, version );
         break;
        
      case AUDSRV_MSG_DisableEOS:
         consumed += audsrv_process_disableeos( client, msglen, version );
         break;
        
      case AUDSRV_MSG_StartCapture:
         consumed += audsrv_process_startcapture( client, msglen, version );
         break;
        
      case AUDSRV_MSG_StopCapture:
         consumed += audsrv_process_stopcapture( client, msglen, version );
         break;

      case AUDSRV_MSG_EnumSessions:
         consumed += audsrv_process_enumsessions( client, msglen, version );
         break;

      case AUDSRV_MSG_GetStatus:
         consumed += audsrv_process_getstatus( client, msglen, version );
         break;

      case AUDSRV_MSG_EOSDetected:
      case AUDSRV_MSG_FirstAudio:
      case AUDSRV_MSG_PtsError:
      case AUDSRV_MSG_Underflow:
      case AUDSRV_MSG_CaptureParameters:
      case AUDSRV_MSG_CaptureData:
      case AUDSRV_MSG_CaptureDone:
      case AUDSRV_MSG_EnumSessionsResults:
      case AUDSRV_MSG_GetStatusResults:
      case AUDSRV_MSG_SessionEvent:
         ERROR("ignoring msg %d inappropriate for server to receive", msgid);
         audsrv_conn_skip( client->conn, msglen );
         consumed += msglen;
         break;
        
      default:
         INFO("ignoring unknown command %d len %d", msgid, msglen );
         audsrv_conn_skip( client->conn, msglen );
         consumed += msglen;
         break;
   }
   
exit:

   return consumed;
}

static int audsrv_process_init( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: init version %d", version);
   
   if ( version <= AUDSRV_MSG_Init_Version )
   {
      unsigned len, type, value;
      unsigned sessionType;
      bool isPrivate;
      char name[AUDSRV_MAX_SESSION_NAME_LEN+1];
      char *sessionName= 0;

      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_U16 )
      {
         ERROR("expecting type %d (U16) not type %d for init arg 1 (sessionType)", AUDSRV_TYPE_U16, type );
         goto exit;
      }

      value= audsrv_conn_get_u16( client->conn );
      switch( value )
      {
         case AUDSRV_SESSION_Primary:
         case AUDSRV_SESSION_Secondary:
         case AUDSRV_SESSION_Effect:
         case AUDSRV_SESSION_Capture:
            sessionType= (AUDSRV_SESSIONTYPE)value;
            break;
         default:
            WARNING("unknown sesson type: using secondary");
            sessionType= AUDSRV_SESSION_Secondary;
            break;
      }

      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_U16 )
      {
         ERROR("expecting type %d (U16) not type %d for init arg 2 (isPrivate)", AUDSRV_TYPE_U16, type );
         goto exit;
      }

      value= audsrv_conn_get_u16( client->conn );
      isPrivate= (value ? true : false);


      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_String )
      {
         ERROR("expecting type %d (string) not type %d for init arg 3 (sessionName)", AUDSRV_TYPE_String, type );
         goto exit;
      }
      if ( len > AUDSRV_MAX_SESSION_NAME_LEN )
      {
         ERROR("sessionName too long (%d) for init", len );
         goto exit;
      }
      
      if ( len )
      {
         audsrv_conn_get_string( client->conn, name );
         sessionName= name;
      }
      INFO("sessionType %d sessionName (%s) isPrivate %d", sessionType, sessionName, isPrivate);
      
      client->soc= AudioServerSocOpenClient( client->ctx->soc, sessionType, isPrivate, sessionName );
      if ( !client->soc )
      {
         ERROR("AudioServerSocOpenClient failed: sessionType %u", sessionType);
         goto exit;
      }

      client->sessionType= sessionType;
      if ( sessionName )
      {
         int len= strlen(sessionName);
         strncpy( client->sessionName, sessionName, len );
         client->sessionName[len]= 0;
      }

      audsrv_distribute_session_event( client->ctx, AUDSRV_SESSIONEVENT_Added, client );

      AudioServerSocSetFirstAudioFrameCallback( client->soc, audsrv_first_audio_callback, client );

      AudioServerSocSetPTSErrorCallback( client->soc, audsrv_pts_error_callback, client );

      AudioServerSocSetUnderflowCallback( client->soc, audsrv_underflow_callback, client );
   }

exit:
   
   return msglen;
}

static int audsrv_process_audioinfo( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: audioinfo version %d", version);

   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_AudioInfo_Version )
      {
         AudSrvAudioInfo audioInfo;
         unsigned len, type;

         len= audsrv_conn_get_u32( client->conn );
         type= audsrv_conn_get_u32( client->conn );
         
         if ( type != AUDSRV_TYPE_String )
         {
            ERROR("expecting type %d (string) not type %d for audioinfo arg 1 (mimeType)", AUDSRV_TYPE_String, type );
            goto exit;
         }
         if ( len > AUDSRV_MAX_MIME_LEN )
         {
            ERROR("mimetype too long (%d) for audioinfo", len );
            goto exit;
         }
         
         audsrv_conn_get_string( client->conn, audioInfo.mimeType );
         
         
         len= audsrv_conn_get_u32( client->conn );
         type= audsrv_conn_get_u32( client->conn );
         
         if ( type != AUDSRV_TYPE_U16 )
         {
            ERROR("expecting type %d (U16) not type %d for audioinfo arg 2 (codec)", AUDSRV_TYPE_U16, type );
            goto exit;
         }
         
         audioInfo.codec= audsrv_conn_get_u16( client->conn );
         
         
         len= audsrv_conn_get_u32( client->conn );
         type= audsrv_conn_get_u32( client->conn );
         
         if ( type != AUDSRV_TYPE_U16 )
         {
            ERROR("expecting type %d (U16) not type %d for audioinfo arg 3 (pid)", AUDSRV_TYPE_U16, type );
            goto exit;
         }
         
         audioInfo.pid= audsrv_conn_get_u16( client->conn );
         
         
         len= audsrv_conn_get_u32( client->conn );
         type= audsrv_conn_get_u32( client->conn );
         
         if ( type != AUDSRV_TYPE_U32 )
         {
            ERROR("expecting type %d (U32) not type %d for audioinfo arg 4 (ptsOffset)", AUDSRV_TYPE_U32, type );
            goto exit;
         }
         
         audioInfo.ptsOffset= audsrv_conn_get_u32( client->conn );
         
         
         len= audsrv_conn_get_u32( client->conn );
         type= audsrv_conn_get_u32( client->conn );
         
         if ( type != AUDSRV_TYPE_U64 )
         {
            ERROR("expecting type %d (U64) not type %d for audioinfo arg 5 (dataId)", AUDSRV_TYPE_U64, type );
            goto exit;
         }
         
         audioInfo.dataId= audsrv_conn_get_u64( client->conn );


         len= audsrv_conn_get_u32( client->conn );
         type= audsrv_conn_get_u32( client->conn );
         
         if ( type != AUDSRV_TYPE_U64 )
         {
            ERROR("expecting type %d (U64) not type %d for audioinfo arg 6 (timingId)", AUDSRV_TYPE_U64, type );
            goto exit;
         }
         
         audioInfo.timingId= audsrv_conn_get_u64( client->conn );


         len= audsrv_conn_get_u32( client->conn );
         type= audsrv_conn_get_u32( client->conn );
         
         if ( type != AUDSRV_TYPE_U64 )
         {
            ERROR("expecting type %d (U64) not type %d for audioinfo arg 7 (privateId)", AUDSRV_TYPE_U64, type );
            goto exit;
         }
         
         audioInfo.privateId= audsrv_conn_get_u64( client->conn );

         
         TRACE1("audioInfo: codec %d pid 0x%X ptsOffset:%u mimeType(%s) timingId %llX dataId %llX privateId %llX", 
                 audioInfo.codec, audioInfo.pid, audioInfo.ptsOffset, audioInfo.mimeType, audioInfo.timingId, audioInfo.dataId, audioInfo.privateId );
         
         if ( !AudioServerSocSetAudioInfo( client->soc, &audioInfo ) )
         {
            ERROR("AudioServerSocSetAudioInfo failed");
         }
      }
   }
   else
   {
      ERROR("msg: audioInfo: no soc");
   }

exit:
   
   return msglen;
}

static int audsrv_process_basetime( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: basetime version %d", version);

   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_Basetime_Version )
      {
         unsigned len, type;
         unsigned long long basetime;
         
         len= audsrv_conn_get_u32( client->conn );
         type= audsrv_conn_get_u32( client->conn );
         
         if ( type != AUDSRV_TYPE_U64 )
         {
            ERROR("expecting type %d (U64) not type %d for basetime arg 1 (basetime)", AUDSRV_TYPE_U64, type );
            goto exit;
         }
         
         basetime= audsrv_conn_get_u64( client->conn );
         
         TRACE1("msg: basetime basetime %lld", basetime );

         if ( !AudioServerSocBasetime( client->soc, basetime ) )
         {
            ERROR("AudioServerSocBasetime failed");
         }
      }
   }
   else
   {
      ERROR("msg: basetime: no soc");
   }

exit:
   
   return msglen;
}

static int audsrv_process_play( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: play version %d", version);
   
   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_Play_Version )
      {
         if ( !AudioServerSocPlay( client->soc ) )
         {
            ERROR("AudioServerSocPlay failed");
         }
      }
   }
   else
   {
      ERROR("msg: play: no soc");
   }
   
   return msglen;
}

static int audsrv_process_stop( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: stop version %d", version);
   
   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_Stop_Version )
      {
         if ( !AudioServerSocStop( client->soc ) )
         {
            ERROR("AudioServerSocStop failed");
         }
      }
   }
   else
   {
      ERROR("msg: stop: no soc");
   }
   
   return msglen;
}

static int audsrv_process_pause( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: pause version %d", version);
   
   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_Pause_Version )
      {
         if ( !AudioServerSocPause( client->soc, true ) )
         {
            ERROR("AudioServerSocPause TRUE failed");
         }
      }
   }
   else
   {
      ERROR("msg: pause: no soc");
   }
   
   return msglen;
}

static int audsrv_process_unpause( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: unpause version %d", version);
   
   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_UnPause_Version )
      {
         if ( !AudioServerSocPause( client->soc, false ) )
         {
            ERROR("AudioServerSocPause FALSE failed");
         }
      }
   }
   else
   {
      ERROR("msg: unpause: no soc");
   }
   
   return msglen;
}

static int audsrv_process_flush( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: flush version %d", version);
   
   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_Flush_Version )
      {
         if ( !AudioServerSocFlush( client->soc ) )
         {
            ERROR("AudioServerSocFlush failed");
         }
      }
   }
   else
   {
      ERROR("msg: flush: no soc");
   }
   
   return msglen;
}

static int audsrv_process_audiosync( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: audiosync version %d", version);

   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_AudioSync_Version )
      {
         unsigned len, type;
         long long thenMicros, nowMicros, diff, diff45KHz;
         unsigned long long stc;
         unsigned adjustedStc;
         
         len= audsrv_conn_get_u32( client->conn );
         type= audsrv_conn_get_u32( client->conn );
         
         if ( type != AUDSRV_TYPE_U64 )
         {
            ERROR("expecting type %d (U64) not type %d for audiosync arg 1 (nowMicros)", AUDSRV_TYPE_U64, type );
            goto exit;
         }
         
         thenMicros= (long long)audsrv_conn_get_u64( client->conn );

         
         len= audsrv_conn_get_u32( client->conn );
         type= audsrv_conn_get_u32( client->conn );
         
         if ( type != AUDSRV_TYPE_U64 )
         {
            ERROR("expecting type %d (U64) not type %d for audiosync arg 2 (stc)", AUDSRV_TYPE_U64, type );
            goto exit;
         }
         
         stc= audsrv_conn_get_u64( client->conn );
         
         nowMicros= getCurrentTimeMicro();
         diff= nowMicros-thenMicros;
         
         diff45KHz= ((diff * 45000) / 1000000);
         
         adjustedStc= stc + diff45KHz;

         TRACE1("msg: audiosync stc %lld then %lld now %lld diff %lld diff45 %lld stcadj %u", 
                stc, thenMicros, nowMicros, diff, diff45KHz, adjustedStc);
         
         if ( !AudioServerSocAudioSync( client->soc, adjustedStc ) )
         {
            ERROR("AudioServerSocAudioSync failed");
         }
      }
   }
   else
   {
      ERROR("msg: audiosync: no soc");
   }

exit:
   
   return msglen;
}

static int audsrv_process_audiodata( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE3("msg: audiodata version %d", version);
   
   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_AudioData_Version )
      {
         unsigned len, type, datalen;
         unsigned char *data;
         
         len= audsrv_conn_get_u32( client->conn );
         type= audsrv_conn_get_u32( client->conn );
         
         if ( type != AUDSRV_TYPE_Buffer )
         {
            ERROR("expecting type %d (buffer) not type %d for audiodata arg 1 (data)", AUDSRV_TYPE_Buffer, type );
            goto exit;
         }
         
         audsrv_conn_get_buffer( client->conn, len, &data, &datalen );
         if ( data && datalen )
         {
            if ( !AudioServerSocAudioData( client->soc, data, datalen ) )
            {
               ERROR("AudioServerSocData failed");
            }
            len -= datalen;
         }

         if ( len )
         {
            audsrv_conn_get_buffer( client->conn, len, &data, &datalen );
            if ( data && datalen )
            {
               if ( !AudioServerSocAudioData( client->soc, data, datalen ) )
               {
                  ERROR("AudioServerSocData failed");
               }
            }
         }
      }
   }
   else
   {
      ERROR("msg: audiodata: no soc");
   }

exit:
   
   return msglen;
}

static int audsrv_process_audiodatahandle( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE3("msg: audiodatahandle version %d", version);
   
   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_AudioDataHandle_Version )
      {
         unsigned len, type;
         unsigned long long dataHandle;
         
         len= audsrv_conn_get_u32( client->conn );
         type= audsrv_conn_get_u32( client->conn );
         
         if ( type != AUDSRV_TYPE_U64 )
         {
            ERROR("expecting type %d (U64) not type %d for audiodatahandle arg 1 (dataHandle)", AUDSRV_TYPE_U64, type );
            goto exit;
         }

         dataHandle= audsrv_conn_get_u64( client->conn );

         if ( !AudioServerSocAudioDataHandle( client->soc, dataHandle ) )
         {
            ERROR("AudioServerSocDataHandle failed" );
         }
      }
   }
   else
   {
      ERROR("msg: audiodata: no soc");
   }

exit:
   
   return msglen;
}

static int audsrv_process_mute( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: mute version %d", version);

   if ( version <= AUDSRV_MSG_Mute_Version )
   {
      unsigned len, type;
      unsigned global;
      char name[AUDSRV_MAX_SESSION_NAME_LEN+1];
      char *sessionName= 0;

      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_U16 )
      {
         ERROR("expecting type %d (U16) not type %d for audio mute arg 1 (global)", AUDSRV_TYPE_U16, type );
         goto exit;
      }

      global= audsrv_conn_get_u16( client->conn );


      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_String )
      {
         ERROR("expecting type %d (string) not type %d for audio mute arg 2 (sessionName)", AUDSRV_TYPE_String, type );
         goto exit;
      }
      if ( len > AUDSRV_MAX_SESSION_NAME_LEN )
      {
         ERROR("sessionName too long (%d) for audio mute", len );
         goto exit;
      }
      
      if ( len )
      {
         audsrv_conn_get_string( client->conn, name );
         sessionName= name;
      }

      if ( global )
      {            
         AudsrvContext *ctx= client->ctx;
         if ( !AudioServerSocGlobalMute( ctx->soc, true ) )
         {
            ERROR("AudioServerSocMute TRUE failed");
         }
      }
      else
      {
         if ( sessionName )
         {
            TRACE1("msg: mute sessionName (%s)", sessionName);

            AudsrvContext *ctx= client->ctx;

            pthread_mutex_lock( &ctx->mutex );
            for( std::vector<AudsrvClient*>::iterator it= ctx->clients.begin();
                 it != ctx->clients.end();
                 ++it )
            {
               AudsrvClient *clientIter= (*it);

               pthread_mutex_lock( &clientIter->mutex );
               if ( !strcmp( sessionName, clientIter->sessionName ) )
               {
                  if ( clientIter->soc )
                  {   
                     if ( !AudioServerSocMute( clientIter->soc, true ) )
                     {
                        ERROR("AudioServerSocMute TRUE failed");
                     }
                  }
                  else
                  {
                     ERROR("msg: mute: no soc");
                  }
               }
               pthread_mutex_unlock( &clientIter->mutex );
            }
            pthread_mutex_unlock( &ctx->mutex );
         }
         else
         {
            if ( client->soc )
            {   
               if ( !AudioServerSocMute( client->soc, true ) )
               {
                  ERROR("AudioServerSocMute TRUE failed");
               }
            }
            else
            {
               ERROR("msg: mute: no soc");
            }
         }
      }
   }

exit:   

   return msglen;
}

static int audsrv_process_unmute( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: unmute version %d", version);

   if ( version <= AUDSRV_MSG_UnMute_Version )
   {
      unsigned len, type;
      unsigned global;
      char name[AUDSRV_MAX_SESSION_NAME_LEN+1];
      char *sessionName= 0;

      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_U16 )
      {
         ERROR("expecting type %d (U16) not type %d for audio unmute arg 1 (global)", AUDSRV_TYPE_U16, type );
         goto exit;
      }

      global= audsrv_conn_get_u16( client->conn );


      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_String )
      {
         ERROR("expecting type %d (string) not type %d for audio unmute arg 2 (sessionName)", AUDSRV_TYPE_String, type );
         goto exit;
      }
      if ( len > AUDSRV_MAX_SESSION_NAME_LEN )
      {
         ERROR("sessionName too long (%d) for audio unmute", len );
         goto exit;
      }
      
      if ( len )
      {
         audsrv_conn_get_string( client->conn, name );
         sessionName= name;
      }

      if ( global )
      {            
         AudsrvContext *ctx= client->ctx;
         if ( !AudioServerSocGlobalMute( ctx->soc, false ) )
         {
            ERROR("AudioServerSocGlobalMute FALSE failed");
         }
      }
      else
      {
         if ( sessionName )
         {
            TRACE1("msg: unmute sessionName (%s)", sessionName);

            AudsrvContext *ctx= client->ctx;

            pthread_mutex_lock( &ctx->mutex );
            for( std::vector<AudsrvClient*>::iterator it= ctx->clients.begin();
                 it != ctx->clients.end();
                 ++it )
            {
               AudsrvClient *clientIter= (*it);

               pthread_mutex_lock( &clientIter->mutex );
               if ( !strcmp( sessionName, clientIter->sessionName ) )
               {
                  if ( clientIter->soc )
                  {   
                     if ( !AudioServerSocMute( clientIter->soc, false ) )
                     {
                        ERROR("AudioServerSocMute FALSE failed");
                     }
                  }
                  else
                  {
                     ERROR("msg: unmute: no soc");
                  }
               }
               pthread_mutex_unlock( &clientIter->mutex );
            }
            pthread_mutex_unlock( &ctx->mutex );
         }
         else
         {
            if ( client->soc )
            {   
               if ( !AudioServerSocMute( client->soc, false ) )
               {
                  ERROR("AudioServerSocMute FALSE failed");
               }
            }
            else
            {
               ERROR("msg: unmute: no soc");
            }
         }
      }
   }

exit:   

   return msglen;
}

static int audsrv_process_volume( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: volume version %d", version);

   if ( version <= AUDSRV_MSG_Volume_Version )
   {
      unsigned len, type;
      unsigned numerator, denominator;
      float volume= 1.0;
      unsigned global;
      char name[AUDSRV_MAX_SESSION_NAME_LEN+1];
      char *sessionName= 0;
      
      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_U32 )
      {
         ERROR("expecting type %d (U32) not type %d for volume arg 1 (numerator)", AUDSRV_TYPE_U32, type );
         goto exit;
      }
      
      numerator= audsrv_conn_get_u32( client->conn );

      
      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_U32 )
      {
         ERROR("expecting type %d (U32) not type %d for volume arg 2 (denominator)", AUDSRV_TYPE_U32, type );
         goto exit;
      }
      
      denominator= audsrv_conn_get_u32( client->conn );
      
      if ( denominator == 0 )
      {
         ERROR("volume denominator is 0 - setting volume to 1.0");            
      }
      else
      {
         volume= (float)numerator/(float)denominator;
         TRACE1("msg: volume (%u/%u) = %f", numerator, denominator, volume );
      }

      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_U16 )
      {
         ERROR("expecting type %d (U16) not type %d for audio volume arg 3 (global)", AUDSRV_TYPE_U16, type );
         goto exit;
      }

      global= audsrv_conn_get_u16( client->conn );


      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_String )
      {
         ERROR("expecting type %d (string) not type %d for audio volume arg 4 (sessionName)", AUDSRV_TYPE_String, type );
         goto exit;
      }
      if ( len > AUDSRV_MAX_SESSION_NAME_LEN )
      {
         ERROR("sessionName too long (%d) for audio volume", len );
         goto exit;
      }
      
      if ( len )
      {
         audsrv_conn_get_string( client->conn, name );
         sessionName= name;
      }

      if ( global )
      {
         AudsrvContext *ctx= client->ctx;
         AudioServerSocGlobalVolume( ctx->soc, volume );
      }
      else
      {
         if ( sessionName )
         {
            TRACE1("msg: volume sessionName (%s)", sessionName);

            AudsrvContext *ctx= client->ctx;

            pthread_mutex_lock( &ctx->mutex );
            for( std::vector<AudsrvClient*>::iterator it= ctx->clients.begin();
                 it != ctx->clients.end();
                 ++it )
            {
               AudsrvClient *clientIter= (*it);

               pthread_mutex_lock( &clientIter->mutex );
               if ( !strcmp( sessionName, clientIter->sessionName ) )
               {
                  if ( clientIter->soc )
                  {   
                     if ( !AudioServerSocVolume( clientIter->soc, volume ) )
                     {
                        ERROR("AudioServerSocVolume failed");
                     }
                  }
                  else
                  {
                     ERROR("msg: volume: no soc");
                  }
               }
               pthread_mutex_unlock( &clientIter->mutex );
            }
            pthread_mutex_unlock( &ctx->mutex );
         }
         else
         {
            if ( client->soc )
            {   
               if ( !AudioServerSocVolume( client->soc, volume ) )
               {
                  ERROR("AudioServerSocVolume failed");
               }
            }
            else
            {
               ERROR("msg: volume: no soc");
            }
         }
      }
   }

exit:
   
   return msglen;
}

static int audsrv_process_enable_session_event( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: enableSessionEvent version %d", version);

   if ( version <= AUDSRV_MSG_EnableSessionEvent_Version )
   {
      if ( client )
      {
         AudsrvContext *ctx= client->ctx;

         if ( ctx )
         {
            pthread_mutex_lock( &ctx->mutex );

            ctx->clientsSessionEvent.push_back( client );

            pthread_mutex_unlock( &ctx->mutex );
         }
      }
   }

   return msglen;
}

static int audsrv_process_disable_session_event( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: disableSessionEvent version %d", version);

   if ( version <= AUDSRV_MSG_DisableSessionEvent_Version )
   {
      if ( client )
      {
         AudsrvContext *ctx= client->ctx;

         if ( ctx )
         {
            pthread_mutex_lock( &ctx->mutex );

            for( std::vector<AudsrvClient*>::iterator it= ctx->clientsSessionEvent.begin();
                 it != ctx->clientsSessionEvent.end();
                 ++it )
            {
               if ( client == (*it) )
               {
                  ctx->clientsSessionEvent.erase( it );
                  break;
               }
            }

            pthread_mutex_unlock( &ctx->mutex );
         }
      }
   }

   return msglen;
}

static int audsrv_process_enableeos( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: enableeos version %d", version);

   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_EnableEOS_Version )
      {
         AudioServerSocEnableEOSDetection( client->soc, audsrv_eos_callback, client );
      }
   }
   else
   {
      ERROR("msg: enableeos: no soc");
   }

   return msglen;
}

static int audsrv_process_disableeos( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: disableeos version %d", version);

   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_DisableEOS_Version )
      {
         AudioServerSocDisableEOSDetection( client->soc );
      }
   }
   else
   {
      ERROR("msg: disableeos: no soc");
   }

   return msglen;
}

static int audsrv_process_startcapture( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: startcapture version %d", version);

   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_StartCapture_Version )
      {
         unsigned len, type, value;
         char name[AUDSRV_MAX_SESSION_NAME_LEN+1];
         char *sessionName= 0;

         len= audsrv_conn_get_u32( client->conn );
         type= audsrv_conn_get_u32( client->conn );
         
         if ( type != AUDSRV_TYPE_String )
         {
            ERROR("expecting type %d (string) not type %d for init arg 1 (sessionName)", AUDSRV_TYPE_String, type );
            goto exit;
         }
         if ( len > AUDSRV_MAX_SESSION_NAME_LEN )
         {
            ERROR("sessionName too long (%d) for init", len );
            goto exit;
         }
         
         if ( len )
         {
            audsrv_conn_get_string( client->conn, name );
            sessionName= name;
         }
         INFO("capture sessionName (%s)", sessionName);

         AudioServerSocSetCaptureCallback( client->soc, sessionName, audsrv_capture_callback, client );
      }
   }
   else
   {
      ERROR("msg: startcapture: no soc");
   }

exit:

   return msglen;
}

static int audsrv_process_stopcapture( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: stopcapture version %d", version);

   if ( client->soc )
   {   
      if ( version <= AUDSRV_MSG_StartCapture_Version )
      {
         AudioServerSocSetCaptureCallback( client->soc, NULL, NULL, 0 );
         
         audsrv_send_capture_done( client );
      }
   }
   else
   {
      ERROR("msg: stopcapture: no soc");
   }

   return msglen;
}

static int audsrv_process_enumsessions( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: enumsessions version %d", version);

   if ( version <= AUDSRV_MSG_EnumSessions_Version )
   {
      unsigned long long token;
      int sessionCount= 0;
      int infoSize= 0;
      int namelen;
      const char *name;
      unsigned char *enumInfo= 0;
      unsigned len, type;

      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_U64 )
      {
         ERROR("expecting type %d (U64) not type %d for enum sessions arg 1 (token)", AUDSRV_TYPE_U64, type );
         goto exit;
      }
      
      token= audsrv_conn_get_u64( client->conn );


      AudsrvContext *ctx= client->ctx;
      pthread_mutex_lock( &ctx->mutex );
      sessionCount= 0;
      for( std::vector<AudsrvClient*>::iterator it= ctx->clients.begin();
           it != ctx->clients.end();
           ++it )
      {
         AudsrvClient *clientIter= (*it);

         pthread_mutex_lock( &clientIter->mutex );
         if ( clientIter->sessionType != AUDSRV_SESSION_Observer )
         {
            ++sessionCount;
            infoSize += (AUDSRV_MSG_TYPE_HDR_LEN+AUDSRV_MSG_U32_LEN); //client pid
            infoSize += (AUDSRV_MSG_TYPE_HDR_LEN+AUDSRV_MSG_U16_LEN); //client session type
            namelen= strlen(clientIter->sessionName);
            name= namelen ? clientIter->sessionName : "no-name";
            infoSize += AUDSRV_MSG_TYPE_HDR_LEN+AUDSRV_MSG_STRING_LEN(name);
         }
         pthread_mutex_unlock( &clientIter->mutex );
      }

      enumInfo= (unsigned char *)malloc( infoSize );
      if ( enumInfo )
      {
         unsigned char *p= enumInfo;
         for( std::vector<AudsrvClient*>::iterator it= ctx->clients.begin();
              it != ctx->clients.end();
              ++it )
         {
            AudsrvClient *clientIter= (*it);

            pthread_mutex_lock( &clientIter->mutex );
            if ( clientIter->sessionType != AUDSRV_SESSION_Observer )
            {
               TRACE1("client pid %d type %d name(%s)", clientIter->ucred.pid, clientIter->sessionType, name );
               p += audsrv_conn_put_u32( p, AUDSRV_MSG_U32_LEN );
               p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
               p += audsrv_conn_put_u32( p, clientIter->ucred.pid );
               p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
               p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
               p += audsrv_conn_put_u16( p, clientIter->sessionType );
               namelen= strlen(clientIter->sessionName);
               name= namelen ? clientIter->sessionName : "no-name";
               p += audsrv_conn_put_u32( p, AUDSRV_MSG_STRING_LEN(name) );
               p += audsrv_conn_put_u32( p, AUDSRV_TYPE_String );
               p += audsrv_conn_put_string( p, name );
            }
            pthread_mutex_unlock( &clientIter->mutex );
         }
      }
      else
      {
         ERROR("No memory for enum session response");
      }

      audsrv_send_enum_session_results( client, token, sessionCount, enumInfo, infoSize );

      if ( enumInfo )
      {
         free( enumInfo );
      }

      pthread_mutex_unlock( &ctx->mutex );
   }

exit:
   return msglen;
}

static int audsrv_process_getstatus( AudsrvClient *client, unsigned msglen, unsigned version )
{
   TRACE1("msg: getstatus version %d", version);

   if ( version <= AUDSRV_MSG_GetStatus_Version )
   {
      unsigned long long token;
      unsigned len, type;
      char name[AUDSRV_MAX_SESSION_NAME_LEN+1];
      char *sessionName= 0;
      bool haveStatus= false;
      AudSrvSessionStatus status, *pStatus= 0;
      AudsrvContext *ctx= client->ctx;

      pStatus= &status;
      memset( &status, 0, sizeof(status) );

      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_U64 )
      {
         ERROR("expecting type %d (U64) not type %d for getstatus arg 1 (token)", AUDSRV_TYPE_U64, type );
         goto exit;
      }
      
      token= audsrv_conn_get_u64( client->conn );

      len= audsrv_conn_get_u32( client->conn );
      type= audsrv_conn_get_u32( client->conn );
      
      if ( type != AUDSRV_TYPE_String )
      {
         ERROR("expecting type %d (string) not type %d for getstatus arg 2 (sessionName)", AUDSRV_TYPE_String, type );
         goto exit;
      }
      if ( len > AUDSRV_MAX_SESSION_NAME_LEN )
      {
         ERROR("sessionName too long (%d) for audio getstatus", len );
         goto exit;
      }
      
      if ( len )
      {
         audsrv_conn_get_string( client->conn, name );
         sessionName= name;
      }

      if ( sessionName )
      {
         TRACE1("msg: getstatus sessionName (%s)", sessionName);

         pthread_mutex_lock( &ctx->mutex );
         for( std::vector<AudsrvClient*>::iterator it= ctx->clients.begin();
              it != ctx->clients.end();
              ++it )
         {
            AudsrvClient *clientIter= (*it);

            pthread_mutex_lock( &clientIter->mutex );
            if ( !strcmp( sessionName, clientIter->sessionName ) )
            {
               if ( clientIter->soc )
               {
                  if ( AudioServerSocGetStatus( ctx->soc, clientIter->soc, &status ) )
                  {
                     status.ready= true;
                  }
                  haveStatus= true;
               }
               else
               {
                  ERROR("msg: getstatus: no soc");
               }
            }
            pthread_mutex_unlock( &clientIter->mutex );
         }
         pthread_mutex_unlock( &ctx->mutex );
      }
      else
      {
         if ( client->soc )
         {   
            if ( AudioServerSocGetStatus( ctx->soc, client->soc, &status ) )
            {
               status.ready= true;
            }
            haveStatus= true;
         }
         else
         {
            if ( !AudioServerSocGetStatus( ctx->soc, 0, &status ) )
            {
               ERROR("msg: getstatus: failed to get global status");
            }
         }
      }

      if ( haveStatus )
      {
         if ( sessionName )
         {
            strcpy( status.sessionName, sessionName );
         }
         else
         {
            strcpy( status.sessionName, client->sessionName );
         }
      }

      audsrv_send_getstatus_results( client, token, pStatus );
   }

exit:   

   return msglen;
}

static void audsrv_eos_callback( void *userData )
{
   AudsrvClient *client= (AudsrvClient*)userData;
   
   if ( client )
   {
      TRACE1("audsrv_eos_callback: client %p", client);
      audsrv_send_eos_detected( client );
   }
}

static void audsrv_first_audio_callback( void *userData )
{
   AudsrvClient *client= (AudsrvClient*)userData;
   
   if ( client )
   {
      TRACE1("audsrv_first_audio_callback: client %p", client);
      audsrv_send_first_audio( client );
   }
}

static void audsrv_pts_error_callback( void *userData, unsigned count )
{
   AudsrvClient *client= (AudsrvClient*)userData;
   
   if ( client )
   {
      TRACE1("audsrv_pts_error_callback: client %p count %u", client, count);
      audsrv_send_pts_error( client, count );
   }
}

static void audsrv_underflow_callback( void *userData, unsigned count, unsigned bufferedBytes, unsigned queuedFrames )
{
   AudsrvClient *client= (AudsrvClient*)userData;
   
   if ( client )
   {
      TRACE1("audsrv_underflow_callback: client %p count %u bufferedBytes %u queuedFrames %u", 
              client, count, bufferedBytes, queuedFrames);
      audsrv_send_underflow( client, count, bufferedBytes, queuedFrames );
   }
}

static void audsrv_capture_callback( void *userData, AudSrvCaptureParameters *params, unsigned char *data, int datalen )
{
   AudsrvClient *client= (AudsrvClient*)userData;
   
   if ( client )
   {
      TRACE1("audsrv_capture_callback: client %p data %p datalen %u", client, data, datalen);
      
      if ( params->version != client->captureParams.version )
      {
         client->captureParams= *params;
         audsrv_send_capture_params( client );
      }

      while( datalen > 0 )
      {
         int sendlen= datalen;
         if (sendlen > AUDSRV_MAX_CAPTURE_DATA_SIZE)
         {
            sendlen= AUDSRV_MAX_CAPTURE_DATA_SIZE;
         }
         audsrv_send_capture_data( client, data, sendlen );
         data += sendlen;
         datalen -= sendlen;
      }
   }
}

static void audsrv_distribute_session_event( AudsrvContext *ctx, int event, AudsrvClient *clientSubject )
{
   TRACE1("audsrv_distribute_session_event: event %d clientSubject %p", clientSubject );

   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutex );

      for( std::vector<AudsrvClient*>::iterator it= ctx->clientsSessionEvent.begin();
           it != ctx->clientsSessionEvent.end();
           ++it )
      {
         AudsrvClient *clientIter= (*it);

         audsrv_send_session_event( clientIter, event, clientSubject );
      }

      pthread_mutex_unlock( &ctx->mutex );
   }
}

static void audsrv_send_session_event( AudsrvClient *client, int event, AudsrvClient *clientSubject )
{
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen, namelen;
   int sendLen;
   const char *name;
   
   TRACE1("audsrv_send_session_event: client %p", client );
   
   if ( client )
   {
      pthread_mutex_lock( &client->mutex );

      p= client->conn->sendbuff;
      paramLen= 0;

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN+AUDSRV_MSG_U16_LEN); //event
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN+AUDSRV_MSG_U32_LEN); //client pid
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN+AUDSRV_MSG_U16_LEN); //client session type
      namelen= strlen(clientSubject->sessionName);
      name= namelen ? clientSubject->sessionName : "no-name";
      paramLen += AUDSRV_MSG_TYPE_HDR_LEN+AUDSRV_MSG_STRING_LEN(name);

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("session event msg too large");
         pthread_mutex_unlock( &client->mutex );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_SessionEvent );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_SessionEvent_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u32( p, event );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U32_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, clientSubject->ucred.pid );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, clientSubject->sessionType );
      namelen= strlen(clientSubject->sessionName);
      name= namelen ? clientSubject->sessionName : "no-name";
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_STRING_LEN(name) );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_String );
      p += audsrv_conn_put_string( p, name );
      
      sendLen= audsrv_conn_send( client->conn, client->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &client->mutex );
   }   

exit:
   TRACE1("audsrv_send_session_event: client %p result %d", client, result );

}

static bool audsrv_send_eos_detected( AudsrvClient *client )
{
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("audsrv_send_eos_detected: client %p", client );
   
   if ( client )
   {
      pthread_mutex_lock( &client->mutex );

      p= client->conn->sendbuff;
      paramLen= 0;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("eos detected msg too large");
         pthread_mutex_unlock( &client->mutex );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_EOSDetected );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_EOSDetected_Version );

      sendLen= audsrv_conn_send( client->conn, client->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &client->mutex );
   }   

exit:
   TRACE1("audsrv_send_eos_detected: client %p result %d", client, result );

   return result;
}

static bool audsrv_send_first_audio( AudsrvClient *client )
{
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("audsrv_send_first_audio: client %p", client );
   
   if ( client )
   {
      pthread_mutex_lock( &client->mutex );

      p= client->conn->sendbuff;
      paramLen= 0;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("first audio msg too large");
         pthread_mutex_unlock( &client->mutex );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_FirstAudio );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_FirstAudio_Version );

      sendLen= audsrv_conn_send( client->conn, client->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &client->mutex );
   }   

exit:
   TRACE1("audsrv_send_first_audio: client %p result %d", client, result );

   return result;
}

static bool audsrv_send_pts_error( AudsrvClient *client, unsigned count )
{
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("audsrv_send_pts_error: client %p count %u", client, count );
   
   if ( client )
   {
      pthread_mutex_lock( &client->mutex );

      p= client->conn->sendbuff;
      paramLen= 0;

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // count

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("pts error msg too large");
         pthread_mutex_unlock( &client->mutex );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_PtsError );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_PtsError_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U32_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, count );

      sendLen= audsrv_conn_send( client->conn, client->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &client->mutex );
   }   

exit:
   TRACE1("audsrv_send_pts_error: client %p result %d", client, result );

   return result;
}

static bool audsrv_send_underflow( AudsrvClient *client, unsigned count, unsigned bufferedBytes, unsigned queuedFrames )
{
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("audsrv_send_underflow: client %p count %u bufferedBytes %u queuedFrames %u", client, count, bufferedBytes, queuedFrames );
   
   if ( client )
   {
      pthread_mutex_lock( &client->mutex );

      p= client->conn->sendbuff;
      paramLen= 0;

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // count
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // bufferedBytes
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // queuedFrames

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("underflow msg too large");
         pthread_mutex_unlock( &client->mutex );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Underflow );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Underflow_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U32_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, count );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U32_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, bufferedBytes );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U32_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, queuedFrames );

      sendLen= audsrv_conn_send( client->conn, client->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &client->mutex );
   }   

exit:
   TRACE1("audsrv_send_underflow: client %p result %d", client, result );

   return result;
}

static bool audsrv_send_capture_params( AudsrvClient *client )
{
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("audsrv_send_capture_params: client %p version %u numchan %u bits %u rate %u", 
          client, client->captureParams.version, client->captureParams.numChannels,
          client->captureParams.bitsPerSample, client->captureParams.sampleRate );
   
   if ( client )
   {
      pthread_mutex_lock( &client->mutex );

      p= client->conn->sendbuff;
      paramLen= 0;

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // version
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // numChannels
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // bitsPerSample
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // sampleRate
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // outputDelay

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("captureParameters msg too large");
         pthread_mutex_unlock( &client->mutex );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_CaptureParameters );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_CaptureParameters_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U32_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, client->captureParams.version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, client->captureParams.numChannels );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, client->captureParams.bitsPerSample );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U32_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, client->captureParams.sampleRate );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U32_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, client->captureParams.outputDelay );

      sendLen= audsrv_conn_send( client->conn, client->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &client->mutex );
   }   

exit:
   TRACE1("audsrv_send_capture_params: client %p result %d", client, result );

   return result;
}

static bool audsrv_send_capture_data( AudsrvClient *client, unsigned char *data, int datalen )
{
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("audsrv_send_capture_data: client %p data %p len %d", client, data, datalen ); 
   
   if ( client )
   {
      pthread_mutex_lock( &client->mutex );

      p= client->conn->sendbuff;
      paramLen= 0;

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + datalen); // buffer

      // Don't include payload data length since it doesn't occupy space
      // in our work buffer
      msgLen= AUDSRV_MSG_HDR_LEN + paramLen - datalen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("captureData msg too large");
         pthread_mutex_unlock( &client->mutex );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_CaptureData );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_CaptureData_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_BUFFER_LEN(datalen) );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_Buffer );

      sendLen= audsrv_conn_send( client->conn, client->conn->sendbuff, msgLen, data, datalen );
      
      result= (sendLen == msgLen+datalen);

      pthread_mutex_unlock( &client->mutex );
   }   

exit:
   TRACE1("audsrv_send_capture_data: client %p result %d", client, result );

   return result;
}

static bool audsrv_send_capture_done( AudsrvClient *client )
{
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("audsrv_send_capture_done: client %p", client );
   
   if ( client )
   {
      pthread_mutex_lock( &client->mutex );

      p= client->conn->sendbuff;
      paramLen= 0;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("capture done msg too large");
         pthread_mutex_unlock( &client->mutex );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_CaptureDone );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_CaptureDone_Version );

      sendLen= audsrv_conn_send( client->conn, client->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &client->mutex );
   }   

exit:
   TRACE1("audsrv_send_capture_done: client %p result %d", client, result );

   return result;
}

static bool audsrv_send_enum_session_results( AudsrvClient *client, unsigned long long token, int sessionCount, unsigned char *data, int datalen )
{
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   int enumResult;
   
   TRACE1("audsrv_send_enum_session_results: client %p token:%llx data %p len %d", client, token, data, datalen ); 
   
   if ( client )
   {
      enumResult= (data && datalen) ? 0 : 1;

      pthread_mutex_lock( &client->mutex );

      p= client->conn->sendbuff;
      paramLen= 0;

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U64_LEN); // token
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // enumResult
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // sessionCount
      paramLen += datalen;  // parameter set comprising enum results

      // Don't include payload data length since it doesn't occupy space
      // in our work buffer
      msgLen= AUDSRV_MSG_HDR_LEN + paramLen - datalen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("enum session results msg too large");
         pthread_mutex_unlock( &client->mutex );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_EnumSessionsResults );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_EnumSessionsResults_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U64_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U64 );
      p += audsrv_conn_put_u64( p, token );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, enumResult );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, sessionCount );

      sendLen= audsrv_conn_send( client->conn, client->conn->sendbuff, msgLen, data, datalen );
      
      result= (sendLen == msgLen+datalen);

      pthread_mutex_unlock( &client->mutex );
   }   

exit:
   TRACE1("audsrv_send_enum_session_results: client %p result %d", client, result );

   return result;
}

static bool audsrv_send_getstatus_results( AudsrvClient *client, unsigned long long token, AudSrvSessionStatus *status )
{
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen, nameLen= 0;
   int sendLen;
   int getStatusResult;

   TRACE1("audsrv_send_getstatus_results: client %p", client ); 
   
   if ( client )
   {
      getStatusResult= (status->ready ? 0 : 1);
      pthread_mutex_lock( &client->mutex );

      p= client->conn->sendbuff;
      paramLen= 0;

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U64_LEN); // token
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // getStatusResult
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // global muted
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // global volume num
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // global volume denom
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // ready
      if ( getStatusResult == 0 )
      {
         nameLen= AUDSRV_MSG_STRING_LEN(status->sessionName);
         paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // playing
         paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // muted
         paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // volume num
         paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // volume denom
         paramLen += AUDSRV_MSG_TYPE_HDR_LEN+nameLen;
      }

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("getstatus results msg too large");
         pthread_mutex_unlock( &client->mutex );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_GetStatusResults );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_GetStatusResults_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U64_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U64 );
      p += audsrv_conn_put_u64( p, token );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, getStatusResult );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, status->globalMuted );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, status->globalVolume*LEVEL_DENOMINATOR );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, LEVEL_DENOMINATOR );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, status->ready );
      if ( status )
      {
         p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
         p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
         p += audsrv_conn_put_u16( p, status->playing );
         p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
         p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
         p += audsrv_conn_put_u16( p, status->muted );
         p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
         p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
         p += audsrv_conn_put_u32( p, status->volume*LEVEL_DENOMINATOR );
         p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
         p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
         p += audsrv_conn_put_u32( p, LEVEL_DENOMINATOR );
         p += audsrv_conn_put_u32( p, nameLen );
         p += audsrv_conn_put_u32( p, AUDSRV_TYPE_String );
         if ( nameLen )
         {
            p += audsrv_conn_put_string( p, status->sessionName );
         }
      }

      sendLen= audsrv_conn_send( client->conn, client->conn->sendbuff, msgLen, NULL, 0 );

      result= (sendLen == msgLen);

      pthread_mutex_unlock( &client->mutex );
   }   

exit:
   TRACE1("audsrv_send_getstatus_results: client %p result %d", client, result );

   return result;
}

/** @} */
/** @} */

