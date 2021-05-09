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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <signal.h>
#include <termios.h>
#include <unistd.h>
#include <errno.h>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include "audioserver.h"

#define CAPTURE_BUFFER_SIZE (2*1024*1024)

typedef enum _Container
{
   Container_unknown,
   Container_ts,
   Container_mp4,
   Container_wav
} Container;

typedef struct _AppCtx
{
   GstElement *pipeline;
   GstElement *appsrc;
   GstElement *queue;
   GstElement *demux;
   GstElement *parser;
   GstElement *audiosink;
   GList *factoryListDemux;
   GList *factoryListParser;
   GstCaps *capsContainer;
   GstBus *bus;
   GMainLoop *loop;
   const char *demuxName;
   const char *clipFilename;
   int containerType;
   float volume;
   bool global;
   bool connected;
   bool muted;
   bool clipPlaying;
   bool eos;
   bool verbose;
   bool takeDemuxToNull;
   const char *sessionName;
   int sessionType;
   bool isPrivate;
   AudSrv audsrvPlayback;
   bool capture;
   AudSrv audsrvCapture;
   AudSrvCaptureParameters captureParams;
   bool captureToFile;
   FILE *pCaptureFile;
   unsigned captureByteCount;
   int captureBufferCapacity;
   unsigned char *captureBuffer;
   int enumCount;
   AudSrvSessionInfo *enumResults;
   AudSrv audsrvObserver;
   const char *sessionNameToCapture;
   char *sessionNameAttached;
   bool haveSessionEvents;
} AppCtx;

static void showUsage();
static void enableTTYBlocking( bool blocking );
static bool keyHit();
static gboolean busCallback(GstBus *bus, GstMessage *message, gpointer data);
static void demuxSrcPadAdded( GstElement *element, GstPad *srcPad, gpointer data );
static bool createPipeline( AppCtx *ctx );
static void destroyPipeline( AppCtx *ctx );
static void pushData( AppCtx *ctx, char *data, int dataSize );
static void playClip( AppCtx *ctx );
static void stopClip( AppCtx *ctx );
static void getStatus( AppCtx *ctx );
static void showKeyHelp();
static void processInput( AppCtx *ctx );
static void volumeUp( AppCtx *ctx );
static void volumeDown( AppCtx *ctx );
static void toggleConnect( AppCtx *ctx );
static void toggleMute( AppCtx *ctx );
static void toggleSessionType( AppCtx *ctx );
static void togglePrivate( AppCtx *ctx );
static void toggleCapture( AppCtx *ctx );
static void captureCallback( void *userData, AudSrvCaptureParameters *params, unsigned char *data, int datalen );
static void captureDoneCallback( void *userData );
static void enumerateSessions( AppCtx *ctx );
static void enumerateSessionsCallback( void *userData, int result, int count, AudSrvSessionInfo *sessionInfo );
static void toggleAttach( AppCtx *ctx );
static void sessionEventCallback( void *userData, int event, AudSrvSessionInfo *sessionInfo );
static void toggleSessionEvent( AppCtx *ctx );
static bool writeLE16( FILE *pFile, unsigned n );
static bool writeLE32( FILE *pFile, unsigned n );
static void waveWriteHeader( AppCtx *ctx );
static void waveWriteData( AppCtx *ctx, unsigned char *data, int datalen );
static void waveFinish( AppCtx *ctx );

static bool g_quit= false;

static void showUsage()
{
   printf("usage:\n");
   printf(" audioserver-test [options] [<audio-ts-file>]\n" );
   printf("where [options] are:\n" );
   printf("  --demux <demuxname> : demux element name\n" );
   printf("  --n <true|false> : take demux to NULL state between clips if true\n" );
   printf("  --s <session-name> : session name\n" );
   printf("  --c <session-name> : session-name to capture\n" );
   printf("  --capfile : capture directly to file\n" );
   printf("  --verbose : emit verbose logging\n" );
   printf("  -? : show usage\n" );
   printf("\n" );   
}

static void signalHandler(int signum)
{
   printf("signalHandler: signum %d\n", signum);
	g_quit= true;
}

static void enableTTYBlocking( bool blocking )
{
   int bits, mask;
   struct termios ttyState;

   if ( !blocking )
   {
      ttyState.c_cc[VMIN]= 1;
   }
   
   mask= (blocking ? -1 : ~(ICANON|ECHO));
   bits= (blocking ? (ICANON|ECHO) : 0);
   
   tcgetattr(STDIN_FILENO, &ttyState);
   ttyState.c_lflag= ((ttyState.c_lflag & mask) | bits);

   tcsetattr(STDIN_FILENO, TCSANOW, &ttyState);   
}

static bool keyHit()
{
   bool isKeyHit= false;
   fd_set descSet;
   struct timeval timeout;

   timeout.tv_sec= 0;  
   timeout.tv_usec= 0;  
   FD_ZERO(&descSet);  
   FD_SET(STDIN_FILENO, &descSet);
   select(STDIN_FILENO+1, &descSet, NULL, NULL, &timeout);
   isKeyHit= FD_ISSET(STDIN_FILENO, &descSet);

   return isKeyHit;
}

static gboolean busCallback(GstBus *bus, GstMessage *message, gpointer data)
{
   AppCtx *ctx= (AppCtx*)data;
   
   switch ( GST_MESSAGE_TYPE(message) ) 
   {
      case GST_MESSAGE_ERROR: 
         {
            GError *error;
            gchar *debug;
            
            gst_message_parse_error(message, &error, &debug);
            g_print("Error: %s\n", error->message);
            if ( debug )
            {
               g_print("Debug info: %s\n", debug);
            }
            g_error_free(error);
            g_free(debug);
            g_quit= true;
            g_main_loop_quit( ctx->loop );
         }
         break;
     case GST_MESSAGE_EOS:
         g_print( "EOS ctx %p\n", ctx );
         ctx->eos= true;
         break;
     default:
         break;
    }
    return TRUE;
}

static void demuxSrcPadAdded( GstElement *element, GstPad *srcPad, gpointer data )
{
   AppCtx *ctx= (AppCtx*)data;
   bool error= false;
   
   g_print("demux src pad added: %s:%s\n", GST_DEBUG_PAD_NAME(srcPad));

   gchar *name= gst_pad_get_name(srcPad);
   if ( name )
   {
      if ( strstr( name, "aud" ) )
      {
         switch( ctx->containerType )
         {
            case Container_ts:
               if ( !gst_element_link( ctx->demux, ctx->audiosink ) )
               {
                  g_print("Error: unable link demux and audiosink\n" );
                  error= true;
               }   
               break;
            case Container_mp4:
               if ( !gst_element_link_many( ctx->demux, ctx->parser, ctx->audiosink, NULL ) )
               {
                  if ( !gst_element_link_many( ctx->demux, ctx->audiosink, NULL) )
                  {
                     g_print("Error: unable link demux, parser, and audiosink\n" );
                     error= true;
                  }
               }   
               break;
            default:
               break;
         }
         if ( !error )
         {
            g_print("demux and audiosink linked\n");
         }
      }
      g_free(name);
   }
}

static bool getContainerType( AppCtx *ctx )
{
   bool result= false;
   if ( ctx->clipFilename )
   {
      const char *type= strrchr( ctx->clipFilename, '.' );
      if ( type )
      {
         int len= strlen(type);
         if ( (len == 3 ) && !strncmp( type, ".ts", len ) )
         {
            ctx->containerType= Container_ts;
         }
         else if ( (len == 4 ) && !strncmp( type, ".mp4", len ) )
         {
            ctx->containerType= Container_mp4;
         }
         else if ( (len == 4 ) && !strncmp( type, ".wav", len ) )
         {
            ctx->containerType= Container_wav;
         }
         if ( ctx->containerType != Container_unknown )
         {
            result= true;
         }
      }
   }
   return result;
}

static bool getContainerCaps( AppCtx *ctx )
{
   bool result= true;
   switch( ctx->containerType )
   {
      case Container_ts:
         ctx->capsContainer= gst_caps_new_simple( "video/mpegts",
                                                  "systemstream", G_TYPE_BOOLEAN, "true",
                                                  "packetsize", G_TYPE_INT, 188,
                                                  NULL );
         break;
      case Container_mp4:
         ctx->capsContainer= gst_caps_new_simple( "application/x-3gp", NULL );
         break;
      case Container_wav:
         ctx->capsContainer= gst_caps_new_simple( "audio/x-wav", NULL );
         break;
      default:
         break;
   }
   if ( !ctx->capsContainer )
   {
      g_print("Error: unable to create caps for container\n");
      result= false;
   }
   return result;
}

static bool getDemux( AppCtx *ctx )
{
   bool result= true;
   if ( ctx->demuxName )
   {
      ctx->demux= gst_element_factory_make( ctx->demuxName, "demux" );
   }
   else
   {
      switch( ctx->containerType )
      {
         case Container_ts:
            ctx->factoryListDemux= gst_element_factory_list_get_elements( GST_ELEMENT_FACTORY_TYPE_DEMUXER, GST_RANK_NONE );
            if ( ctx->factoryListDemux )
            {
               GstPad *audioSinkPad= 0;
               GstCaps *audioSinkCaps= 0;
               GList *iter;

               audioSinkPad= gst_element_get_static_pad( ctx->audiosink, "sink" );
               if ( audioSinkPad )
               {
                  audioSinkCaps= gst_pad_get_pad_template_caps( audioSinkPad );
                  if (audioSinkCaps )
                  {
                     iter= ctx->factoryListDemux;
                     for( iter= ctx->factoryListDemux; iter != NULL; iter= iter->next )
                     {
                        GstElementFactory *factory= (GstElementFactory*)iter->data;
                        if ( factory )
                        {
                           if ( gst_element_factory_can_src_any_caps( factory, audioSinkCaps ) )
                           {
                              if ( gst_element_factory_can_sink_any_caps( factory, ctx->capsContainer ) )
                              {
                                 g_print("factory %p can produce a compatible demux\n", factory);

                                 if ( ctx->verbose )
                                 {
                                    gchar **keys= gst_element_factory_get_metadata_keys( factory );
                                    int i= 0;
                                    while( keys[i] )
                                    {
                                       gchar *key= keys[i++];
                                       if ( key ) g_print("  %s: %s\n", key, gst_element_factory_get_metadata( factory, key ) );
                                    }
                                    g_strfreev(keys);
                                    g_print("\n");
                                 }

                                 ctx->demux= gst_element_factory_create( factory, "demux" );
                                 break;
                              }
                           }
                        }
                     }
                  }
                  else
                  {
                     g_print("Error: unable to get audiosink sink pad template caps\n" );
                  }
               }
               else
               {
                  g_print("Error: unable to get audiosink sink pad\n");
               }

               if ( audioSinkCaps )
               {
                  gst_caps_unref( audioSinkCaps );
                  audioSinkCaps= 0;
               }
               
               if ( audioSinkPad )
               {
                  gst_object_unref( audioSinkPad );
                  audioSinkPad= 0;
               }
            }
            else
            {
               g_print("Error: unable to get factory list\n");
            }
            break;
         case Container_mp4:
            ctx->demux= gst_element_factory_make( "qtdemux", "demux" );
            break;
         case Container_wav:
            // The 'wavparse' element could be used as the wav demux,
            // but we prefer passing unparsed wav data to audsrvsink
            ctx->demux= gst_element_factory_make( "identity", "demux" );
            break;
         default:
            break;
      }
   }
   if ( ctx->demux )
   {
      gst_object_ref( ctx->demux );
   }
   else
   {
      if ( ctx->demuxName )
      {
         g_print("Error: unable to create demux instance (%s)\n", ctx->demuxName );
      }
      else
      {
         g_print("Error: unable to create demux instance\n" );
      }
      result= false;
   }
   return result;
}

static bool getParser( AppCtx *ctx )
{
   bool result= false;
   bool needParser= false;
   switch( ctx->containerType )
   {
      case Container_ts:
         // No parser needed
         result= true;
         break;
      case Container_mp4:
         needParser= true;
         break;
      case Container_wav:
         // No parser needed
         result= true;
         break;
      default:
         break;
   }
   if ( needParser )
   {
      ctx->factoryListParser= gst_element_factory_list_get_elements( GST_ELEMENT_FACTORY_TYPE_PARSER|GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO, 
                                                                     GST_RANK_NONE );
      if ( ctx->factoryListParser )
      {
         GstPad *audioSinkPad= 0;
         GstCaps *audioSinkCaps= 0;
         GstPadTemplate *demuxSrcPadTemplate= 0;
         GstCaps *demuxSrcCaps= 0;
         GList *iter;

         audioSinkPad= gst_element_get_static_pad( ctx->audiosink, "sink" );
         if ( audioSinkPad )
         {
            audioSinkCaps= gst_pad_get_pad_template_caps( audioSinkPad );
            if (audioSinkCaps )
            {
               demuxSrcPadTemplate= gst_element_class_get_pad_template( GST_ELEMENT_GET_CLASS(ctx->demux), "audio_%u" );
               if ( !demuxSrcPadTemplate )
               {
                  demuxSrcPadTemplate= gst_element_class_get_pad_template( GST_ELEMENT_GET_CLASS(ctx->demux), "src" );
               }
               if ( demuxSrcPadTemplate )
               {
                  demuxSrcCaps= gst_pad_template_get_caps( demuxSrcPadTemplate );
                  if (demuxSrcCaps )
                  {
                     iter= ctx->factoryListParser;
                     for( iter= ctx->factoryListParser; iter != NULL; iter= iter->next )
                     {
                        GstElementFactory *factory= (GstElementFactory*)iter->data;
                        if ( factory )
                        {
                           if ( gst_element_factory_can_src_any_caps( factory, audioSinkCaps ) )
                           {
                              if ( gst_element_factory_can_sink_any_caps( factory, demuxSrcCaps ) )
                              {
                                 g_print("factory %p can produce a compatible parser\n", factory);

                                 if ( ctx->verbose )
                                 {
                                    gchar **keys= gst_element_factory_get_metadata_keys( factory );
                                    int i= 0;
                                    while( keys[i] )
                                    {
                                       gchar *key= keys[i++];
                                       if ( key ) g_print("  %s: %s\n", key, gst_element_factory_get_metadata( factory, key ) );
                                    }
                                    g_strfreev(keys);
                                    g_print("\n");
                                 }

                                 ctx->parser= gst_element_factory_create( factory, "parser" );
                                 break;
                              }
                           }
                        }
                     }
                  }
                  else
                  {
                     g_print("Error: unable to get demux src pad template caps\n" );
                  }
               }
               else
               {
                  g_print("Error: unable to get demux src pad\n");
               }
            }
            else
            {
               g_print("Error: unable to get audiosink sink pad template caps\n" );
            }
         }
         else
         {
            g_print("Error: unable to get audiosink sink pad\n");
         }

         if ( demuxSrcCaps )
         {
            gst_caps_unref( demuxSrcCaps );
            demuxSrcCaps= 0;
         }

         if ( audioSinkCaps )
         {
            gst_caps_unref( audioSinkCaps );
            audioSinkCaps= 0;
         }
         
         if ( audioSinkPad )
         {
            gst_object_unref( audioSinkPad );
            audioSinkPad= 0;
         }
      }
   }

   if ( ctx->parser )
   {
      gst_object_ref( ctx->parser );
      result= true;
   }
   else if ( needParser )
   {
      g_print("Error: unable to create parser\n");
   }

   return result;
}

static bool createPipeline( AppCtx *ctx )
{
   bool result= false;
   int argc= 0;
   char **argv= 0;

   gst_init( &argc, &argv );

   ctx->pipeline= gst_pipeline_new("pipeline");
   if ( !ctx->pipeline )
   {
      g_print("Error: unable to create pipeline instance\n" );
      goto exit;
   }

   ctx->bus= gst_pipeline_get_bus( GST_PIPELINE(ctx->pipeline) );
   if ( !ctx->bus )
   {
      g_print("Error: unable to get pipeline bus\n");
      goto exit;
   }
   gst_bus_add_watch( ctx->bus, busCallback, ctx );
   
   ctx->appsrc= gst_element_factory_make( "appsrc", "appsrc" );
   if ( !ctx->appsrc )
   {
      g_print("Error: unable to create appsrc instance\n" );
      goto exit;
   }
   gst_object_ref( ctx->appsrc );

   ctx->queue= gst_element_factory_make( "queue", "queue" );
   if ( !ctx->queue )
   {
      g_print("Error: unable to create queue instance\n" );
      goto exit;
   }
   gst_object_ref( ctx->queue );

   ctx->audiosink= gst_element_factory_make( "audsrvsink", "audiosink" );
   if ( !ctx->audiosink )
   {
      g_print("Error: unable to create audiosink instance\n" );
      goto exit;
   }
   gst_object_ref( ctx->audiosink );

   if ( !getContainerType( ctx ) )
   {
      g_print("Error: unsupported media container: (%s)\n", ctx->clipFilename );
      goto exit;
   }

   if ( !getContainerCaps( ctx ) )
   {
      g_print("Error: unable to get caps for container\n");
      goto exit;
   }

   if ( !getDemux( ctx ) )
   {
      g_print("Error: unable to get a workable demux\n");
      goto exit;
   }

   if ( !getParser( ctx ) )
   {
      g_print("Error: unable to get a workable parser\n");
      goto exit;
   }

   gst_app_src_set_caps( GST_APP_SRC(ctx->appsrc), ctx->capsContainer );

   gst_bin_add_many( GST_BIN(ctx->pipeline), 
                     ctx->appsrc,
                     ctx->queue,
                     ctx->demux,
                     ctx->audiosink,
                     NULL
                   );
   if ( ctx->parser )
   {
      gst_bin_add_many( GST_BIN(ctx->pipeline), 
                        ctx->parser,
                        NULL );
   }


   if ( !gst_element_link( ctx->appsrc, ctx->queue ) )
   {
      g_print("Error: unable to link appsrc and queue\n");
      goto exit;
   }

   if ( !gst_element_link_filtered( ctx->queue, ctx->demux, ctx->capsContainer ) )
   {
      g_print("Error: unable to link queue to demux\n");
      goto exit;
   }

   gst_caps_unref( ctx->capsContainer );
   ctx->capsContainer= 0;

   g_object_set( G_OBJECT(ctx->audiosink), "session", ctx->audsrvPlayback, NULL );   
   g_object_set( G_OBJECT(ctx->audiosink), "audio-pts-offset", 4500, NULL );

   switch( ctx->containerType )
   {
      case Container_ts:
      case Container_mp4:
         g_signal_connect(ctx->demux, "pad-added", G_CALLBACK(demuxSrcPadAdded), ctx);
         break;
      case Container_wav:
         if ( !gst_element_link( ctx->demux, ctx->audiosink ) )
         {
            g_print("Error: unable to link demux and audiosink\n");
            goto exit;
         }
         break;
      default:
         break;
   }

   result= true;
   
exit:
   
   if ( ctx->factoryListDemux )
   {
      gst_plugin_feature_list_free( ctx->factoryListDemux );
      ctx->factoryListDemux= 0;
   }

   if ( ctx->factoryListParser )
   {
      gst_plugin_feature_list_free( ctx->factoryListParser );
      ctx->factoryListParser= 0;
   }

   return result;
}

static void destroyPipeline( AppCtx *ctx )
{
   if ( ctx->pipeline )
   {
      gst_element_set_state(ctx->pipeline, GST_STATE_NULL);
   }
   if ( ctx->audiosink )
   {
      gst_object_unref( ctx->audiosink );
      ctx->audiosink= 0;
   }
   if ( ctx->appsrc )
   {
      gst_object_unref( ctx->appsrc );
      ctx->appsrc= 0;
   }
   if ( ctx->queue )
   {
      gst_object_unref( ctx->queue );
      ctx->queue= 0;
   }
   if ( ctx->parser )
   {
      gst_object_unref( ctx->parser );
      ctx->parser= 0;
   }
   if ( ctx->demux )
   {
      gst_object_unref( ctx->demux );
      ctx->demux= 0;
   }
   if ( ctx->capsContainer )
   {
      gst_caps_unref( ctx->capsContainer );
      ctx->capsContainer= 0;
   }      
   if ( ctx->bus )
   {
      gst_object_unref( ctx->bus );
      ctx->bus= 0;
   }
   if ( ctx->pipeline )
   {
      gst_object_unref( GST_OBJECT(ctx->pipeline) );
      ctx->pipeline= 0;
   }
}

static void pushData( AppCtx *ctx, char *data, int dataSize )
{
   GstBuffer *buffer= 0;
   GstFlowReturn ret;

   buffer= gst_buffer_new_wrapped( data, dataSize );
   if ( !buffer )
   {
      g_print("Error: unable to allocate gst buffer\n");
      g_free( data );
      goto exit;
   }
   
   ret= gst_app_src_push_buffer( GST_APP_SRC(ctx->appsrc), buffer );
   
exit:
   return;
}

static void playClip( AppCtx *ctx )
{
   FILE *pFile= 0;
   int lenToRead, lenDidRead;
   bool eof= false;
   int bufferSize= 32*1024;
   char *buffer= 0;
      
   pFile= fopen( ctx->clipFilename, "rb" );
   if ( pFile )
   {
      if ( GST_STATE_CHANGE_FAILURE != gst_element_set_state(ctx->pipeline, GST_STATE_PLAYING) )
      {
         ctx->eos= false;
         while( !ctx->eos && !g_quit )
         {
            if ( !eof )
            {
               buffer= (char*)malloc( bufferSize );
               if ( !buffer )
               {
                  g_print("Error: unable to allocate audio buffer size %d\n", bufferSize );
                  break;
               }
               lenToRead= bufferSize;
               lenDidRead= fread( buffer, 1, lenToRead, pFile );
               if ( lenDidRead )
               {
                  pushData( ctx, buffer, lenDidRead );
                  buffer= 0;
               }
               else
               {
                  gst_app_src_end_of_stream( GST_APP_SRC(ctx->appsrc) );
                  eof= true;
               }
            }
            else
            {
               usleep( 10000 );
            }
            processInput( ctx );
            g_main_context_iteration( NULL, FALSE );
         }
         gst_element_set_state(ctx->pipeline, GST_STATE_READY );
         if ( ctx->takeDemuxToNull )
         {
            gst_element_set_state(ctx->demux, GST_STATE_NULL );
            gst_element_set_state(ctx->demux, GST_STATE_READY );
         }
      }
      fclose( pFile );
   }
}

static void stopClip( AppCtx *ctx )
{
   ctx->eos= true;
}

static void sessionStatusCallback( void *userData, int result, AudSrvSessionStatus *sessionStatus )
{
   AppCtx *ctx= (AppCtx*)userData;

   g_print("getstatus: result %d\n", result);
   g_print( "  global: muted %d volume %f\n", sessionStatus->globalMuted, sessionStatus->globalVolume );
   if ( result == 0 )
   {
      g_print( "  session (%s): ready %d playing %d muted %d volume %f\n", 
               sessionStatus->sessionName, sessionStatus->ready, sessionStatus->playing, sessionStatus->muted, sessionStatus->volume );
      if ( ctx )
      {
         ctx->muted= ctx->global ? sessionStatus->globalMuted : sessionStatus->muted;
         ctx->volume= ctx->global ? sessionStatus->globalVolume : sessionStatus->volume;
      }
   }
}

static void getStatus( AppCtx *ctx )
{
   if ( ctx->connected )
   {
      AudioServerGetSessionStatus( ctx->audsrvPlayback, sessionStatusCallback, ctx );
   }
   else if ( ctx->sessionNameAttached )
   {
      AudioServerGetSessionStatus( ctx->audsrvObserver, sessionStatusCallback, ctx );
   }
}

static void showKeyHelp()
{
   g_print("Commands:\n");
   g_print(" n - toggle connect\n" );
   g_print(" p - play clip\n");
   g_print(" s - get status\n");
   g_print(" a - toggle attach\n");
   g_print(" g - toggle global (mute/volume)\n" );
   g_print(" e - enumerate sessions\n");
   g_print(" v - toggle session event registration\n");
   g_print(" up - volume up\n" );
   g_print(" down - volume up\n" );
   g_print(" m - toggle mute\n" );
   g_print(" 1 - use clip1 (wav: (pcm) wow effect)\n" );
   g_print(" 2 - use clip2 (wav (mp3): cash register sound)\n" );
   //g_print(" 3 - use clip3 (ts: short tone)\n" );
   //g_print(" 4 - use clip4 (ts: trumpet fanfair)\n" );
   //g_print(" 5 - use clip5 (mp4: nature)\n" );
   g_print(" t - toggle session type\n" );
   g_print(" h - toggle private\n" );
   g_print(" c - toggle capture\n" );
   g_print(" ? - show help\n" );
   g_print(" q  - quit\n" );
}

static void processInput( AppCtx *ctx )
{
   if ( keyHit() )
   {
      int key= fgetc(stdin);
      
      switch( key )
      {
         case 'q':
         case 'Q':
            g_quit= true;
            break;

         case 'n':
            toggleConnect(ctx);
            break;

         case 'p':
            if ( ctx->connected )
            {
               if ( ctx->clipPlaying )
               {
                  stopClip( ctx );
               }
               else
               {
                  ctx->clipPlaying= true;
                  g_print("playing clip\n");
                  playClip( ctx );
                  g_print("done playing clip\n");
                  ctx->clipPlaying= false;
               }
            }
            break;

         case 's':
            getStatus( ctx );
            break;

         case 'e':
            g_print("enumerating sessions\n");
            enumerateSessions(ctx);
            break;

         case 'v':
            toggleSessionEvent(ctx);
            break;

         case 'a':
            toggleAttach(ctx);
            break;

         case 'g':
            if ( !ctx->audsrvObserver )
            {
               const char *serverName= getenv("AUDSRV_NAME");
               ctx->audsrvObserver= AudioServerConnect( serverName );
            }
            if ( ctx->audsrvObserver )
            {
               ctx->global= !ctx->global;
            }
            g_print("%s\n", (ctx->global ? "global ctrl" : "session ctrl") );
            break;

         case '1':
            g_print("switching to clip1\n");
            ctx->clipFilename= "/usr/share/audioserver/clip1.wav";
            if ( ctx->connected )
            {
               destroyPipeline( ctx );
               createPipeline( ctx );
            }
            break;
            
         case '2':
            g_print("switching to clip2\n");
            ctx->clipFilename= "/usr/share/audioserver/clip2.mp3";
            if ( ctx->connected )
            {
               destroyPipeline( ctx );
               createPipeline( ctx );
            }
            break;
            
         /*case '3':
            g_print("switching to clip3\n");
            ctx->clipFilename= "/usr/share/audioserver/clip3.ts";
            if ( ctx->connected )
            {
               destroyPipeline( ctx );
               createPipeline( ctx );
            }
            break;
            
         case '4':
            g_print("switching to clip4\n");
            ctx->clipFilename= "/usr/share/audioserver/clip4.ts";
            if ( ctx->connected )
            {
               destroyPipeline( ctx );
               createPipeline( ctx );
            }
            break;
            
          case '5':
            g_print("switching to clip5\n");
            ctx->clipFilename= "/usr/share/audioserver/clip5.mp4";
            if ( ctx->connected )
            {
               destroyPipeline( ctx );
               createPipeline( ctx );
            }
            break;
           */
         case '?':
            showKeyHelp();
            break;
            
         case 0x1B:
            key= fgetc(stdin);
            if ( key == 0x5B )
            {
               key= fgetc(stdin);
               switch( key )
               {
                  case 0x41: // Up
                     volumeUp(ctx);
                     break;
                  case 0x42: // Down
                     volumeDown(ctx);
                     break;
                  case 0x43: // Right
                  case 0x44: // Left
                  default:
                     break;
               }
            }
            break;

         case 'm':
            toggleMute(ctx);
            break;

         case 'h':
            togglePrivate(ctx);
            break;

         case 't':
            toggleSessionType(ctx);
            break;

         case 'c':
            toggleCapture(ctx);
            break;

         default:
            break;
      }
      
      tcflush(STDIN_FILENO,TCIFLUSH);
   }
}

static void volumeUp( AppCtx *ctx )
{
   if ( ctx->global || ctx->connected || ctx->sessionNameAttached )
   {
      float newVolume;
      
      newVolume= ctx->volume + 0.1;
      if ( newVolume > 1.0 ) newVolume= 1.0;
      
      if ( ctx->global )
      {
         AudioServerGlobalVolume( ctx->audsrvObserver, newVolume );
      }
      else
      if ( ctx->connected )
      {            
         g_object_set( G_OBJECT(ctx->audiosink), "volume", newVolume, NULL );
      }
      else
      {
         AudioServerVolume( ctx->audsrvObserver, newVolume );
      }     
      
      ctx->volume= newVolume;
      
      g_print( "volume %f\n", ctx->volume );
   }
}


static void volumeDown( AppCtx *ctx )
{
   if ( ctx->global || ctx->connected || ctx->sessionNameAttached )
   {
      float newVolume;
      
      newVolume= ctx->volume - 0.1;
      if ( newVolume < 0.0 ) newVolume= 0.0;
      
      if ( ctx->global )
      {
         AudioServerGlobalVolume( ctx->audsrvObserver, newVolume );
      }
      else
      if ( ctx->connected )
      {            
         g_object_set( G_OBJECT(ctx->audiosink), "volume", newVolume, NULL );
      }
      else
      {
         AudioServerVolume( ctx->audsrvObserver, newVolume );
      }     
      
      ctx->volume= newVolume;
      
      g_print( "volume %f\n", ctx->volume );
   }
}

static void toggleConnect( AppCtx *ctx )
{
   bool success;
   if ( ctx->connected )
   {
      g_print("disconnecting...\n");
      if ( ctx->audsrvCapture )
      {
         AudioServerDisconnect( ctx->audsrvCapture );
         ctx->audsrvCapture= 0;
      }

      destroyPipeline( ctx );

      if ( ctx->audsrvPlayback )
      {
         AudioServerDisconnect( ctx->audsrvPlayback );
         ctx->audsrvPlayback= 0;
      }

      ctx->connected= false;
   }
   else
   {
      g_print("connecting...\n");

      const char *serverName= getenv("AUDSRV_NAME");
      ctx->audsrvPlayback= AudioServerConnect( serverName );
      if ( ctx->audsrvPlayback )
      {
         if ( AudioServerInitSession( ctx->audsrvPlayback, ctx->sessionType, ctx->isPrivate, ctx->sessionName ) )
         {
            AudioServerVolume( ctx->audsrvPlayback, ctx->volume);

            AudioServerMute( ctx->audsrvPlayback, ctx->muted);

            success= createPipeline( ctx );
            if ( success )
            {
               if ( GST_STATE_CHANGE_FAILURE != gst_element_set_state(ctx->pipeline, GST_STATE_READY) )
               {
                  g_print("pipeline created\n");
                  ctx->connected= true;
               }
               else
               {
                  destroyPipeline( ctx );
                  AudioServerDisconnect( ctx->audsrvPlayback );
                  ctx->audsrvPlayback= 0;
               }
            }
         }
      }
   }
   if ( ctx->connected )
   {
      g_print("connected\n");
   }
   else
   {
      g_print("not connected\n");
   }
}

static void toggleMute( AppCtx *ctx )
{
   if ( ctx->global || ctx->connected || ctx->sessionNameAttached )
   {
      ctx->muted= !ctx->muted;

      if ( ctx->global )
      {
         AudioServerGlobalMute( ctx->audsrvObserver, ctx->muted );
      }
      else
      if ( ctx->connected )
      {            
         g_object_set( G_OBJECT(ctx->audiosink), "mute", ctx->muted, NULL );
      }
      else
      {
         AudioServerMute( ctx->audsrvObserver, ctx->muted );
      }

      g_print( "%s %s\n", (ctx->global ? "global" : "session"), (ctx->muted ? "muted" : "unmuted" ) );
   }
}

static void toggleSessionType( AppCtx *ctx )
{
   switch( ctx->sessionType )
   {
      case AUDSRV_SESSION_Primary:
         ctx->sessionType= AUDSRV_SESSION_Secondary;
         g_print("secondary\n");
         break;
      case AUDSRV_SESSION_Secondary:
         ctx->sessionType= AUDSRV_SESSION_Effect;
         g_print("effect\n");
         break;
      default:
      case AUDSRV_SESSION_Observer:
      case AUDSRV_SESSION_Effect:
         ctx->sessionType= AUDSRV_SESSION_Primary;
         g_print("primary\n");
         break;
   }
}

static void togglePrivate( AppCtx *ctx )
{
   ctx->isPrivate= !ctx->isPrivate;

   g_print( "%s\n", (ctx->isPrivate ? "private" : "public") );
}

static void toggleCapture( AppCtx *ctx )
{
   if ( ctx->capture )
   {
      if ( ctx->audsrvCapture )
      {
         AudioServerStopCapture( ctx->audsrvCapture, captureDoneCallback, ctx );
      }
      ctx->capture= false;
   }
   else
   {
      if ( !ctx->audsrvCapture )
      {
         const char *serverName= getenv("AUDSRV_NAME");
         ctx->audsrvCapture= AudioServerConnect( serverName );
         if ( ctx->audsrvCapture )
         {
            if ( !AudioServerInitSession( ctx->audsrvCapture, AUDSRV_SESSION_Capture, ctx->isPrivate, ctx->sessionName ) )
            {
               AudioServerDisconnect( ctx->audsrvCapture );
               ctx->audsrvCapture= 0;
            }
         }
      }
      if ( ctx->audsrvCapture )
      {
         ctx->captureByteCount= 0;         
         if ( AudioServerStartCapture( ctx->audsrvCapture, ctx->sessionNameToCapture, captureCallback, NULL, ctx ) )
         {
            g_print("capture started...\n");
            ctx->capture= true;
         }
         else
         {
            AudioServerDisconnect( ctx->audsrvCapture );
            ctx->audsrvCapture= 0;
         }
      }
   }
}

static void captureCallback( void *userData, AudSrvCaptureParameters *params, unsigned char *data, int datalen )
{
   AppCtx *ctx= (AppCtx*)userData;
   
   if ( ctx )
   {
      ctx->captureParams= *params;
      if ( ctx->captureToFile )
      {
         if ( !ctx->pCaptureFile )
         {
            ctx->pCaptureFile= fopen( "/opt/audioserver-test-capture.wav", "wb" );
            if ( ctx->pCaptureFile )
            {
               waveWriteHeader( ctx );
            }
         }
         if ( ctx->pCaptureFile && data && datalen )
         {
            waveWriteData( ctx, data, datalen );
            ctx->captureByteCount += datalen;
         }
      }
      else
      {
         if ( ctx->captureByteCount+datalen > ctx->captureBufferCapacity )
         {
            int newCapacity= ctx->captureBufferCapacity+CAPTURE_BUFFER_SIZE;
            unsigned char *newBuffer= (unsigned char*)malloc( newCapacity );
            if ( newBuffer )
            {
               if ( ctx->captureBuffer )
               {
                  memcpy( newBuffer, ctx->captureBuffer, ctx->captureByteCount );
                  free( ctx->captureBuffer );
               }
               ctx->captureBuffer= newBuffer;
               ctx->captureBufferCapacity= newCapacity;
            }
         }
         if ( ctx->captureBuffer && (ctx->captureByteCount+datalen <= ctx->captureBufferCapacity) )
         {
            memcpy( ctx->captureBuffer+ctx->captureByteCount, data, datalen );
            ctx->captureByteCount += datalen;
         }   
      }
   }
}

static void captureDoneCallback( void *userData )
{
   AppCtx *ctx= (AppCtx*)userData;
   
   if ( ctx )
   {
      g_print("capture done\n");
      if ( ctx->captureToFile )
      {
         if ( ctx->pCaptureFile )
         {
            waveFinish( ctx );
            fclose( ctx->pCaptureFile );
            ctx->pCaptureFile= 0;
         }
      }
      else
      {
         ctx->pCaptureFile= fopen( "/opt/audioserver-test-capture.wav", "wb" );
         if ( ctx->pCaptureFile )
         {
            waveWriteHeader( ctx );
            if ( ctx->captureBuffer && ctx->captureByteCount )
            {
               waveWriteData( ctx, ctx->captureBuffer, ctx->captureBufferCapacity );
               free( ctx->captureBuffer );
               ctx->captureBuffer= 0;
               ctx->captureBufferCapacity= 0;
            }
            waveFinish( ctx );
            fclose( ctx->pCaptureFile );
            ctx->pCaptureFile= 0;
         }
      }
      ctx->capture= false;
   }
}

static void enumerateSessions( AppCtx *ctx )
{
   if ( !ctx->audsrvObserver )
   {
      const char *serverName= getenv("AUDSRV_NAME");
      ctx->audsrvObserver= AudioServerConnect( serverName );
   }
   if ( ctx->audsrvObserver )
   {
      if ( ctx->enumResults )
      {
         free( ctx->enumResults );
         ctx->enumResults= 0;
      }
      ctx->enumCount= 0;
      bool result= AudioServerEnumerateSessions( ctx->audsrvObserver, enumerateSessionsCallback, ctx );
      if ( !result )
      {
         g_print("enumeration failed\n");
      }
   }
}

static void enumerateSessionsCallback( void *userData, int result, int count, AudSrvSessionInfo *sessionInfo )
{
   AppCtx *ctx= (AppCtx*)userData;
   g_print("enum sessions: result %d count %d\n", result, count );
   if ( result == 0 )
   {
      ctx->enumResults= (AudSrvSessionInfo*)malloc( count*sizeof(AudSrvSessionInfo) );
      for( int i= 0; i < count; ++i )
      {
         g_print(" %d: pid %d type %d name(%s)\n", i, sessionInfo[i].pid, sessionInfo[i].sessionType, sessionInfo[i].sessionName );
         if ( ctx->enumResults )
         {
            ctx->enumResults[i]= sessionInfo[i];            
         }
      }
      if ( ctx->enumResults )
      {
         ctx->enumCount= count;
      }
      g_print("\n");
   }
}

static void toggleAttach( AppCtx *ctx )
{
   if ( ctx->sessionNameAttached )
   {
      AudioServerSessionDetach( ctx->audsrvObserver );
      free( ctx->sessionNameAttached );
      ctx->sessionNameAttached= 0;
      g_print("detached\n");
   }
   else
   {
      if ( ctx->enumCount )
      {
         bool done= false;

         g_print(" 0: cancel\n");
         for( int i= 0; i < ctx->enumCount; ++i )
         {
            g_print(" %d: pid %d type %d name(%s)\n", i+1, ctx->enumResults[i].pid, ctx->enumResults[i].sessionType, ctx->enumResults[i].sessionName );
         }
         g_print("\n: ");

         g_print("select (1-%d) or 0 to cancel\n: ", ctx->enumCount); 
         while( !done )
         {
            if ( keyHit() )
            {
               int key= fgetc(stdin);

               g_print("%c\n", key);            
               switch( key )
               {
                  case '0':
                     done= true;
                     break;
                  default:
                     if ( (key >= '1') && (key <= '9') )
                     {
                        int attachIdx= key-'0'-1;
                        if ( attachIdx < ctx->enumCount )
                        {
                           ctx->sessionNameAttached= strdup( ctx->enumResults[attachIdx].sessionName );
                           if ( ctx->sessionNameAttached )
                           {
                              g_print("Attach to session (%s)\n", ctx->sessionNameAttached );
                              AudioServerSessionAttach( ctx->audsrvObserver, ctx->sessionNameAttached );
                           }
                           done= true;
                        }
                        else
                        {
                           g_print("select (1-%d) or 0 to cancel\n: ", ctx->enumCount); 
                        }
                     }
                     break;
               }
            }
            else
            {
               usleep( 10000 );
            }
         }
      }
      else
      {
         g_print("Do enum first\n");
      }
   }
}

static void sessionEventCallback( void *userData, int event, AudSrvSessionInfo *sessionInfo )
{
   const char *eventName;
   switch ( event )
   {
      case AUDSRV_SESSIONEVENT_Removed:
         eventName= "removed";
         break;
      case AUDSRV_SESSIONEVENT_Added:
         eventName= "added";
         break;
      default:
         eventName= "unknown";
         break;
   }
   g_print("event %s: pid %d type %d name(%s)\n", eventName, sessionInfo->pid, sessionInfo->sessionType, sessionInfo->sessionName );
}

static void toggleSessionEvent( AppCtx *ctx )
{
   if ( ctx->haveSessionEvents )
   {
      g_print("unregister for session events\n");
      ctx->haveSessionEvents= false;
      AudioServerDisableSessionEvent( ctx->audsrvObserver );      
   }
   else
   {
      if ( !ctx->audsrvObserver )
      {
         const char *serverName= getenv("AUDSRV_NAME");
         ctx->audsrvObserver= AudioServerConnect( serverName );
      }
      if ( ctx->audsrvObserver )
      {
         if ( AudioServerEnableSessionEvent( ctx->audsrvObserver, sessionEventCallback, ctx ) )
         {
            ctx->haveSessionEvents= true;
         }
      }
      if ( ctx->haveSessionEvents )
      {
         g_print("register for session events\n");
      }
      else
      {
         g_print("failed to register for session events\n");
      }
   }
}

static bool writeLE16( FILE *pFile, unsigned n )
{
   unsigned char d[2];
   
   d[0]= ((n)&0xFF);
   d[1]= ((n>>8)&0xFF);
   
   if ( fwrite( d, 1, 2, pFile ) != 2 )
   {
      return false;
   }
   return true;
}

static bool writeLE32( FILE *pFile, unsigned n )
{
   unsigned char d[4];
   
   d[0]= ((n)&0xFF);
   d[1]= ((n>>8)&0xFF);
   d[2]= ((n>>16)&0xFF);
   d[3]= ((n>>24)&0xFF);
   
   if ( fwrite( d, 1, 4, pFile ) != 4 )
   {
      return false;
   }
   return true;
}

static void waveWriteHeader( AppCtx *ctx )
{
   int lenDidWrite;
   
   if ( ctx && ctx->pCaptureFile )
   {
      // ChunkID
      lenDidWrite= fwrite( "RIFF", 1, 4, ctx->pCaptureFile );
      if ( lenDidWrite != 4 )
      {
         printf("waveWriteHeader: line %d errno %d\n", __LINE__, errno );
         goto exit;
      }
      // ChunkSize
      if ( !writeLE32( ctx->pCaptureFile, 44 ) )
      {
         printf("waveWriteHeader: line %d errno %d\n", __LINE__, errno );
         goto exit;
      }
      // Format
      lenDidWrite= fwrite( "WAVE", 1, 4, ctx->pCaptureFile );
      if ( lenDidWrite != 4 )
      {
         printf("waveWriteHeader: line %d errno %d\n", __LINE__, errno );
         goto exit;
      }
      // Subchunk1ID
      lenDidWrite= fwrite( "fmt ", 1, 4, ctx->pCaptureFile );
      if ( lenDidWrite != 4 )
      {
         printf("waveWriteHeader: line %d errno %d\n", __LINE__, errno );
         goto exit;
      }
      // Subchunk1Size
      if ( !writeLE32( ctx->pCaptureFile, 16 ) )
      {
         printf("waveWriteHeader: line %d errno %d\n", __LINE__, errno );
         goto exit;
      }
      // Format PCM
      if ( !writeLE16( ctx->pCaptureFile, 1 ) )
      {
         printf("waveWriteHeader: line %d errno %d\n", __LINE__, errno );
         goto exit;
      }
      // NumChannels
      if ( !writeLE16( ctx->pCaptureFile, ctx->captureParams.numChannels ) )
      {
         printf("waveWriteHeader: line %d errno %d\n", __LINE__, errno );
         goto exit;
      }
      // SampleRate
      if ( !writeLE32( ctx->pCaptureFile, ctx->captureParams.sampleRate ) )
      {
         printf("waveWriteHeader: line %d errno %d\n", __LINE__, errno );
         goto exit;
      }
      // ByteRate
      if ( !writeLE32( ctx->pCaptureFile, 
                       ctx->captureParams.sampleRate*
                       ctx->captureParams.numChannels*
                       (ctx->captureParams.bitsPerSample/8) ) )
      {
         printf("waveWriteHeader: line %d errno %d\n", __LINE__, errno );
         goto exit;
      }
      // BlockAlign
      if ( !writeLE16( ctx->pCaptureFile, ctx->captureParams.numChannels*(ctx->captureParams.bitsPerSample/8) ) )
      {
         printf("waveWriteHeader: line %d errno %d\n", __LINE__, errno );
         goto exit;
      }
      // BitsPerSample
      if ( !writeLE16( ctx->pCaptureFile, ctx->captureParams.bitsPerSample ) )
      {
         printf("waveWriteHeader: line %d errno %d\n", __LINE__, errno );
         goto exit;
      }
      // Subchunk2ID
      lenDidWrite= fwrite( "data", 1, 4, ctx->pCaptureFile );
      if ( lenDidWrite != 4 )
      {
         printf("waveWriteHeader: line %d errno %d\n", __LINE__, errno );
         goto exit;
      }
      // Subchunk2Size
      if ( !writeLE32( ctx->pCaptureFile, 0 ) )
      {
         printf("waveWriteHeader: line %d errno %d\n", __LINE__, errno );
         goto exit;
      }
   }

exit:
   return;
}

static void waveWriteData( AppCtx *ctx, unsigned char *data, int datalen )
{
   int lenToWrite, lenDidWrite;
   
   if ( ctx && ctx->pCaptureFile )
   {
      lenToWrite= datalen;
      lenDidWrite= fwrite( data, 1, datalen, ctx->pCaptureFile );
      if ( lenDidWrite != lenToWrite )
      {
         printf("waveWriteData: line %d errno %d\n", __LINE__, errno );
      }
   }
}

static void waveFinish( AppCtx *ctx )
{
   int nc;
   if ( ctx && ctx->pCaptureFile )
   {
      nc= fseek( ctx->pCaptureFile, 4, SEEK_SET );
      if ( !nc )
      {
         if ( writeLE32( ctx->pCaptureFile, 44+ctx->captureByteCount ) )
         {
            nc= fseek( ctx->pCaptureFile, 40, SEEK_SET );
            if ( !nc )
            {
               if ( !writeLE32( ctx->pCaptureFile, ctx->captureByteCount ) )
               {
                  printf("waveFinish: line %d errno %d\n", __LINE__, errno );
               }
            }
         }
         else
         {
            printf("waveFinish: line %d errno %d\n", __LINE__, errno );
         }      
      }
   }
}

int main( int argc, char **argv )
{
   int result= -1;
   int argidx;
   const char *audioFilename= 0;
   const char *demuxName= 0;
   const char *sessionName= 0;
   const char *sessionNameToCapture= 0;
   bool asInit= false;
   bool takeDemuxToNull= false;
   bool success;
   bool verbose= false;
   bool captureToFile= false;
   struct sigaction sigAction;
   AppCtx *ctx= 0;

   printf("audioserver-test: v1.0\n\n" );
   
   argidx= 1;   
   while ( argidx < argc )
   {
      if ( argv[argidx][0] == '-' )
      {
         switch( argv[argidx][1] )
         {
            case '?':
               showUsage();
               goto exit;
            case '-':
               if ( !strcmp( argv[argidx], "--verbose" ) )
               {
                  verbose= true;
               }
               else if ( !strcmp( argv[argidx], "--capfile" ) )
               {
                  captureToFile= true;
               }
               else if ( !strcmp( argv[argidx], "--demux" ) )
               {
                  ++argidx;
                  
                  if ( argidx < argc )
                  {
                     demuxName= argv[argidx];
                     takeDemuxToNull= true;
                  }
               }
               else if ( !strcmp( argv[argidx], "--n" ) )
               {
                  ++argidx;
                  
                  if ( argidx < argc )
                  {
                     if ( !strcmp( argv[argidx], "true" ) )
                     {
                        takeDemuxToNull= true;
                     }
                     else
                     {
                        takeDemuxToNull= false;
                     }
                  }
               }
               else if ( !strcmp( argv[argidx], "--s" ) )
               {
                  ++argidx;

                  if ( argidx < argc )
                  {
                     sessionName= argv[argidx];
                  }
               }
               else if ( !strcmp( argv[argidx], "--c" ) )
               {
                  ++argidx;

                  if ( argidx < argc )
                  {
                     sessionNameToCapture= argv[argidx];
                  }
               }
               break;
            default:
               printf( "unknown option %s\n\n", argv[argidx] );
               exit( -1 );
               break;
         }
      }
      else
      {
         if ( !audioFilename )
         {
            audioFilename= argv[argidx];
         }
         else
         {
            printf( "ignoring extra argument: %s\n", argv[argidx] );
         }
      }
      
      ++argidx;
   }

   if ( !AudioServerInit() )
   {
      printf("Error: AudioServerInit failed\n");
      goto exit; 
   }
   asInit= true;

   ctx= (AppCtx*)calloc( 1, sizeof(AppCtx) );
   if ( !ctx )
   {
      printf("Error: unable to allocate application context\n");
      goto exit;
   }

   ctx->verbose= verbose;
   ctx->captureToFile= captureToFile;
   ctx->demuxName= demuxName;
   ctx->takeDemuxToNull= takeDemuxToNull;
   if ( !sessionName )
   {
      sessionName= "audioserver-test";
   }
   ctx->sessionName= sessionName;
   ctx->sessionNameToCapture= sessionNameToCapture;
   if ( !audioFilename )
   {
      audioFilename= "/usr/share/audioserver/clip1.wav";
   }
   ctx->clipFilename= audioFilename;
   ctx->containerType= Container_unknown;
   ctx->volume= 1.0;
   ctx->captureParams.version= (unsigned)-1;

   ctx->loop= g_main_loop_new(NULL,FALSE);
   
   if ( ctx->loop )
   {
      sigAction.sa_handler= signalHandler;
      sigemptyset(&sigAction.sa_mask);
      sigAction.sa_flags= SA_RESETHAND;
      sigaction(SIGINT, &sigAction, NULL);

      enableTTYBlocking( false );
      
      printf("Press '?' for help\n");
      
      while( !g_quit )
      {
         processInput( ctx );
         
         g_main_context_iteration( NULL, FALSE );

         usleep( 10000 );
      }
      
      enableTTYBlocking( true );
   }

   result= 0;
      
exit:

   g_print("audioserver-test exiting...\n");
   
   if ( ctx )
   {
      if ( ctx->enumResults )
      {
         free( ctx->enumResults );
         ctx->enumResults= 0;
      }

      if ( ctx->sessionNameAttached )
      {
         free( ctx->sessionNameAttached );
         ctx->sessionNameAttached= 0;
      }

      if ( ctx->audsrvObserver )
      {
         AudioServerDisconnect( ctx->audsrvObserver );
         ctx->audsrvObserver= 0;
      }

      if ( ctx->audsrvCapture )
      {
         AudioServerDisconnect( ctx->audsrvCapture );
         ctx->audsrvCapture= 0;
      }

      destroyPipeline( ctx );
      
      if ( ctx->loop )
      {
         g_main_loop_unref(ctx->loop);
         ctx->loop= 0;
      }
      
      free( ctx );
   }

   if ( asInit )
   {
      AudioServerTerm();
   }

   return result;   
}

