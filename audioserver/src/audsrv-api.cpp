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
* @defgroup audsrv-api
* @{
**/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <vector>

#include "audioserver.h"
#include "audioserver-soc.h"
#include "audsrv-logger.h"
#include "audsrv-protocol.h"
#include "audsrv-conn.h"

#define LEVEL_DENOMINATOR (1000000)

typedef struct _AudsrvCBCtx
{
   union
   {
      AudioServerEnumSessions enumsess;
      AudioServerSessionStatus getstatus;
   } cb;
   void *userData;
} AudsrvCBCtx;

#define AUDSRV_MAX_MSG (1024)
#define AUDSRV_RCVBUFFSIZE (80*1024)
typedef struct _AudsrvApiContext
{
   char *serverName;
   struct sockaddr_un addr;
   int fdSocket;
   AudsrvConn *conn;
   pthread_mutex_t mutexSend;
   pthread_t threadId;
   pthread_mutex_t mutexRecv;
   bool receiveThreadStopRequested;
   bool receiveThreadStarted;
   bool receiveThreadReady;
   AudSrvCaptureParameters captureParameters;

   std::vector<AudsrvCBCtx*> pendingCallbacks;

   bool inCallback;
   bool discPending;

   bool isPrivate;
   unsigned sessionType;
   char *sessionNameAttached;
   char *sessionNamePrivate;

   AudioServerSessionEvent sessionEventCB;
   void *sessionEventUserData;
   AudioServerFirstAudio firstAudioCB;
   void *firstAudioUserData;
   AudioServerPTSError ptsErrorCB;
   void *ptsErrorUserData;
   AudioServerUnderflow underflowCB;
   void *underflowUserData;
   AudioServerEOS eosCB;
   void *eosUserData;
   AudioServerCapture captureCB;
   void *captureUserData;
   AudioServerCaptureDone captureDoneCB;
   void *captureDoneUserData;
} AudsrvApiContext;

static bool audsrv_connect_socket( AudsrvApiContext *ctx );
static void* audsrv_receive_thread( void *arg );
static int audsrv_process_message( AudsrvApiContext *ctx );
static int audsrv_process_session_event( AudsrvApiContext *ctx, unsigned msglen, unsigned version );
static int audsrv_process_eosdetected( AudsrvApiContext *ctx, unsigned msglen, unsigned version );
static int audsrv_process_firstaudio( AudsrvApiContext *ctx, unsigned msglen, unsigned version );
static int audsrv_process_ptserror( AudsrvApiContext *ctx, unsigned msglen, unsigned version );
static int audsrv_process_underflow( AudsrvApiContext *ctx, unsigned msglen, unsigned version );
static int audsrv_process_capture_parameters( AudsrvApiContext *ctx, unsigned msglen, unsigned version );
static int audsrv_process_capture_data( AudsrvApiContext *ctx, unsigned msglen, unsigned version );
static int audsrv_process_capture_done( AudsrvApiContext *ctx, unsigned msglen, unsigned version );
static int audsrv_process_enum_sessions_results( AudsrvApiContext *ctx, unsigned msglen, unsigned version );
static int audsrv_process_getstatus_results( AudsrvApiContext *ctx, unsigned msglen, unsigned version );

bool AudioServerInit( void )
{
   bool result;

   result= AudioServerSocInit();

   return result;
}

void AudioServerTerm( void )
{
   AudioServerSocTerm();
}

AudSrv AudioServerConnect( const char *name )
{
   bool error= false;
   AudsrvApiContext *ctx= 0;
   int rc;
   
   char *env= getenv( "AUDSRV_DEBUG" );
   if ( env )
   {
      int level= atoi( env );
      audsrv_set_log_level( level );
   }
   
   TRACE1( "AudioServerConnect: enter" );
   
   if ( !name )
   {
      name= "audsrv0";
   }
   
   ctx= (AudsrvApiContext*)calloc( 1, sizeof(AudsrvApiContext) );
   if ( !ctx )
   {
      ERROR("Unable to allocate api context");
      error= true;
      goto exit;
   }
   
   ctx->fdSocket= -1;
   ctx->captureParameters.version= (unsigned)-1;
   ctx->pendingCallbacks= std::vector<AudsrvCBCtx*>();
   
   ctx->serverName= strdup( name );
   if ( !ctx->serverName )
   {
      ERROR("Unable to allocate server name");
      error= true;
      goto exit;
   }
   
   pthread_mutex_init( &ctx->mutexSend, 0 );
   pthread_mutex_init( &ctx->mutexRecv, 0 );
   
   if ( !audsrv_connect_socket( ctx ) )
   {
      ERROR("Unable to connect socket");
      error= true;
      goto exit;
   }

   ctx->conn= audsrv_conn_init( ctx->fdSocket, AUDSRV_MAX_MSG, AUDSRV_RCVBUFFSIZE );
   if ( !ctx->conn )
   {
      ERROR("Error initilaizing connection" );
      error= true;
      goto exit;
   }
   
   rc= pthread_create( &ctx->threadId, NULL, audsrv_receive_thread, ctx );
   if ( !rc )
   {
      bool ready;
      
      for( int i= 0; i < 500; ++i )
      {
         pthread_mutex_lock( &ctx->mutexRecv );
         ready= ctx->receiveThreadReady;
         pthread_mutex_unlock( &ctx->mutexRecv );

         if ( ready )
         {
            TRACE1("ctx %p receive thread ready", ctx);
            break;
         }

         usleep( 10000 );
      }
      
      if ( !ready )
      {
         error= true;
         ERROR("client thread failed to become ready");
      }         
   }
   else
   {
      ERROR("error creating thread for ctx %p fd %d", ctx, ctx->fdSocket );
   }

exit:

   if ( error )
   {
      if ( ctx )
      {
         if ( ctx->conn )
         {
            audsrv_conn_term( ctx->conn );
            ctx->conn= 0;
         }
         if ( ctx->serverName )
         {
            free( ctx->serverName );
            ctx->serverName= 0;
         }
         free( ctx );
         ctx= 0;
      }
   }

   TRACE1( "AudioServerConnect: exit : audsrv %p", ctx );

   return (AudSrv)ctx;   
}

void AudioServerDisconnect( AudSrv audsrv )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   
   TRACE1( "AudioServerDisconnect: enter: audsrv %p", audsrv );

   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexRecv );

      if ( ctx->inCallback )
      {
         ctx->discPending= true;
         goto exit;
      }

      if ( ctx->fdSocket >= 0 )
      {
         TRACE1("AudioServerDisconnect: shutting down socket fd %d for ctx %p", ctx->fdSocket, ctx );
         shutdown( ctx->fdSocket, SHUT_RDWR );
      }

      if ( ctx->receiveThreadStarted )
      {
         ctx->receiveThreadStopRequested= true;
         pthread_mutex_unlock( &ctx->mutexRecv );
         TRACE1("AudioServerDisconnect: requested stop, calling join for ctx %p", ctx);
         pthread_join( ctx->threadId, NULL );
         TRACE1("AudioServerDisconnect: join complete for ctx %p", ctx);
         pthread_mutex_lock( &ctx->mutexRecv );
      }

      while( ctx->pendingCallbacks.size() > 0 )
      {
         AudsrvCBCtx *pCBCtx= ctx->pendingCallbacks.back();
         free( pCBCtx );
         ctx->pendingCallbacks.pop_back();
      }

      if ( ctx->conn )
      {
         audsrv_conn_term( ctx->conn );
         ctx->conn= 0;
      }
      
      if ( ctx->fdSocket >= 0 )
      {
         close( ctx->fdSocket );
         ctx->fdSocket= -1;
      }

      if ( ctx->sessionNamePrivate ) {
         free( ctx->sessionNamePrivate );
         ctx->sessionNamePrivate = 0;
      }
      
      if ( ctx->serverName )
      {
         free( ctx->serverName );
         ctx->serverName= 0;
      }

      pthread_mutex_unlock( &ctx->mutexRecv );
      pthread_mutex_destroy( &ctx->mutexRecv );
      pthread_mutex_destroy( &ctx->mutexSend );
      
      free( ctx );
   }

exit:

   TRACE1( "AudioServerDisconnect: exit" );
}

bool AudioServerInitSession( AudSrv audsrv, int sessionType, bool isPrivate, const char *sessionName )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen, nameLen;
   int sendLen;
   
   TRACE1("AudioServerInitSession: audsrv %p", audsrv );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;

      nameLen= (sessionName ? strlen(sessionName) : 0);
      if ( nameLen > AUDSRV_MAX_SESSION_NAME_LEN )
      {
         ERROR("audio session name len too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }
      nameLen= (sessionName ? AUDSRV_MSG_STRING_LEN(sessionName) : 0);
      
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // sessionType
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // isPriate
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + nameLen); // sessionName

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("init msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Init );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Init_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, sessionType );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, isPrivate );
      p += audsrv_conn_put_u32( p, nameLen );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_String );
      if ( sessionName )
      {
         p += audsrv_conn_put_string( p, sessionName );
      }

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      if ( result )
      {
         ctx->isPrivate= isPrivate;
         ctx->sessionType= sessionType;

         if ( ctx->isPrivate == true ) {
             if ( !ctx->sessionNamePrivate || strcmp( ctx->sessionNamePrivate, sessionName ) ) {
                if ( ctx->sessionNamePrivate ) {
                   free( ctx->sessionNamePrivate );
                   ctx->sessionNamePrivate = 0;
                }

                ctx->sessionNamePrivate = strdup( sessionName );
             }
         }
      }

      pthread_mutex_unlock( &ctx->mutexSend );
   }

exit:
   TRACE1("AudioServerInitSession: audsrv %p result %d", audsrv, result );
   
   return result;
}

bool AudioServerGetSessionType( AudSrv audsrv, int *sessionType )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;

   if ( ctx && sessionType )
   {
      *sessionType= ctx->sessionType;
      result= true;
   }

   return result;
}

bool AudioServerGetSessionIsPrivate( AudSrv audsrv, bool *sessionIsPrivate )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;

   if ( ctx && sessionIsPrivate )
   {
      *sessionIsPrivate= ctx->isPrivate;
      result= true;
   }

   return result;
}

bool AudioServerSessionAttach( AudSrv audsrv, const char *sessionName )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;

   if ( ctx )
   {
      if ( ctx->sessionType == AUDSRV_SESSION_Observer )
      {
         if ( !ctx->sessionNameAttached || strcmp( ctx->sessionNameAttached, sessionName ) )
         {
            if ( ctx->sessionNameAttached )
            {         
               free( ctx->sessionNameAttached );
               ctx->sessionNameAttached= 0;
            }
            ctx->sessionNameAttached= strdup( sessionName );
         }
         if ( ctx->sessionNameAttached )
         {
            result= true;
         }
      }
      else
      {
         ERROR("Cannot perform attach/detach on non-observer session");
      }
   }   

   return result;
}

bool AudioServerSessionDetach( AudSrv audsrv )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;

   if ( ctx )
   {
      if ( ctx->sessionType == AUDSRV_SESSION_Observer )
      {
         if ( ctx->sessionNameAttached )
         {
            free( ctx->sessionNameAttached );
            ctx->sessionNameAttached= 0;
         }
         result= true;
      }
      else
      {
         ERROR("Cannot perform attach/detach on non-observer session");
      }
   }   

   return result;
}

bool AudioServerSetAudioInfo( AudSrv audsrv, AudSrvAudioInfo *info )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen, mimeLen;
   int sendLen;
   
   TRACE1("AudioServerSetAudioInfo: audsrv %p", audsrv );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;
      
      mimeLen= strlen(info->mimeType);
      if ( mimeLen > AUDSRV_MAX_MIME_LEN )
      {
         ERROR("audio info mimeType len too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }
      mimeLen= AUDSRV_MSG_STRING_LEN(info->mimeType);
      
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + mimeLen); // mimetype
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // codec
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // pid
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // ptsOffset
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U64_LEN); // dataId
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U64_LEN); // timingId
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U64_LEN); // privateId
      
      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("audio info msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }
      
      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_AudioInfo );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_AudioInfo_Version );
      p += audsrv_conn_put_u32( p, mimeLen );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_String );
      p += audsrv_conn_put_string( p, info->mimeType );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, info->codec );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, info->pid );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U32_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, info->ptsOffset );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U64_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U64 );
      p += audsrv_conn_put_u64( p, info->dataId );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U64_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U64 );
      p += audsrv_conn_put_u64( p, info->timingId );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U64_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U64 );
      p += audsrv_conn_put_u64( p, info->privateId );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }

exit:
   TRACE1("AudioServerSetAudioInfo: audsrv %p result %d", audsrv, result );
   
   return result;
}

bool AudioServerBasetime( AudSrv audsrv, long long basetime )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("AudioServerBasetime: audsrv %p basetime %lld", audsrv, basetime );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;
      
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U64_LEN); // basetime
      
      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("audio basetime msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }
      
      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Basetime);
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Basetime_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U64_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U64 );
      p += audsrv_conn_put_u64( p, (unsigned long long)basetime );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }

exit:
   TRACE1("AudioServerBasetime: audsrv %p result %d", audsrv, result );
   
   return result;
}

bool AudioServerPlay( AudSrv audsrv )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("AudioServerPlay: audsrv %p", audsrv );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("play msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Play );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Play_Version );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }   

exit:
   TRACE1("AudioServerPlay: audsrv %p result %d", audsrv, result );

   return result;
}

bool AudioServerStop( AudSrv audsrv )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("AudioServerStop: audsrv %p", audsrv );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("stop msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Stop );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Stop_Version );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }   

exit:
   TRACE1("AudioServerStop: audsrv %p result %d", audsrv, result );

   return result;
}

bool AudioServerPause( AudSrv audsrv, bool pause )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("AudioServerPause: audsrv %p pause %d", audsrv, pause );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("pause msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, (pause ? AUDSRV_MSG_Pause : AUDSRV_MSG_UnPause) );
      p += audsrv_conn_put_u32( p, (pause ? AUDSRV_MSG_Pause_Version : AUDSRV_MSG_UnPause_Version) );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }   

exit:
   TRACE1("AudioServerPause: audsrv %p pause %d result %d", audsrv, pause, result );

   return result;
}

bool AudioServerFlush( AudSrv audsrv )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("AudioServerFlush: audsrv %p", audsrv );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("flush msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Flush );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Flush_Version );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }   

exit:
   TRACE1("AudioServerFlush: audsrv %p result %d", audsrv, result );

   return result;
}

bool AudioServerAudioSync( AudSrv audsrv, long long nowMicros, long long stc )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("AudioServerAudioSync: audsrv %p nowMicros %lld stc %lld", audsrv, nowMicros, stc );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;
      
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U64_LEN); // timeMicro
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U64_LEN); // stc
      
      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("audio sync msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }
      
      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_AudioSync );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_AudioSync_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U64_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U64 );
      p += audsrv_conn_put_u64( p, (unsigned long long)nowMicros );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U64_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U64 );
      p += audsrv_conn_put_u64( p, (unsigned long long)stc );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }

exit:
   TRACE1("AudioServerAudioSync: audsrv %p result %d", audsrv, result );
   
   return result;
}

bool AudioServerAudioData( AudSrv audsrv, unsigned char *data, unsigned len )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE3("AudioServerData: audsrv %p data %p len %u", audsrv, data, len );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + len); // buffer

      // Don't include payload data length since it doesn't occupy space
      // in our work buffer
      msgLen= AUDSRV_MSG_HDR_LEN + paramLen - len;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("audio data msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_AudioData );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_AudioData_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_BUFFER_LEN(len) );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_Buffer );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, data, len );
      
      result= (sendLen == (msgLen+len));

      pthread_mutex_unlock( &ctx->mutexSend );
   }   

exit:

   return result;
}

bool AudioServerAudioDataHandle( AudSrv audsrv, unsigned long long dataHandle )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE3("AudioServerDataHandle: audsrv %p handle %llu", audsrv, dataHandle );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U64_LEN); // dataHandle

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("audio data handle msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_AudioDataHandle );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_AudioDataHandle_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U64_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U64 );
      p += audsrv_conn_put_u64( p, dataHandle );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }   

exit:

   return result;
}

static bool audioServerMute( AudsrvApiContext *ctx, bool mute, bool global, const char *sessionName )
{
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen, nameLen;
   int sendLen;
   
   TRACE1("audioServerMute: audsrv %p mute %d global %d sessionName (%s)", ctx, mute, global, sessionName );
   if ( global && sessionName )
   {
      ERROR("audioServerMute: sessionName not permitted with global request");
      sessionName= 0;
   }

   nameLen= (sessionName ? AUDSRV_MSG_STRING_LEN(sessionName) : 0);
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // global
      paramLen += AUDSRV_MSG_TYPE_HDR_LEN+nameLen;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("mute msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, (mute ? AUDSRV_MSG_Mute : AUDSRV_MSG_UnMute) );
      p += audsrv_conn_put_u32( p, (mute ? AUDSRV_MSG_Mute_Version : AUDSRV_MSG_UnMute_Version) );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, (global ? 0x0001 : 0x0000) );
      p += audsrv_conn_put_u32( p, nameLen );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_String );
      if ( sessionName )
      {
         p += audsrv_conn_put_string( p, sessionName );
      }

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }   

exit:
   TRACE1("audioServerMute: audsrv %p mute %d result %d", ctx, mute, result );

   return result;
}

bool AudioServerGlobalMute( AudSrv audsrv, bool mute )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;

   if ( ctx )
   {
      result= audioServerMute( ctx, mute, true, 0 );
   }

   return result;
}

bool AudioServerMute( AudSrv audsrv, bool mute )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;

   if ( ctx )
   {
      if ((ctx->isPrivate == true) && ctx->sessionNamePrivate) {
          result= audioServerMute( ctx, mute, false, ctx->sessionNamePrivate );
      }
      else {
          result= audioServerMute( ctx, mute, false, ctx->sessionNameAttached );
      }
   }

   return result;
}

static bool audioServerVolume( AudsrvApiContext *ctx, float volume, bool global, const char *sessionName )
{
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen, nameLen;
   int sendLen;
   unsigned level;
   
   TRACE1("audioServerVolume: audsrv %p volume %f global %d sessionName (%s)", ctx, volume, global, sessionName );
   if ( global && sessionName )
   {
      ERROR("audioServerVolume: sessionName not permitted with global request");
      sessionName= 0;
   }

   nameLen= (sessionName ? AUDSRV_MSG_STRING_LEN(sessionName) : 0);
      
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;
      
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // level numerator
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN); // level denominator
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN); // global
      paramLen += AUDSRV_MSG_TYPE_HDR_LEN+nameLen;
      
      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("audio volume msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }
      
      if ( volume < 0.0 ) volume= 0.0;
      if ( volume > 1.0 ) volume= 1.0;
      
      level= (unsigned)(LEVEL_DENOMINATOR * volume);
      TRACE1("AudioServerVolume: audsrv %p level %u / %u", ctx, level, LEVEL_DENOMINATOR );
      
      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Volume );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_Volume_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U32_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, level );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U32_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, LEVEL_DENOMINATOR );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, (global ? 0x0001 : 0x0000) );
      p += audsrv_conn_put_u32( p, nameLen );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_String );
      if ( sessionName )
      {
         p += audsrv_conn_put_string( p, sessionName );
      }

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }

exit:
   TRACE1("AudioServerVolume: audsrv %p result %d", ctx, result );
   
   return result;
}

bool AudioServerGlobalVolume( AudSrv audsrv, float volume )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;

   if ( ctx )
   {
      result= audioServerVolume( ctx, volume, true, 0 );
   }

   return result;
}

bool AudioServerVolume( AudSrv audsrv, float volume )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   
   if ( ctx )
   {
      result= audioServerVolume( ctx, volume, false, ctx->sessionNameAttached );
   }

   return result;
}

bool AudioServerEnumerateSessions( AudSrv audsrv, AudioServerEnumSessions cb, void *userData )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   AudsrvCBCtx *pCBCtx= 0;
   unsigned long long token;
   
   TRACE1("AudioServerEnumerateSessions: audsrv %p", audsrv );

   if ( ctx )
   {
      pCBCtx= (AudsrvCBCtx*)calloc( 1, sizeof(AudsrvCBCtx));
      if ( !pCBCtx )
      {
         ERROR("No memory for session enum callback context");
         goto exit;
      }

      pCBCtx->cb.enumsess= cb;
      pCBCtx->userData= userData;

      if ( sizeof(void*) == 4 )
      {
         token= (unsigned)pCBCtx;
      }
      else
      {
         token= (unsigned long long)pCBCtx;
      }
      TRACE1("audio enum sessions: pCBCtx %p token %llx", pCBCtx, token);      

      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U64_LEN); // token

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;

      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("audio enum sessions msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_EnumSessions );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_EnumSessions_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U64_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U64 );
      p += audsrv_conn_put_u64( p, token );
      
      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      if ( result )
      {
         ctx->pendingCallbacks.push_back(pCBCtx);
      }
      else
      {
         free( pCBCtx );
      }

      pthread_mutex_unlock( &ctx->mutexSend );
   }

exit:
   TRACE1("AudioServerEnumerateSessions: audsrv %p result %d", audsrv, result );

   return result;
}

bool AudioServerGetSessionStatus( AudSrv audsrv, AudioServerSessionStatus cb, void *userData )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen, nameLen;
   int sendLen;
   AudsrvCBCtx *pCBCtx= 0;
   unsigned long long token;

   TRACE1("AudioServerGetSessionStatus: audsrv %p", audsrv );

   if ( ctx )
   {
      pCBCtx= (AudsrvCBCtx*)calloc( 1, sizeof(AudsrvCBCtx));
      if ( !pCBCtx )
      {
         ERROR("No memory for getstatus callback context");
         goto exit;
      }

      pCBCtx->cb.getstatus= cb;
      pCBCtx->userData= userData;

      if (ctx->isPrivate == true) {
         nameLen= (ctx->sessionNamePrivate ? AUDSRV_MSG_STRING_LEN(ctx->sessionNamePrivate) : 0);
      }
      else {
         nameLen= (ctx->sessionNameAttached ? AUDSRV_MSG_STRING_LEN(ctx->sessionNameAttached) : 0);
      }

      if ( sizeof(void*) == 4 )
      {
         token= (unsigned)pCBCtx;
      }
      else
      {
         token= (unsigned long long)pCBCtx;
      }
      TRACE1("audio getstatus: pCBCtx %p token %llx", pCBCtx, token);      

      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U64_LEN); // token
      paramLen += AUDSRV_MSG_TYPE_HDR_LEN+nameLen;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;

      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("audio get session status msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_GetStatus );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_GetStatus_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U64_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U64 );
      p += audsrv_conn_put_u64( p, token );
      p += audsrv_conn_put_u32( p, nameLen );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_String );
      if ( nameLen ) {
         if (ctx->isPrivate == true) {
            p += audsrv_conn_put_string( p, ctx->sessionNamePrivate );
         }
         else {
            p += audsrv_conn_put_string( p, ctx->sessionNameAttached );
         }
      }

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      if ( result )
      {
         ctx->pendingCallbacks.push_back(pCBCtx);
      }
      else
      {
         free( pCBCtx );
      }

      pthread_mutex_unlock( &ctx->mutexSend );
   }

exit:
   TRACE1("AudioServerGetSessionStatus: audsrv %p result %d", audsrv, result );

   return result;
}

bool AudioServerEnableSessionEvent( AudSrv audsrv, AudioServerSessionEvent cb, void *userData )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("AudioServerEnableSessionEvent: audsrv %p", audsrv );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      if ( ctx->sessionType != AUDSRV_SESSION_Observer )
      {
         ERROR("session event callbacks may only be added by observer sessions");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      ctx->sessionEventCB= cb;
      ctx->sessionEventUserData= userData;

      p= ctx->conn->sendbuff;
      paramLen= 0;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("enableSessionEvent msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_EnableSessionEvent );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_EnableSessionEvent_Version );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }   

exit:
   TRACE1("AudioServerEnableSessionEvent: audsrv %p result %d", audsrv, result );

   return result;
}

bool AudioServerDisableSessionEvent( AudSrv audsrv )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("AudioServerDisableSessionEvent: audsrv %p", audsrv );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );
      
      ctx->sessionEventCB= 0;
      ctx->sessionEventUserData= 0;

      p= ctx->conn->sendbuff;
      paramLen= 0;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("disableSessionEvent msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_DisableSessionEvent );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_DisableSessionEvent_Version );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }   

exit:
   TRACE1("AudioServerDisableSessionEvent: audsrv %p result %d", audsrv, result );

   return result;
}

bool AudioServerEnableEOSDetection( AudSrv audsrv, AudioServerEOS cb, void *userData )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("AudioServerEnableEOSDetection: audsrv %p", audsrv );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );
      
      ctx->eosCB= cb;
      ctx->eosUserData= userData;

      p= ctx->conn->sendbuff;
      paramLen= 0;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("enableEOS msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_EnableEOS );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_EnableEOS_Version );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }   

exit:
   TRACE1("AudioServerEnableEOSDetection: audsrv %p result %d", audsrv, result );

   return result;
}

bool AudioServerDisableEOSDetection( AudSrv audsrv )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("AudioServerDisableEOSDetection: audsrv %p", audsrv );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );
      
      ctx->eosCB= 0;
      ctx->eosUserData= 0;

      p= ctx->conn->sendbuff;
      paramLen= 0;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("disableEOS msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_DisableEOS );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_DisableEOS_Version );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }   

exit:
   TRACE1("AudioServerDisableEOSDetection: audsrv %p result %d", audsrv, result );

   return result;
}

void AudioServerSetFirstAudioCallback( AudSrv audsrv, AudioServerFirstAudio cb, void *userData )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      ctx->firstAudioCB= cb;
      ctx->firstAudioUserData= userData;

      pthread_mutex_unlock( &ctx->mutexSend );
   }
}

void AudioServerSetPTSErrorCallback( AudSrv audsrv, AudioServerPTSError cb, void *userData )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      ctx->ptsErrorCB= cb;
      ctx->ptsErrorUserData= userData;

      pthread_mutex_unlock( &ctx->mutexSend );
   }
}

void AudioServerSetUnderflowCallback( AudSrv audsrv, AudioServerUnderflow cb, void *userData )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      ctx->underflowCB= cb;
      ctx->underflowUserData= userData;

      pthread_mutex_unlock( &ctx->mutexSend );
   }
}

bool AudioServerStartCapture( AudSrv audsrv, const char *sessionName, AudioServerCapture cb, AudSrvCaptureParameters *params, void *userData )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   AudSrvCaptureParameters captureParams;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen, nameLen;
   int sendLen;
   
   TRACE1("AudioServerStartCapture: audsrv %p", audsrv );

   memset(&captureParams, 0, sizeof(AudSrvCaptureParameters));

   if ( params )
      memcpy(&captureParams, params, sizeof(AudSrvCaptureParameters));
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      ctx->captureCB= cb;
      ctx->captureUserData= userData;

      p= ctx->conn->sendbuff;
      paramLen= 0;

      nameLen= (sessionName ? strlen(sessionName) : 0);
      if ( nameLen > AUDSRV_MAX_SESSION_NAME_LEN )
      {
         ERROR("audio session name len too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }
      nameLen= (sessionName ? AUDSRV_MSG_STRING_LEN(sessionName) : 0);

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN);   // numChannels
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN);   // bitsPerSample
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U16_LEN);   // threshold
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN);   // sampleRate
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN);   // outputDelay
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U32_LEN);   // fifoSize
      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + nameLen);              // sessionName

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("startCapture msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_StartCapture );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_StartCapture_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, captureParams.numChannels );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, captureParams.bitsPerSample );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U16 );
      p += audsrv_conn_put_u16( p, captureParams.threshold );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, captureParams.sampleRate );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, captureParams.outputDelay );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U16_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U32 );
      p += audsrv_conn_put_u32( p, captureParams.fifoSize );
      p += audsrv_conn_put_u32( p, nameLen );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_String );
      if ( sessionName )
      {
         p += audsrv_conn_put_string( p, sessionName );
      }

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }

exit:

   TRACE1("AudioServerStartCapture: audsrv %p result %d", audsrv, result );

   return result;
}

bool AudioServerStopCapture( AudSrv audsrv, AudioServerCaptureDone cb, void *userData )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen;
   int sendLen;
   
   TRACE1("AudioServerStopCapture: audsrv %p", audsrv );
   
   if ( ctx )
   {
      pthread_mutex_lock( &ctx->mutexSend );

      ctx->captureDoneCB= cb;
      ctx->captureDoneUserData= userData;

      p= ctx->conn->sendbuff;
      paramLen= 0;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;
      
      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("stopCapture msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_StopCapture );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_StopCapture_Version );

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      pthread_mutex_unlock( &ctx->mutexSend );
   }

exit:

   TRACE1("AudioServerStopCapture: audsrv %p result %d", audsrv, result );

   return result;
}

bool AudioServerGetCaptureSessionStatus( AudSrv audsrv, const char *sessionName, AudioServerSessionStatus cb, void *userData )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)audsrv;
   bool result= false;
   unsigned char *p;
   int msgLen, paramLen, nameLen;
   int sendLen;
   AudsrvCBCtx *pCBCtx= 0;
   unsigned long long token;

   TRACE1("AudioServerGetCaptureSessionStatus: audsrv %p", audsrv );

   if ( ctx )
   {
      pCBCtx= (AudsrvCBCtx*)calloc( 1, sizeof(AudsrvCBCtx));
      if ( !pCBCtx )
      {
         ERROR("No memory for getstatus callback context");
         goto exit;
      }

      pCBCtx->cb.getstatus= cb;
      pCBCtx->userData= userData;

      if ( sizeof(void*) == 4 )
      {
         token= (unsigned)pCBCtx;
      }
      else
      {
         token= (unsigned long long)pCBCtx;
      }
      TRACE1("audio getstatus: pCBCtx %p token %llx", pCBCtx, token);      

      pthread_mutex_lock( &ctx->mutexSend );

      p= ctx->conn->sendbuff;
      paramLen= 0;

      nameLen= (sessionName ? strlen(sessionName) : 0);
      if ( nameLen > AUDSRV_MAX_SESSION_NAME_LEN )
      {
         ERROR("audio session name len too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }
      nameLen= (sessionName ? AUDSRV_MSG_STRING_LEN(sessionName) : 0);

      paramLen += (AUDSRV_MSG_TYPE_HDR_LEN + AUDSRV_MSG_U64_LEN); // token
      paramLen += AUDSRV_MSG_TYPE_HDR_LEN+nameLen;

      msgLen= AUDSRV_MSG_HDR_LEN + paramLen;

      if ( msgLen > AUDSRV_MAX_MSG )
      {
         ERROR("audio get session status msg too large");
         pthread_mutex_unlock( &ctx->mutexSend );
         goto exit;
      }

      p += audsrv_conn_put_u32( p, paramLen );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_GetStatus );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_GetStatus_Version );
      p += audsrv_conn_put_u32( p, AUDSRV_MSG_U64_LEN );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_U64 );
      p += audsrv_conn_put_u64( p, token );
      p += audsrv_conn_put_u32( p, nameLen );
      p += audsrv_conn_put_u32( p, AUDSRV_TYPE_String );
      if ( sessionName )
      {
         p += audsrv_conn_put_string( p, sessionName );
      }

      sendLen= audsrv_conn_send( ctx->conn, ctx->conn->sendbuff, msgLen, NULL, 0 );
      
      result= (sendLen == msgLen);

      if ( result )
      {
         ctx->pendingCallbacks.push_back(pCBCtx);
      }
      else
      {
         free( pCBCtx );
      }

      pthread_mutex_unlock( &ctx->mutexSend );
   }

exit:
   TRACE1("AudioServerGetCaptureSessionStatus: audsrv %p result %d", audsrv, result );

   return result;
}

static bool audsrv_connect_socket( AudsrvApiContext *ctx )
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

   ctx->fdSocket= socket( PF_LOCAL, SOCK_STREAM|SOCK_CLOEXEC, 0 );
   if ( ctx->fdSocket < 0 )
   {
      ERROR("unable to open socket: errno %d", errno );
      goto exit;
   }
   
   addrSize= offsetof(struct sockaddr_un, sun_path) + pathSize;
   
   rc= connect(ctx->fdSocket, (struct sockaddr *)&ctx->addr, addrSize );
   if ( rc < 0 )
   {
      ERROR("connect failed for socket: errno %d", errno );
      goto exit;
   }

   result= true;

exit:

   if ( !result )
   {
      ctx->addr.sun_path[0]= '\0';
      
      if ( ctx->fdSocket >= 0 )
      {
         close( ctx->fdSocket );
         ctx->fdSocket= -1;
      }
   }

   return result;
}

static void* audsrv_receive_thread( void *arg )
{
   AudsrvApiContext *ctx= (AudsrvApiContext*)arg;

   TRACE1( "audsrv_receive_thread: enter" );

   ctx->receiveThreadStarted= true;
   
   pthread_mutex_lock( &ctx->mutexRecv );
   ctx->receiveThreadReady= true;
   pthread_mutex_unlock( &ctx->mutexRecv );
   
   while ( !ctx->receiveThreadStopRequested )
   {
      int consumed;
      int len= audsrv_conn_recv( ctx->conn );
      if ( !ctx->receiveThreadStopRequested && (len > 0) )
      {
         if ( ctx->conn->count >= AUDSRV_MSG_HDR_LEN )
         {
            do
            {
               consumed= audsrv_process_message( ctx );
            }
            while( consumed > 0 );
         }
      }
   }

   ctx->receiveThreadStarted= true;
   TRACE1( "audsrv_receive_thread: exit" );
   return NULL;
}

static int audsrv_process_message( AudsrvApiContext *ctx )
{
   int consumed= 0;
   int avail= ctx->conn->count;
   unsigned msglen, msgid, version;
   
   if ( avail < AUDSRV_MSG_HDR_LEN )
   {
      goto exit;
   }
   
   msglen= audsrv_conn_peek_u32( ctx->conn );
   TRACE2("audsrv_process_message: peeked msglen %d", msglen);
   if ( msglen+AUDSRV_MSG_HDR_LEN > ctx->conn->count )
   {
      goto exit;
   }
   
   audsrv_conn_skip( ctx->conn, 4 );
   msgid= audsrv_conn_get_u32( ctx->conn );
   version= audsrv_conn_get_u32( ctx->conn );
   TRACE2("audsrv_process_message: msglen %d msgid %d version %d", msglen, msgid, version);
   
   consumed += AUDSRV_MSG_HDR_LEN;
      
   switch( msgid )
   {
      case AUDSRV_MSG_Init:
      case AUDSRV_MSG_AudioInfo:
      case AUDSRV_MSG_Play:
      case AUDSRV_MSG_Stop:
      case AUDSRV_MSG_Pause:
      case AUDSRV_MSG_UnPause:
      case AUDSRV_MSG_Flush:
      case AUDSRV_MSG_AudioSync:
      case AUDSRV_MSG_AudioData:
      case AUDSRV_MSG_Mute:
      case AUDSRV_MSG_UnMute:
      case AUDSRV_MSG_Volume:
      case AUDSRV_MSG_EnableEOS:
      case AUDSRV_MSG_DisableEOS:
      case AUDSRV_MSG_StartCapture:
      case AUDSRV_MSG_StopCapture:
      case AUDSRV_MSG_EnumSessions:
      case AUDSRV_MSG_GetStatus:
      case AUDSRV_MSG_EnableSessionEvent:
      case AUDSRV_MSG_DisableSessionEvent:
         ERROR("ignoring msg %d inappropriate for client to receive", msgid);
         audsrv_conn_skip( ctx->conn, msglen );
         consumed += msglen;
         break;

      case AUDSRV_MSG_SessionEvent:
         consumed += audsrv_process_session_event( ctx, msglen, version );
         break;

      case AUDSRV_MSG_EOSDetected:
         consumed += audsrv_process_eosdetected( ctx, msglen, version );
         break;

      case AUDSRV_MSG_FirstAudio:
         consumed += audsrv_process_firstaudio( ctx, msglen, version );
         break; 
        
      case AUDSRV_MSG_PtsError:
         consumed += audsrv_process_ptserror( ctx, msglen, version );
         break; 
        
      case AUDSRV_MSG_Underflow:
         consumed += audsrv_process_underflow( ctx, msglen, version );
         break; 
        
      case AUDSRV_MSG_CaptureParameters:
         consumed += audsrv_process_capture_parameters( ctx, msglen, version );
         break; 
        
      case AUDSRV_MSG_CaptureData:
         consumed += audsrv_process_capture_data( ctx, msglen, version );
         break; 
        
      case AUDSRV_MSG_CaptureDone:
         consumed += audsrv_process_capture_done( ctx, msglen, version );
         break;

      case AUDSRV_MSG_EnumSessionsResults:
         consumed += audsrv_process_enum_sessions_results( ctx, msglen, version );
         break;

      case AUDSRV_MSG_GetStatusResults:
         consumed += audsrv_process_getstatus_results( ctx, msglen, version );
         break;        

      default:
         INFO("ignoring unknown command %d len %d", msgid, msglen );
         audsrv_conn_skip( ctx->conn, msglen );
         consumed += msglen;
         break;
   }

   if ( ctx->discPending )
   {
      ctx->discPending= false;
      AudioServerDisconnect( (AudSrv)ctx );
   }   

exit:

   return consumed;
}

static int audsrv_process_session_event( AudsrvApiContext *ctx, unsigned msglen, unsigned version )
{
   TRACE1("msg: session event version %d", version);
   
   if ( ctx )
   {   
      if ( version <= AUDSRV_MSG_SessionEvent_Version )
      {
         unsigned len, type;
         int event;
         AudSrvSessionInfo info;

         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U16 )
         {
            ERROR("expecting type %d (U16) not type %d for session event arg 1 (event)", AUDSRV_TYPE_U16, type );
            goto exit;
         }
         
         event= audsrv_conn_get_u32( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U32 )
         {
            ERROR("expecting type %d (U32) not type %d for session event (pid)", AUDSRV_TYPE_U32, type );
            goto exit;
         }
         
         info.pid= audsrv_conn_get_u32( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U16 )
         {
            ERROR("expecting type %d (U16) not type %d for session event (sessionType)", AUDSRV_TYPE_U16, type );
            goto exit;
         }
         
         info.sessionType= audsrv_conn_get_u16( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_String )
         {
            ERROR("expecting type %d (string) not type %d for session event (sessionName)", AUDSRV_TYPE_String, type );
            goto exit;
         }
         if ( len > AUDSRV_MAX_SESSION_NAME_LEN )
         {
            ERROR("sessionName too long (%d) for session event", len );
            goto exit;
         }
         
         if ( len )
         {
            audsrv_conn_get_string( ctx->conn, info.sessionName );
         }

         pthread_mutex_lock( &ctx->mutexSend );
         if ( ctx->sessionEventCB )
         {
            ctx->inCallback= true;
            ctx->sessionEventCB( ctx->sessionEventUserData, event, &info );
            ctx->inCallback= false;
         }
         pthread_mutex_unlock( &ctx->mutexSend );
      }
   }

exit:

   return msglen;
}

static int audsrv_process_eosdetected( AudsrvApiContext *ctx, unsigned msglen, unsigned version )
{
   TRACE1("msg: eosdetected version %d", version);
   
   if ( ctx )
   {   
      if ( version <= AUDSRV_MSG_EOSDetected_Version )
      {
         pthread_mutex_lock( &ctx->mutexSend );
         if ( ctx->eosCB )
         {
            ctx->inCallback= true;
            ctx->eosCB( ctx->eosUserData );
            ctx->inCallback= false;
         }
         pthread_mutex_unlock( &ctx->mutexSend );
      }
   }
   
   return msglen;
}

static int audsrv_process_firstaudio( AudsrvApiContext *ctx, unsigned msglen, unsigned version )
{
   TRACE1("msg: firstaudio version %d", version);
   
   if ( ctx )
   {   
      if ( version <= AUDSRV_MSG_FirstAudio_Version )
      {
         if ( ctx->firstAudioCB )
         {
            ctx->inCallback= true;
            ctx->firstAudioCB( ctx->firstAudioUserData );
            ctx->inCallback= false;
         }
      }
   }
   
   return msglen;
}

static int audsrv_process_ptserror( AudsrvApiContext *ctx, unsigned msglen, unsigned version )
{
   TRACE1("msg: ptserror version %d", version);
   
   if ( ctx )
   {   
      if ( version <= AUDSRV_MSG_PtsError_Version )
      {
         unsigned len, type;
         unsigned count;


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U32 )
         {
            ERROR("expecting type %d (U32) not type %d for ptserror arg 1 (count)", AUDSRV_TYPE_U32, type );
            goto exit;
         }
         
         count= audsrv_conn_get_u32( ctx->conn );
         
         if ( ctx->ptsErrorCB )
         {
            ctx->inCallback= true;
            ctx->ptsErrorCB( ctx->ptsErrorUserData, count );
            ctx->inCallback= false;
         }
      }
   }

exit:

   return msglen;
}

static int audsrv_process_underflow( AudsrvApiContext *ctx, unsigned msglen, unsigned version )
{
   TRACE1("msg: underflow version %d", version);
   
   if ( ctx )
   {   
      if ( version <= AUDSRV_MSG_Underflow_Version )
      {
         unsigned len, type;
         unsigned count, bufferedBytes, queuedFrames;


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U32 )
         {
            ERROR("expecting type %d (U32) not type %d for underflow arg 1 (count)", AUDSRV_TYPE_U32, type );
            goto exit;
         }
         
         count= audsrv_conn_get_u32( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U32 )
         {
            ERROR("expecting type %d (U32) not type %d for underflow arg 2 (bufferedBytes)", AUDSRV_TYPE_U32, type );
            goto exit;
         }
         
         bufferedBytes= audsrv_conn_get_u32( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U32 )
         {
            ERROR("expecting type %d (U32) not type %d for underflow arg 3 (queuedFrames)", AUDSRV_TYPE_U32, type );
            goto exit;
         }
         
         queuedFrames= audsrv_conn_get_u32( ctx->conn );

         if ( ctx->underflowCB )
         {
            ctx->inCallback= true;
            ctx->underflowCB( ctx->ptsErrorUserData, count, bufferedBytes, queuedFrames );
            ctx->inCallback= false;
         }
      }
   }

exit:
   
   return msglen;
}

static int audsrv_process_capture_parameters( AudsrvApiContext *ctx, unsigned msglen, unsigned version )
{
   TRACE1("msg: capture parameters version %d", version);
   
   if ( ctx )
   {   
      if ( version <= AUDSRV_MSG_CaptureParameters_Version )
      {
         unsigned len, type;
         unsigned version, sampleRate, outputDelay;
         unsigned short numChannels, bitsPerSample;


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U32 )
         {
            ERROR("expecting type %d (U32) not type %d for captureParameters arg 1 (version)", AUDSRV_TYPE_U32, type );
            goto exit;
         }
         
         version= audsrv_conn_get_u32( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U16 )
         {
            ERROR("expecting type %d (U16) not type %d for captureParameters arg 2 (numChannels)", AUDSRV_TYPE_U16, type );
            goto exit;
         }
         
         numChannels= audsrv_conn_get_u16( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U16 )
         {
            ERROR("expecting type %d (U16) not type %d for captureParameters arg 3 (bitsPerSample)", AUDSRV_TYPE_U16, type );
            goto exit;
         }
         
         bitsPerSample= audsrv_conn_get_u16( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U32 )
         {
            ERROR("expecting type %d (U32) not type %d for captureParameters arg 4 (sampleRate)", AUDSRV_TYPE_U32, type );
            goto exit;
         }
         
         sampleRate= audsrv_conn_get_u32( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U32 )
         {
            ERROR("expecting type %d (U32) not type %d for captureParameters arg 5 (outputDelay)", AUDSRV_TYPE_U32, type );
            goto exit;
         }
         
         outputDelay= audsrv_conn_get_u32( ctx->conn );


         if ( version != ctx->captureParameters.version )
         {
            ctx->captureParameters.version= version;
            ctx->captureParameters.numChannels= numChannels;
            ctx->captureParameters.bitsPerSample= bitsPerSample;
            ctx->captureParameters.sampleRate= sampleRate;
            ctx->captureParameters.outputDelay= outputDelay;
            
            if ( ctx->captureCB )
            {
               ctx->inCallback= true;
               ctx->captureCB( ctx->captureUserData, &ctx->captureParameters, NULL, 0 );
               ctx->inCallback= false;
            }
         }
      }
   }

exit:
   
   return msglen;
}

static int audsrv_process_capture_data( AudsrvApiContext *ctx, unsigned msglen, unsigned version )
{
   TRACE1("msg: capture data version %d", version);
   
   if ( ctx )
   {   
      if ( version <= AUDSRV_MSG_CaptureData_Version )
      {
         unsigned len, type;
         unsigned datalen;
         unsigned char *data;

         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_Buffer )
         {
            ERROR("expecting type %d (buffer) not type %d for captureData arg 1 (data)", AUDSRV_TYPE_Buffer, type );
            goto exit;
         }
         
         audsrv_conn_get_buffer( ctx->conn, len, &data, &datalen );
         if ( data && datalen )
         {
            if ( ctx->captureCB )
            {
               ctx->captureCB( ctx->captureUserData, &ctx->captureParameters, data, datalen );               
            }
            len -= datalen;
         }

         if ( len )
         {
            audsrv_conn_get_buffer( ctx->conn, len, &data, &datalen );
            if ( data && datalen )
            {
               if ( ctx->captureCB )
               {
                  ctx->inCallback= true;
                  ctx->captureCB( ctx->captureUserData, &ctx->captureParameters, data, datalen );               
                  ctx->inCallback= false;
               }
            }
         }
      }
   }

exit:
   
   return msglen;
}

static int audsrv_process_capture_done( AudsrvApiContext *ctx, unsigned msglen, unsigned version )
{
   TRACE1("msg: capture done version %d", version);
   
   if ( ctx )
   {   
      if ( version <= AUDSRV_MSG_CaptureDone_Version )
      {
         pthread_mutex_lock( &ctx->mutexSend );
         if ( ctx->captureDoneCB )
         {
            ctx->inCallback= true;
            ctx->captureDoneCB( ctx->captureDoneUserData );
            ctx->inCallback= false;
         }
         ctx->captureCB= 0;
         ctx->captureDoneCB= 0;
         pthread_mutex_unlock( &ctx->mutexSend );
      }
   }
   
   return msglen;
}

static int audsrv_process_enum_sessions_results( AudsrvApiContext *ctx, unsigned msglen, unsigned version )
{
   TRACE1("msg: emum sessions results version %d", version);

   if ( ctx )
   {   
      if ( version <= AUDSRV_MSG_EnumSessionsResults_Version )
      {
         unsigned len, type;
         unsigned long long token, tokenMatch;
         unsigned result, sessionCount;
         AudsrvCBCtx *pCBCtx= 0;   


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U64 )
         {
            ERROR("expecting type %d (U64) not type %d for enumSessionsResults arg 1 (token)", AUDSRV_TYPE_U64, type );
            goto exit;
         }
         
         token= audsrv_conn_get_u64( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U16 )
         {
            ERROR("expecting type %d (U16) not type %d for enumSessionsResults arg 2 (result)", AUDSRV_TYPE_U16, type );
            goto exit;
         }
         
         result= audsrv_conn_get_u16( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U16 )
         {
            ERROR("expecting type %d (U16) not type %d for enumSessionsResults arg 3 (sessionCount)", AUDSRV_TYPE_U16, type );
            goto exit;
         }
         
         sessionCount= audsrv_conn_get_u16( ctx->conn );

         TRACE1("enum sessions results: token %llx result %u sessionCount %u", token, result, sessionCount );

         pthread_mutex_lock( &ctx->mutexSend );

         for( std::vector<AudsrvCBCtx*>::iterator it= ctx->pendingCallbacks.begin();
              it != ctx->pendingCallbacks.end();
              ++it )
         {
            AudsrvCBCtx *pCBCtxIter= (*it);

            if ( sizeof(void*) == 4 )
            {
               tokenMatch= (unsigned)pCBCtxIter;
            }
            else
            {
               tokenMatch= (unsigned long long)pCBCtxIter;
            }

            if ( token == tokenMatch )
            {
               pCBCtx= pCBCtxIter;
               ctx->pendingCallbacks.erase( it );
               break;
            }
         }

         if ( pCBCtx )
         {
            AudSrvSessionInfo *pInfo= 0;

            if ( (result == 0) && (sessionCount > 0) )
            {
               pInfo= (AudSrvSessionInfo*)calloc( sessionCount, sizeof(AudSrvSessionInfo) );
               if ( pInfo )
               {
                  bool error= false;
                  for( int i= 0; i < sessionCount; ++i )
                  {
                     len= audsrv_conn_get_u32( ctx->conn );
                     type= audsrv_conn_get_u32( ctx->conn );
                     
                     if ( type != AUDSRV_TYPE_U32 )
                     {
                        ERROR("expecting type %d (U32) not type %d for enumSessionsResults session %d pid", AUDSRV_TYPE_U32, type, i );
                        error= true;
                        break;
                     }
                     
                     pInfo[i].pid= audsrv_conn_get_u32( ctx->conn );


                     len= audsrv_conn_get_u32( ctx->conn );
                     type= audsrv_conn_get_u32( ctx->conn );
                     
                     if ( type != AUDSRV_TYPE_U16 )
                     {
                        ERROR("expecting type %d (U16) not type %d for enumSessionsResults session %d sessionType", AUDSRV_TYPE_U16, type, i );
                        error= true;
                        break;
                     }
                     
                     pInfo[i].sessionType= audsrv_conn_get_u16( ctx->conn );


                     len= audsrv_conn_get_u32( ctx->conn );
                     type= audsrv_conn_get_u32( ctx->conn );
                     
                     if ( type != AUDSRV_TYPE_String )
                     {
                        ERROR("expecting type %d (string) not type %d for enumSessionsResults session %d sessionName)", AUDSRV_TYPE_String, type, i );
                        error= true;
                        break;
                     }
                     if ( len > AUDSRV_MAX_SESSION_NAME_LEN )
                     {
                        ERROR("sessionName too long (%d) for enumSessionsResults session %d", len, i );
                        error= true;
                        break;
                     }
                     
                     if ( len )
                     {
                        audsrv_conn_get_string( ctx->conn, pInfo[i].sessionName );
                     }
                  }
                  if ( error )
                  {
                     result= 1;
                     free( pInfo );
                     pInfo= 0;
                  }
               }
               else
               {
                  ERROR("No memory for enumn sessions results info");
                  result= 1;
               }
            }

            ctx->inCallback= true;
            pCBCtx->cb.enumsess( pCBCtx->userData, result, sessionCount, pInfo );
            ctx->inCallback= false;
            
            if ( pInfo )
            {
               free( pInfo );
            }
            free( pCBCtx );
         }
         else
         {
            ERROR("enum session results: no match for token %llx", token);
         }

         pthread_mutex_unlock( &ctx->mutexSend );
      }
   }

exit:
   
   return msglen;
}

static int audsrv_process_getstatus_results( AudsrvApiContext *ctx, unsigned msglen, unsigned version )
{
   TRACE1("msg: getstatus results version %d", version);

   if ( ctx )
   {   
      if ( version <= AUDSRV_MSG_GetStatusResults_Version )
      {
         unsigned len, type;
         unsigned long long token, tokenMatch;
         unsigned result;
         unsigned numerator, denominator;
         AudsrvCBCtx *pCBCtx= 0;   
         AudSrvSessionStatus status, *pStatus;

         pStatus= &status;
         memset( &status, 0, sizeof(status) );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U64 )
         {
            ERROR("expecting type %d (U64) not type %d for getStatusResults arg 1 (token)", AUDSRV_TYPE_U64, type );
            goto exit;
         }         
         token= audsrv_conn_get_u64( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U16 )
         {
            ERROR("expecting type %d (U16) not type %d for getStatusResults arg 2 (result)", AUDSRV_TYPE_U16, type );
            goto exit;
         }         
         result= audsrv_conn_get_u16( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U16 )
         {
            ERROR("expecting type %d (U16) not type %d for getStatusResults arg 3 (global muted)", AUDSRV_TYPE_U16, type );
            goto exit;
         }
         status.globalMuted= audsrv_conn_get_u16( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U32 )
         {
            ERROR("expecting type %d (U32) not type %d for getStatusResults arg 4 (global volume num)", AUDSRV_TYPE_U32, type );
            goto exit;
         }
         numerator= audsrv_conn_get_u32( ctx->conn );


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U32 )
         {
            ERROR("expecting type %d (U32) not type %d for getStatusResults arg 5 (global volume denom)", AUDSRV_TYPE_U32, type );
            goto exit;
         }
         denominator= audsrv_conn_get_u32( ctx->conn );
         if ( denominator == 0 )
         {
            ERROR("zero global volume denominator for getStatusResults");
            goto exit;
         }
         status.globalVolume= ((float)numerator)/((float)denominator);


         len= audsrv_conn_get_u32( ctx->conn );
         type= audsrv_conn_get_u32( ctx->conn );
         
         if ( type != AUDSRV_TYPE_U16 )
         {
            ERROR("expecting type %d (U16) not type %d for getStatusResults arg 6 (ready)", AUDSRV_TYPE_U16, type );
            goto exit;
         }         
         status.ready= audsrv_conn_get_u16( ctx->conn );
         


         TRACE1("getstatus results: token %llx result %u", token, result );

         pthread_mutex_lock( &ctx->mutexSend );

         for( std::vector<AudsrvCBCtx*>::iterator it= ctx->pendingCallbacks.begin();
              it != ctx->pendingCallbacks.end();
              ++it )
         {
            AudsrvCBCtx *pCBCtxIter= (*it);

            if ( sizeof(void*) == 4 )
            {
               tokenMatch= (unsigned)pCBCtxIter;
            }
            else
            {
               tokenMatch= (unsigned long long)pCBCtxIter;
            }

            if ( token == tokenMatch )
            {
               pCBCtx= pCBCtxIter;
               ctx->pendingCallbacks.erase( it );
               break;
            }
         }

         if ( pCBCtx )
         {
            bool error= false;

            if ( result == 0 )
            {
               len= audsrv_conn_get_u32( ctx->conn );
               type= audsrv_conn_get_u32( ctx->conn );
               
               if ( type != AUDSRV_TYPE_U16 )
               {
                  ERROR("expecting type %d (U16) not type %d for getStatusResults playing", AUDSRV_TYPE_U16, type );
                  error= true;
               }
               else
               {
                  status.playing= audsrv_conn_get_u16( ctx->conn );
               }

               if ( !error )
               {
                  len= audsrv_conn_get_u32( ctx->conn );
                  type= audsrv_conn_get_u32( ctx->conn );
                  
                  if ( type != AUDSRV_TYPE_U16 )
                  {
                     ERROR("expecting type %d (U16) not type %d for getStatusResults paused", AUDSRV_TYPE_U16, type );
                     error= true;
                  }
                  else
                  {
                     status.paused= audsrv_conn_get_u16( ctx->conn );
                  }
               }

               if ( !error )
               {
                  len= audsrv_conn_get_u32( ctx->conn );
                  type= audsrv_conn_get_u32( ctx->conn );

                  if ( type != AUDSRV_TYPE_U16 )
                  {
                     ERROR("expecting type %d (U16) not type %d for getStatusResults muted", AUDSRV_TYPE_U16, type );
                     error= true;
                  }
                  else
                  {
                     status.muted= audsrv_conn_get_u16( ctx->conn );
                  }
               }

               if ( !error )
               {
                  len= audsrv_conn_get_u32( ctx->conn );
                  type= audsrv_conn_get_u32( ctx->conn );
                  
                  if ( type != AUDSRV_TYPE_U32 )
                  {
                     ERROR("expecting type %d (U32) not type %d for getStatusResults volume num", AUDSRV_TYPE_U32, type );
                     error= true;
                  }
                  else
                  {
                     numerator= audsrv_conn_get_u32( ctx->conn );
                  }
               }

               if ( !error )
               {
                  len= audsrv_conn_get_u32( ctx->conn );
                  type= audsrv_conn_get_u32( ctx->conn );
                  
                  if ( type != AUDSRV_TYPE_U32 )
                  {
                     ERROR("expecting type %d (U32) not type %d for getStatusResults volume denom", AUDSRV_TYPE_U32, type );
                     error= true;
                  }
                  else
                  {
                     denominator= audsrv_conn_get_u32( ctx->conn );
                     if ( denominator == 0 )
                     {
                        ERROR("zero volume denominator for getStatusResults");
                        error= true;
                     }
                  }
               }

               status.volume= ((float)numerator)/((float)denominator);

               if ( !error )
               {
                  len= audsrv_conn_get_u32( ctx->conn );
                  type= audsrv_conn_get_u32( ctx->conn );
                  
                  if ( type != AUDSRV_TYPE_String )
                  {
                     ERROR("expecting type %d (string) not type %d for getStatusResults sessionName)", AUDSRV_TYPE_String, type );
                     error= true;
                  }
                  else if ( len > AUDSRV_MAX_SESSION_NAME_LEN )
                  {
                     ERROR("sessionName too long (%d) for getStatusResults", len );
                     error= true;
                  }                  
                  else if ( len )
                  {
                     audsrv_conn_get_string( ctx->conn, status.sessionName );
                  }
               }
            }

            if ( error )
            {
               result= 0;
               pStatus= 0;
            }

            ctx->inCallback= true;
            pCBCtx->cb.getstatus( pCBCtx->userData, result, pStatus );
            ctx->inCallback= false;
            
            free( pCBCtx );
         }
         else
         {
            ERROR("enum session results: no match for token %llx", token);
         }

         pthread_mutex_unlock( &ctx->mutexSend );
      }
   }

exit:
   
   return msglen;
}

/** @} */
/** @} */

