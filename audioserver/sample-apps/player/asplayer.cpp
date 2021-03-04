/*
 * If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2018 RDK Management
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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <gst/gst.h>
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>

#include "audioserver.h"

extern "C"
{
#ifdef USE_BT
#include "btmgr.h"
#include "btrCore.h"
#include "btrMgr_Types.h" //TBD: bluetooth needs to export this
#include "btrMgr_mediaTypes.h" //TBD: bluetooth needs to export this
#include "btrMgr_streamOut.h"
#endif
}

/* Note:
   need either -ms21 or -ms11 -persistent_audio in /lib/systemd/system/nxserver.service
   need to stop btmgr service while asplayer is running: systemctl stop btmgr
*/
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
   GstBus *bus;
   GstElement *httpsrc;
   GstElement *queue;
   GstElement *tee;
   GstElement *demux;
   GstElement *parserAudio;
   GstElement *videodecoder;
   GstElement *videosink;
   GstElement *audiosink;
   GstElement *demuxSAP;
   GstElement *audiosinkSAP;
   GMainLoop *loop;
   const char *demuxName;
   const char *languageMain;
   const char *languageSAP;
   const char *mediaURI;
   const char *btDeviceName;
   int containerType;
   GstCaps *capsContainer;
   bool eos;
   bool verbose;
   bool takeDemuxToNull;
   #ifdef USE_BT
   bool btDeviceConnected;
   bool btStarted;
   bool btError;
   pthread_t btThread;
   tBTRCoreHandle btCore;
   tBTRCoreDevId btDeviceId;
   enBTRCoreDeviceType btDeviceType;
   stBTRCoreDevMediaInfo btMediaInfo;
   stBTRMgrInASettings  btInSettings;
   stBTRMgrOutASettings btOutSettings;   
   tBTRMgrSoHdl btStream;
   int btDeviceFd;
   int btDeviceReadMTU;
   int btDeviceWriteMTU;
   #endif
   bool haveCaptureParams;
   AudSrv audsrvCapture;
   AudSrvCaptureParameters captureParams;
} AppCtx;

static void showUsage();
static void signalHandler(int signum);
#ifdef USE_BT
static eBTRMgrRet btStreamStatusCallback( stBTRMgrMediaStatus* apstBtrMgrSoStatus, void *userData);
static void* btThread( void *arg );
static bool btInit( AppCtx *ctx );
static void btTerm( AppCtx *ctx );
static bool btStart( AppCtx *ctx );
#endif
static bool captureInit( AppCtx *ctx );
static void captureTerm( AppCtx *ctx );
static void captureCallback( void *userData, AudSrvCaptureParameters *params, unsigned char *data, int datalen );
static bool getContainerType( AppCtx *ctx );
static bool getContainerCaps( AppCtx *ctx );
static bool getDemux( AppCtx *ctx, bool mainAudio= true );
static bool getAudioParser( AppCtx *ctx );
static bool getVideoDecoder( AppCtx *ctx, GstPad *demuxSrcPad );
static gboolean busCallback(GstBus *bus, GstMessage *message, gpointer data);
static void demuxSrcPadAdded( GstElement *element, GstPad *srcPad, gpointer data );
static bool createPipeline( AppCtx *ctx );
static void destroyPipeline( AppCtx *ctx );

static void showUsage()
{
   printf("usage:\n");
   printf(" asplayer [options] <uri>\n" );
   printf("  uri - URI of video asset to play\n" );
   printf("where [options] are:\n" );
   printf("  --lang <lang> : preferred audio language (eg. eng,spa, ...)\n");
   printf("  --sap <lang> : preferred SAP language (eg. eng,spa, ...)\n");
   #ifdef USE_BT
   printf("  --bt <devicename> : name of paired bluetooth device\n" );
   #endif
   printf("  --demux <demuxname> : demux element name\n" );
   printf("  --verbose : emit verbose logging\n" );
   printf("  -? : show usage\n" );
   printf("\n" );   
}

static bool g_quit= false;

static void signalHandler(int signum)
{
   printf("signalHandler: signum %d\n", signum);
	g_quit= true;
}

static long long getCurrentTimeMillis(void)
{
   struct timeval tv;
   long long utcCurrentTimeMillis;

   gettimeofday(&tv,0);
   utcCurrentTimeMillis= tv.tv_sec*1000LL+(tv.tv_usec/1000LL);

   return utcCurrentTimeMillis;
}

#ifdef USE_BT
static eBTRMgrRet btStreamStatusCallback( stBTRMgrMediaStatus* apstBtrMgrSoStatus, void *userData)
{
   eBTRMgrRet btmrc= eBTRMgrSuccess;
   AppCtx *ctx= (AppCtx*)userData;

   // Nothing to do

   return btmrc;
}

static enBTRCoreRet btDeviceStatusCallback( stBTRCoreDevStatusCBInfo* statusCB, void *userData )
{
   AppCtx *ctx= (AppCtx*)userData;

   if ( statusCB )
   {
      switch( statusCB->eDeviceCurrState )
      {
         case enBTRCoreDevStConnected:
            ctx->btDeviceConnected= true;
            printf("btDeviceStatusCallback: connected\n");
            break;
      }
   }

   return enBTRCoreSuccess;
}

static void* btAllocBtrMgrCodecInfo()
{
   void *info= 0;
   int sizePcm= sizeof(stBTRMgrPCMInfo);
   int sizeSbc= sizeof(stBTRMgrSBCInfo);
   int sizeMpeg= sizeof(stBTRMgrMPEGInfo);
   int size= sizePcm > sizeSbc ? sizePcm : sizeSbc;
   if ( sizeMpeg > size ) size= sizeMpeg;
   info= calloc( 1, size );
   return info;
}

static void* btThread( void *arg )
{
   AppCtx *ctx= (AppCtx*)arg;
   int deviceIdx= -1;
   enBTRCoreDeviceClass deviceClass;
   enBTRCoreRet btrc;
   eBTRMgrRet btmrc;
   stBTRCorePairedDevicesCount pairedDevices;
   stBTRMgrInASettings btInSettings;
   stBTRMgrOutASettings btOutSettings;
   long long now, limit;

   printf("btThread: enter\n");
   btrc= BTRCore_Init( &ctx->btCore );
   if ( btrc != enBTRCoreSuccess )
   {
      printf("Error: BTRCore_Init failed: %x\n", btrc );
      goto exit;
   }

   btrc= BTRCore_RegisterStatusCb( ctx->btCore, btDeviceStatusCallback, ctx );
   if ( btrc != enBTRCoreSuccess )
   {
      printf("Error: BTRCore_RegisterStatusCb failed: %x\n", btrc );
      goto exit;
   }

   memset( &pairedDevices, 0, sizeof(pairedDevices) );
   btrc= BTRCore_GetListOfPairedDevices( ctx->btCore, &pairedDevices );
   if ( btrc != enBTRCoreSuccess )
   {
      printf("Error: BTRCore_GetListOfPairedDevices failed: %x\n", btrc );
      goto exit;
   }
   for( int i= 0; i < pairedDevices.numberOfDevices; ++i )
   {
      printf(" device %d: name: %s id %lld\n", i, pairedDevices.devices[i].pcDeviceName, pairedDevices.devices[i].tDeviceId );
      if ( !strcmp( ctx->btDeviceName, pairedDevices.devices[i].pcDeviceName ) )
      {
         deviceIdx= i;
         ctx->btDeviceId= pairedDevices.devices[i].tDeviceId;
      }
   }

   if ( deviceIdx < 0 )
   {
      printf("Error: unable to find bluetooth device\n");
      goto exit;
   }

   btrc= BTRCore_GetDeviceTypeClass( ctx->btCore, ctx->btDeviceId, &ctx->btDeviceType, &deviceClass );
   if ( btrc != enBTRCoreSuccess )
   {
      printf("Error: BTRCore_GetDeviceTypeClass failed: %d\n", btrc );
      goto exit;
   }
   btrc= BTRCore_ConnectDevice( ctx->btCore, ctx->btDeviceId, ctx->btDeviceType );
   if ( btrc != enBTRCoreSuccess )
   {
      printf("Error: BTRCore_ConnectDevice failed: %d\n", btrc );
      goto exit;
   }

   limit= getCurrentTimeMillis() + 10000;
   for( ; ; )
   {
      now= getCurrentTimeMillis();
      if ( now > limit )
      {
         printf("Error: bluetooth connedtion timeout\n");
         goto exit;
      }
      if ( ctx->btDeviceConnected )
      {
         break;
      }
      usleep( 1000 );
   }

   limit= getCurrentTimeMillis() + 2000;
   for( ; ; )
   {
      now= getCurrentTimeMillis();
      if ( now > limit )
      {
         printf("Error: capture start timeout\n");
         goto exit;
      }
      if ( ctx->haveCaptureParams )
      {
         break;
      }
      usleep( 1000 );
   }

   if ( ctx->btMediaInfo.pstBtrCoreDevMCodecInfo == 0 )
   {
      int sizePcm= sizeof(stBTRCoreDevMediaPcmInfo);
      int sizeSbc= sizeof(stBTRCoreDevMediaSbcInfo);
      int sizeMpeg= sizeof(stBTRCoreDevMediaMpegInfo);
      int size= sizePcm > sizeSbc ? sizePcm : sizeSbc;
      if ( sizeMpeg > size ) size= sizeMpeg;
      ctx->btMediaInfo.pstBtrCoreDevMCodecInfo= calloc( 1, size );
      if ( ctx->btMediaInfo.pstBtrCoreDevMCodecInfo == 0 )
      {
         printf("Error: no memory for bt codec info\n");
         goto exit;
      }
   }

   btrc= BTRCore_GetDeviceMediaInfo( ctx->btCore, ctx->btDeviceId, enBTRCoreSpeakers, &ctx->btMediaInfo );
   if ( btrc != enBTRCoreSuccess )
   {
      printf("Error: BTRCore_GetDeviceMediaInfo failed: %d\n", btrc);
      goto exit;
   }

   if ( ctx->btMediaInfo.eBtrCoreDevMType != eBTRCoreDevMediaTypeSBC )
   {
      printf("Error: bluetooth device uses unsupported media type\n");
      goto exit;
   }

   btrc= BTRCore_AcquireDeviceDataPath( ctx->btCore, ctx->btDeviceId, enBTRCoreSpeakers, &ctx->btDeviceFd, &ctx->btDeviceReadMTU, &ctx->btDeviceWriteMTU );
   if ( btrc != enBTRCoreSuccess )
   {
      printf("Error: BTRCore_AcquireDeviceDataPath failed: %d\n", btrc);
      goto exit;
   }

   btmrc= BTRMgr_SO_Init(&ctx->btStream, btStreamStatusCallback, ctx );
   if ( btmrc != eBTRMgrSuccess )
   {
      printf("Error: BTRMgr_SO_Init failed: %d\n", btmrc);
      goto exit;
   }

   memset( &btInSettings, 0, sizeof(btInSettings) );
   btInSettings.pstBtrMgrInCodecInfo= btAllocBtrMgrCodecInfo();
   if ( !btInSettings.pstBtrMgrInCodecInfo )
   {
      printf("Error: no memory for In-Codec info\n");
      goto exit;
   }

   btInSettings.eBtrMgrInAType= eBTRMgrATypePCM;
   if ( ctx->captureParams.numChannels == 2 )
   {
      ((stBTRMgrPCMInfo*)btInSettings.pstBtrMgrInCodecInfo)->eBtrMgrAChan= eBTRMgrAChanStereo;
   }
   else
   {
      ((stBTRMgrPCMInfo*)btInSettings.pstBtrMgrInCodecInfo)->eBtrMgrAChan= eBTRMgrAChanMono;
   }
   switch( ctx->captureParams.bitsPerSample )
   {
      case 8:
         ((stBTRMgrPCMInfo*)btInSettings.pstBtrMgrInCodecInfo)->eBtrMgrSFmt= eBTRMgrSFmt8bit;
         break;
      default:
      case 16:
         ((stBTRMgrPCMInfo*)btInSettings.pstBtrMgrInCodecInfo)->eBtrMgrSFmt= eBTRMgrSFmt16bit;
         break;
      case 24:
         ((stBTRMgrPCMInfo*)btInSettings.pstBtrMgrInCodecInfo)->eBtrMgrSFmt= eBTRMgrSFmt24bit;
         break;
   }
   switch( ctx->captureParams.sampleRate )
   {
      case 16000:
         ((stBTRMgrPCMInfo*)btInSettings.pstBtrMgrInCodecInfo)->eBtrMgrSFreq= eBTRMgrSFreq16K;
         break;
      case 32000:
         ((stBTRMgrPCMInfo*)btInSettings.pstBtrMgrInCodecInfo)->eBtrMgrSFreq= eBTRMgrSFreq32K;
         break;
      case 44100:
         ((stBTRMgrPCMInfo*)btInSettings.pstBtrMgrInCodecInfo)->eBtrMgrSFreq= eBTRMgrSFreq44_1K;
         break;
      default:
      case 48000:
         ((stBTRMgrPCMInfo*)btInSettings.pstBtrMgrInCodecInfo)->eBtrMgrSFreq= eBTRMgrSFreq48K;
         break;
   }

   memset( &btOutSettings, 0, sizeof(btOutSettings) );
   btOutSettings.pstBtrMgrOutCodecInfo= btAllocBtrMgrCodecInfo();
   if ( !btOutSettings.pstBtrMgrOutCodecInfo )
   {
      printf("Error: no memory for Out-Codec info\n");
      goto exit;
   }

   btOutSettings.eBtrMgrOutAType= eBTRMgrATypeSBC;
   switch( ((stBTRCoreDevMediaSbcInfo*)ctx->btMediaInfo.pstBtrCoreDevMCodecInfo)->ui32DevMSFreq )
   {
      case 8000:
         ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->eBtrMgrSbcSFreq= eBTRMgrSFreq8K;
         break;
      case 16000:
         ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->eBtrMgrSbcSFreq= eBTRMgrSFreq16K;
         break;
      case 32000:
         ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->eBtrMgrSbcSFreq= eBTRMgrSFreq32K;
         break;
      case 44100:
         ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->eBtrMgrSbcSFreq= eBTRMgrSFreq44_1K;
         break;
      case 48000:
         ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->eBtrMgrSbcSFreq= eBTRMgrSFreq48K;
         break;
      default:
         ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->eBtrMgrSbcSFreq= eBTRMgrSFreqUnknown;
         break;
   }
   switch( ((stBTRCoreDevMediaSbcInfo*)ctx->btMediaInfo.pstBtrCoreDevMCodecInfo)->eDevMAChan )
   {
      case eBTRCoreDevMediaAChanMono:
         ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->eBtrMgrSbcAChan= eBTRMgrAChanMono;
         break;
      case eBTRCoreDevMediaAChanDualChannel:
         ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->eBtrMgrSbcAChan= eBTRMgrAChanDualChannel;
         break;
      case eBTRCoreDevMediaAChanStereo:
         ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->eBtrMgrSbcAChan= eBTRMgrAChanStereo;
         break;
      case eBTRCoreDevMediaAChanJointStereo:
         ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->eBtrMgrSbcAChan= eBTRMgrAChanJStereo;
         break;
      case eBTRCoreDevMediaAChan5_1:
         ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->eBtrMgrSbcAChan= eBTRMgrAChan5_1;
         break;
      case eBTRCoreDevMediaAChan7_1:
         ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->eBtrMgrSbcAChan= eBTRMgrAChan7_1;
         break;
      default:
      case eBTRCoreDevMediaAChanUnknown:
         ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->eBtrMgrSbcAChan= eBTRMgrAChanUnknown;
         break;
   }
   ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->ui8SbcAllocMethod= ((stBTRCoreDevMediaSbcInfo*)ctx->btMediaInfo.pstBtrCoreDevMCodecInfo)->ui8DevMSbcAllocMethod;
   ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->ui8SbcSubbands= ((stBTRCoreDevMediaSbcInfo*)ctx->btMediaInfo.pstBtrCoreDevMCodecInfo)->ui8DevMSbcSubbands;
   ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->ui8SbcBlockLength= ((stBTRCoreDevMediaSbcInfo*)ctx->btMediaInfo.pstBtrCoreDevMCodecInfo)->ui8DevMSbcBlockLength;
   ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->ui8SbcMinBitpool= ((stBTRCoreDevMediaSbcInfo*)ctx->btMediaInfo.pstBtrCoreDevMCodecInfo)->ui8DevMSbcMinBitpool;
   ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->ui8SbcMaxBitpool= ((stBTRCoreDevMediaSbcInfo*)ctx->btMediaInfo.pstBtrCoreDevMCodecInfo)->ui8DevMSbcMaxBitpool;
   ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->ui16SbcFrameLen= ((stBTRCoreDevMediaSbcInfo*)ctx->btMediaInfo.pstBtrCoreDevMCodecInfo)->ui16DevMSbcFrameLen;
   ((stBTRMgrSBCInfo*)btOutSettings.pstBtrMgrOutCodecInfo)->ui16SbcBitrate= ((stBTRCoreDevMediaSbcInfo*)ctx->btMediaInfo.pstBtrCoreDevMCodecInfo)->ui16DevMSbcBitrate;   

   btOutSettings.i32BtrMgrDevFd= ctx->btDeviceFd;
   btOutSettings.i32BtrMgrDevMtu= ctx->btDeviceWriteMTU;

   btmrc= BTRMgr_SO_GetEstimatedInABufSize(ctx->btStream, &btInSettings, &btOutSettings );
   if ( btmrc != eBTRMgrSuccess )
   {
      printf("Error: BTRMgr_SO_GetEstimatedInABufSize failed: %d\n", btmrc);
      btInSettings.i32BtrMgrInBufMaxSize= 3072;
   }

   btmrc= BTRMgr_SO_Start( ctx->btStream, &btInSettings, &btOutSettings );
   if ( btmrc != eBTRMgrSuccess )
   {
      printf("Error: BTRMgr_SO_Start failed: %d\n", btmrc );
      goto exit;
   }

   ctx->btInSettings= btInSettings;
   ctx->btOutSettings= btOutSettings;

   ctx->btStarted= true;

exit:

   printf("btThread: exit: connecdted %d started %d\n", ctx->btDeviceConnected, ctx->btStarted );

   return NULL;
}

static bool btInit( AppCtx *ctx )
{
   bool result= true;
   int rc;

   rc= pthread_create( &ctx->btThread, NULL, btThread, ctx );
   if ( rc )
   {
      printf( "Error: unable to created btThread\n");
      result= false;
   }

   return result;
}

static void btTerm( AppCtx *ctx )
{
   if ( ctx->btStarted )
   {
      BTRMgr_SO_SendEOS( ctx->btStream );
      BTRMgr_SO_Stop( ctx->btStream );
      ctx->btStarted= false;
   }

   if ( ctx->btStream )
   {
      BTRMgr_SO_DeInit( ctx->btStream );
      ctx->btStream= 0;
   }

   if ( ctx->btDeviceConnected )
   {
      BTRCore_DisconnectDevice( ctx->btCore, ctx->btDeviceId, ctx->btDeviceType );
      ctx->btDeviceConnected= false;
   }
   if ( ctx->btCore )
   {
      BTRCore_DeInit( ctx->btCore );
      ctx->btCore= 0;
   }
}
#endif

static bool captureInit( AppCtx *ctx )
{
   bool result= false;
   const char *serverName= getenv("AUDSRV_NAME");

   ctx->audsrvCapture= AudioServerConnect( serverName );
   if ( !ctx->audsrvCapture )
   {
      printf("captureInit: AudioServerConnect for SAP capture failed\n");
      goto exit;
   }

   if ( !AudioServerInitSession( ctx->audsrvCapture, AUDSRV_SESSION_Capture, true, "asplayer-capture" ) )
   {
      printf("captureInit: AudioServerInitSession for SAP capture failed\n");
      AudioServerDisconnect( ctx->audsrvCapture );
      ctx->audsrvCapture= 0;
      goto exit;
   }

   if ( !AudioServerStartCapture( ctx->audsrvCapture, "btSAP", captureCallback, ctx ) )
   {
      printf("captureInit: AudioServerStartCapture for SAP capture failed\n");
      goto exit;
   }

   result= true;

exit:
   return result;
}

static void captureTerm( AppCtx *ctx )
{
   if ( ctx->audsrvCapture )
   {
      AudioServerDisconnect( ctx->audsrvCapture );
      ctx->audsrvCapture= 0;
   }
}

static void captureCallback( void *userData, AudSrvCaptureParameters *params, unsigned char *data, int datalen )
{
   AppCtx *ctx= (AppCtx*)userData;
   
   if ( ctx )
   {
      ctx->captureParams= *params;
      ctx->haveCaptureParams= true;

      #ifdef USE_BT
      if ( ctx->btStarted )
      {
         eBTRMgrRet btmrc;

         btmrc= BTRMgr_SO_SendBuffer( ctx->btStream, (char*)data, datalen );
         if ( btmrc != eBTRMgrSuccess )
         {
            printf("Error: BTRMgr_SO_SendBuffer failed %d : datalen %d\n", btmrc, datalen );
         }
      }
      #endif
   }
}

static bool getContainerType( AppCtx *ctx )
{
   bool result= false;
   if ( ctx->mediaURI )
   {
      const char *type= strrchr( ctx->mediaURI, '.' );
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
      printf("Error: unable to create caps for container\n");
      result= false;
   }
   return result;
}

static bool getDemux( AppCtx *ctx, bool mainAudio )
{
   bool result= true;
   GList *factoryList;
   GstElement **demux= (mainAudio ? &ctx->demux : &ctx->demuxSAP);
   GstElement *audiosink= (mainAudio ? ctx->audiosink : ctx->audiosinkSAP);
   const char *nameForDemux= (mainAudio ? "demux" : "demuxSAP" );

   if ( ctx->demuxName )
   {
      *demux= gst_element_factory_make( ctx->demuxName, nameForDemux );
   }
   else
   {
      switch( ctx->containerType )
      {
         case Container_ts:
            factoryList= gst_element_factory_list_get_elements( GST_ELEMENT_FACTORY_TYPE_DEMUXER, GST_RANK_NONE );
            if ( factoryList )
            {
               GstPad *audioSinkPad= 0;
               GstCaps *audioSinkCaps= 0;
               GList *iter;

               audioSinkPad= gst_element_get_static_pad( audiosink, "sink" );
               if ( audioSinkPad )
               {
                  audioSinkCaps= gst_pad_get_pad_template_caps( audioSinkPad );
                  if (audioSinkCaps )
                  {
                     iter= factoryList;
                     for( iter= factoryList; iter != NULL; iter= iter->next )
                     {
                        GstElementFactory *factory= (GstElementFactory*)iter->data;
                        if ( factory )
                        {
                           if ( gst_element_factory_can_src_any_caps( factory, audioSinkCaps ) )
                           {
                              if ( gst_element_factory_can_sink_any_caps( factory, ctx->capsContainer ) )
                              {
                                 printf("factory %p can produce a compatible demux\n", factory);

                                 *demux= gst_element_factory_create( factory, nameForDemux );

                                 gchar **keys= gst_element_factory_get_metadata_keys( factory );
                                 int i= 0;
                                 while( keys[i] )
                                 {
                                    gchar *key= keys[i++];
                                    if ( key ) 
                                    {
                                       const char *language= (mainAudio ? ctx->languageMain : ctx->languageSAP);
                                       const gchar *value= gst_element_factory_get_metadata( factory, key );
                                       if ( ctx->verbose ) printf("  %s: %s\n", key, value );
                                       if ( strcmp( key, "long-name" ) == 0 )
                                       {
                                          if ( strstr( value, "BRCM" ) )
                                          {
                                             g_object_set( G_OBJECT(*demux), "preferred-language", language, NULL );
                                          }
                                          else if ( strstr( value, "Intel" ) )
                                          {
                                             g_object_set( G_OBJECT(*demux), "aud-lang", language, NULL );
                                          }
                                       }
                                    }
                                 }
                                 g_strfreev(keys);
                                 if ( ctx->verbose ) printf("\n");
                                 break;
                              }
                           }
                        }
                     }
                  }
                  else
                  {
                     printf("Error: unable to get audiosink sink pad template caps\n" );
                  }
               }
               else
               {
                  printf("Error: unable to get audiosink sink pad\n");
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

               gst_plugin_feature_list_free( factoryList );
               factoryList= 0;
            }
            else
            {
               printf("Error: unable to get factory list\n");
            }
            break;
         case Container_mp4:
            *demux= gst_element_factory_make( "qtdemux", "demux" );
            break;
         case Container_wav:
            // The 'wavparse' element could be used as the wav demux,
            // but we prefer passing unparsed wav data to audsrvsink
            *demux= gst_element_factory_make( "identity", "demux" );
            break;
         default:
            break;
      }
   }
   if ( *demux )
   {
      gst_object_ref( *demux );
   }
   else
   {
      if ( ctx->demuxName )
      {
         printf("Error: unable to create demux instance (%s)\n", ctx->demuxName );
      }
      else
      {
         printf("Error: unable to create demux instance\n" );
      }
      result= false;
   }
   return result;
}

static bool getAudioParser( AppCtx *ctx )
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
      GList *factoryList;
      factoryList= gst_element_factory_list_get_elements( GST_ELEMENT_FACTORY_TYPE_PARSER|GST_ELEMENT_FACTORY_TYPE_MEDIA_AUDIO, 
                                                          GST_RANK_NONE );
      if ( factoryList )
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
                     iter= factoryList;
                     for( iter= factoryList; iter != NULL; iter= iter->next )
                     {
                        GstElementFactory *factory= (GstElementFactory*)iter->data;
                        if ( factory )
                        {
                           if ( gst_element_factory_can_src_any_caps( factory, audioSinkCaps ) )
                           {
                              if ( gst_element_factory_can_sink_any_caps( factory, demuxSrcCaps ) )
                              {
                                 printf("factory %p can produce a compatible audio parser\n", factory);

                                 if ( ctx->verbose )
                                 {
                                    gchar **keys= gst_element_factory_get_metadata_keys( factory );
                                    int i= 0;
                                    while( keys[i] )
                                    {
                                       gchar *key= keys[i++];
                                       if ( key ) printf("  %s: %s\n", key, gst_element_factory_get_metadata( factory, key ) );
                                    }
                                    g_strfreev(keys);
                                    printf("\n");
                                 }

                                 ctx->parserAudio= gst_element_factory_create( factory, "audioparser" );
                                 break;
                              }
                           }
                        }
                     }
                  }
                  else
                  {
                     printf("Error: unable to get demux src pad template caps\n" );
                  }
               }
               else
               {
                  printf("Error: unable to get demux src pad\n");
               }
            }
            else
            {
               printf("Error: unable to get audiosink sink pad template caps\n" );
            }
         }
         else
         {
            printf("Error: unable to get audiosink sink pad\n");
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

         gst_plugin_feature_list_free( factoryList );
         factoryList= 0;
      }
   }

   if ( ctx->parserAudio )
   {
      gst_object_ref( ctx->parserAudio );
      result= true;
   }
   else if ( needParser )
   {
      printf("Error: unable to create audio parser\n");
   }

   return result;
}

static bool getVideoDecoder( AppCtx *ctx, GstPad *demuxSrcPad )
{
   bool result= false;
   GList *factoryList;

   factoryList= gst_element_factory_list_get_elements( GST_ELEMENT_FACTORY_TYPE_DECODER|GST_ELEMENT_FACTORY_TYPE_PARSER|GST_ELEMENT_FACTORY_TYPE_MEDIA_VIDEO, 
                                                       GST_RANK_NONE );
   if ( factoryList )
   {
      GstPad *videoSinkPad= 0;
      GstCaps *videoSinkCaps= 0;
      GstPadTemplate *demuxSrcPadTemplate= 0;
      GstCaps *demuxSrcCaps= 0;
      GList *iter;

      videoSinkPad= gst_element_get_static_pad( ctx->videosink, "sink" );
      if ( videoSinkPad )
      {
         videoSinkCaps= gst_pad_get_pad_template_caps( videoSinkPad );
         if ( videoSinkCaps )
         {
            demuxSrcCaps= gst_pad_get_current_caps( demuxSrcPad );
            if (demuxSrcCaps )
            {
               iter= factoryList;
               for( iter= factoryList; iter != NULL; iter= iter->next )
               {
                  GstElementFactory *factory= (GstElementFactory*)iter->data;
                  if ( factory )
                  {
                     if ( gst_element_factory_can_src_any_caps( factory, videoSinkCaps ) )
                     {
                        if ( gst_element_factory_can_sink_any_caps( factory, demuxSrcCaps ) )
                        {
                           printf("factory %p can produce a compatible video decoder\n", factory);

                           if ( ctx->verbose )
                           {
                              gchar **keys= gst_element_factory_get_metadata_keys( factory );
                              int i= 0;
                              while( keys[i] )
                              {
                                 gchar *key= keys[i++];
                                 if ( key ) printf("  %s: %s\n", key, gst_element_factory_get_metadata( factory, key ) );
                              }
                              g_strfreev(keys);
                              printf("\n");
                           }

                           ctx->videodecoder= gst_element_factory_create( factory, "viddec" );
                           break;
                        }
                     }
                  }
               }
            }
            else
            {
               printf("Error: unable to get demux src pad template caps\n" );
            }
         }
         else
         {
            printf("Error: unable to get videosink sink pad template caps\n" );
         }
      }
      else
      {
         printf("Error: unable to get videosink sink pad\n");
      }

      if ( demuxSrcCaps )
      {
         gst_caps_unref( demuxSrcCaps );
         demuxSrcCaps= 0;
      }

      if ( videoSinkCaps )
      {
         gst_caps_unref( videoSinkCaps );
         videoSinkCaps= 0;
      }
      
      if ( videoSinkPad )
      {
         gst_object_unref( videoSinkPad );
         videoSinkPad= 0;
      }

      gst_plugin_feature_list_free( factoryList );
      factoryList= 0;
   }

   if ( ctx->videodecoder )
   {
      gst_object_ref( ctx->videodecoder );
      result= true;
   }
   else
   {
      printf("Error: unable to create video decoder\n");
   }

   return result;
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
            printf("Error: %s\n", error->message);
            if ( debug )
            {
               printf("Debug info: %s\n", debug);
            }
            g_error_free(error);
            g_free(debug);
            g_quit= true;
            g_main_loop_quit( ctx->loop );
         }
         break;
     case GST_MESSAGE_EOS:
         printf( "EOS ctx %p\n", ctx );
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
   
   printf("demux src pad added: demux %p, %s:%s\n", element, GST_DEBUG_PAD_NAME(srcPad));

   gchar *name= gst_pad_get_name(srcPad);
   if ( name )
   {
      if ( strstr( name, "aud" ) )
      {
         switch( ctx->containerType )
         {
            case Container_ts:
               if ( element == ctx->demux )
               {
                  if ( !gst_element_link( ctx->demux, ctx->audiosink ) )
                  {
                     printf("Error: unable link demux and audiosink\n" );
                     error= true;
                  }   
               }
               else if ( element == ctx->demuxSAP )
               {
                  printf("SAP audio pad added\n");
                  if ( !gst_element_link( ctx->demuxSAP, ctx->audiosinkSAP ) )
                  {
                     printf("Error: unable link demux and audiosink for SAP\n" );
                     error= true;
                  }   
               }
               break;
            case Container_mp4:
               if ( !gst_element_link_many( ctx->demux, ctx->parserAudio, ctx->audiosink, NULL ) )
               {
                  if ( !gst_element_link_many( ctx->demux, ctx->audiosink, NULL ) )
                  {
                     printf("Error: unable link demux, parser, and audiosink\n" );
                     error= true;
                  }
               }   
               break;
            default:
               break;
         }
         if ( !error )
         {
            printf("demux and audiosink linked\n");
         }
      }
      else if ( strstr( name, "vid" ) )
      {
         if ( element == ctx->demux )
         {
            if ( !gst_element_link( ctx->demux, ctx->videosink ) )
            {
               if ( getVideoDecoder( ctx, srcPad ) )
               {
                  gst_bin_remove( GST_BIN(ctx->pipeline), ctx->videosink );
                  gst_element_set_state( ctx->videosink, GST_STATE_NULL );
                  gst_object_unref( ctx->videosink );
                  ctx->videosink= gst_element_factory_make( "westerossink", "videosink" );
                  if ( !ctx->videosink )
                  {
                     printf("Error: unable to create westerossink instance\n" );
                     error= true;
                  }
                  gst_object_ref( ctx->videosink );
               
                  gst_bin_add_many( GST_BIN(ctx->pipeline), 
                                    ctx->videodecoder,
                                    ctx->videosink,
                                    NULL );
                  if ( !gst_element_link_many( ctx->demux, ctx->videodecoder, NULL ) )
                  {
                     printf("Error: unable link demux, viddec, and videosink\n" );
                     error= true;
                  }
                  if ( !gst_element_link_many( ctx->videodecoder, ctx->videosink, NULL ) )
                  {
                     printf("Error: unable link viddec, and videosink\n" );
                     error= true;
                  }
                  if ( !gst_element_sync_state_with_parent( ctx->videodecoder ) )
                  {
                     printf("Error: unable to sync videodecoder state\n");
                     error= true;
                  }
                  if ( !gst_element_sync_state_with_parent( ctx->videosink ) )
                  {
                     printf("Error: unable to sync videosink state\n");
                     error= true;
                  }
               }
               else
               {
                  printf("Error: unable link demux and videosink\n" );
                  error= true;
               }
            }
         }
         if ( !error )
         {
            printf("demux and videosink linked\n");
         }
      }
      g_free(name);
   }
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
      printf("Error: unable to create pipeline instance\n" );
      goto exit;
   }

   ctx->bus= gst_pipeline_get_bus( GST_PIPELINE(ctx->pipeline) );
   if ( !ctx->bus )
   {
      printf("Error: unable to get pipeline bus\n");
      goto exit;
   }
   gst_bus_add_watch( ctx->bus, busCallback, ctx );

   if ( !getContainerType( ctx ) )
   {
      printf("Error: unsupported media container: (%s)\n", ctx->mediaURI );
      goto exit;
   }

   if ( !getContainerCaps( ctx ) )
   {
      printf("Error: unable to get caps for container\n");
      goto exit;
   }

   ctx->httpsrc= gst_element_factory_make( "httpsrc", "httpsrc" );
   if ( !ctx->httpsrc )
   {
      printf("Error: unable to create httpsrc instance\n" );
      goto exit;
   }
   gst_object_ref( ctx->httpsrc );

   ctx->queue= gst_element_factory_make( "queue", "queue" );
   if ( !ctx->queue )
   {
      printf("Error: unable to create queue instance\n" );
      goto exit;
   }
   gst_object_ref( ctx->queue );

   ctx->tee= gst_element_factory_make( "tee", "tee" );
   if ( !ctx->tee )
   {
      printf("Error: unable to create tee instance\n" );
      goto exit;
   }
   gst_object_ref( ctx->tee );

   if ( ctx->containerType != Container_wav )
   {
      ctx->videosink= gst_element_factory_make( "westerossink", "videosink" );
      if ( !ctx->videosink )
      {
         printf("Error: unable to create westerossink instance\n" );
         goto exit;
      }
      gst_object_ref( ctx->videosink );
   }

   ctx->audiosink= gst_element_factory_make( "audsrvsink", "audiosink" );
   if ( !ctx->audiosink )
   {
      printf("Error: unable to create audiosink instance\n" );
      goto exit;
   }
   gst_object_ref( ctx->audiosink );

   if ( !getDemux( ctx ) )
   {
      printf("Error: unable to get a workable demux\n");
      goto exit;
   }

   if ( !getAudioParser( ctx ) )
   {
      printf("Error: unable to get a workable audio parser\n");
      goto exit;
   }

   gst_bin_add_many( GST_BIN(ctx->pipeline), 
                     ctx->httpsrc,
                     ctx->queue,
                     ctx->tee,
                     ctx->demux,
                     ctx->audiosink,
                     NULL
                   );
   if ( ctx->videosink )
   {
      gst_bin_add_many( GST_BIN(ctx->pipeline), 
                        ctx->videosink,
                        NULL );
   }
   if ( ctx->parserAudio )
   {
      gst_bin_add_many( GST_BIN(ctx->pipeline), 
                        ctx->parserAudio,
                        NULL );
   }

   if ( !gst_element_link_filtered( ctx->httpsrc, ctx->queue, ctx->capsContainer ) )
   {
      printf("Error: unable to link src to queue\n");
      goto exit;
   }

   if ( !gst_element_link_many( ctx->queue, ctx->tee, ctx->demux, NULL ) )
   {
      printf("Error: unable to link queue, tee, and demux\n");
      goto exit;
   }

   g_object_set( G_OBJECT(ctx->httpsrc), "location", ctx->mediaURI, NULL );
   g_object_set( G_OBJECT(ctx->audiosink), "audio-pts-offset", 4500, NULL );
   g_object_set( G_OBJECT(ctx->audiosink), "session-name", "asplayer-main-playback", NULL );

   switch( ctx->containerType )
   {
      case Container_ts:
      case Container_mp4:
         g_signal_connect(ctx->demux, "pad-added", G_CALLBACK(demuxSrcPadAdded), ctx);
         break;
      case Container_wav:
         if ( !gst_element_link( ctx->demux, ctx->audiosink ) )
         {
            printf("Error: unable to link demux and audiosink\n");
            goto exit;
         }
         break;
      default:
         break;
   }

   if ( ctx->languageSAP && (ctx->containerType == Container_ts) )
   {
      ctx->audiosinkSAP= gst_element_factory_make( "audsrvsink", "audiosinkSAP" );
      if ( !ctx->audiosinkSAP )
      {
         printf("Error: unable to create audiosink SAP instance\n" );
         goto exit;
      }
      gst_object_ref( ctx->audiosinkSAP );

      if ( !getDemux( ctx, false ) )
      {
         printf("Error: unable to get a workable demux for SAP\n");
         goto exit;
      }

      gst_bin_add_many( GST_BIN(ctx->pipeline), 
                        ctx->demuxSAP,
                        ctx->audiosinkSAP,
                        NULL
                      );

      if ( !gst_element_link_many( ctx->tee, ctx->demuxSAP, NULL ) )
      {
         printf("Error: unable to link tee to SAP demux\n");
         goto exit;
      }

      g_object_set( G_OBJECT(ctx->audiosinkSAP), "audio-pts-offset", 22000, NULL );
      g_object_set( G_OBJECT(ctx->audiosinkSAP), "session-type", 2, NULL );
      g_object_set( G_OBJECT(ctx->audiosinkSAP), "session-name", "btSAP", NULL );
      g_object_set( G_OBJECT(ctx->audiosinkSAP), "session-private", TRUE, NULL );
      g_signal_connect(ctx->demuxSAP, "pad-added", G_CALLBACK(demuxSrcPadAdded), ctx);
   }

   result= true;   

exit:

   if ( ctx->capsContainer )
   {
      gst_caps_unref( ctx->capsContainer );
      ctx->capsContainer= 0;
   }

   return result;
}

static void destroyPipeline( AppCtx *ctx )
{
   if ( ctx->pipeline )
   {
      gst_element_set_state(ctx->pipeline, GST_STATE_NULL);
   }
   if ( ctx->audiosinkSAP )
   {
      gst_object_unref( ctx->audiosinkSAP );
      ctx->audiosinkSAP= 0;
   }
   if ( ctx->demuxSAP )
   {
      gst_object_unref( ctx->demuxSAP );
      ctx->demuxSAP= 0;
   }
   if ( ctx->audiosink )
   {
      gst_object_unref( ctx->audiosink );
      ctx->audiosink= 0;
   }   
   if ( ctx->videodecoder )
   {
      gst_object_unref( ctx->videodecoder );
      ctx->videodecoder= 0;
   }
   if ( ctx->videosink )
   {
      gst_object_unref( ctx->videosink );
      ctx->videosink= 0;
   }
   if ( ctx->parserAudio )
   {
      gst_object_unref( ctx->parserAudio );
      ctx->parserAudio= 0;
   }
   if ( ctx->demux )
   {
      gst_object_unref( ctx->demux );
      ctx->demux= 0;
   }
   if ( ctx->tee )
   {
      gst_object_unref( ctx->tee );
      ctx->tee= 0;
   }
   if ( ctx->queue )
   {
      gst_object_unref( ctx->queue );
      ctx->queue= 0;
   }
   if ( ctx->httpsrc )
   {
      gst_object_unref( ctx->httpsrc );
      ctx->httpsrc= 0;
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

static GstState
validateStateWithMsTimeout (
    AppCtx*     ctx,
    GstState    stateToValidate,
    guint       msTimeOut
) {
    GstState    gst_current;
    GstState    gst_pending;
    float       timeout = 100.0;
    gint        gstGetStateCnt = 5;

    do { 
        if ((GST_STATE_CHANGE_SUCCESS == gst_element_get_state(ctx->pipeline, &gst_current, &gst_pending, timeout * GST_MSECOND)) && (gst_current == stateToValidate)) {
            printf("validateStateWithMsTimeout - PIPELINE gst_element_get_state - SUCCESS : State = %d, Pending = %d\n", gst_current, gst_pending);
            return gst_current;
        }
        usleep(msTimeOut * 1000); // Let pipeline safely transition to required state
    } while ((gst_current != stateToValidate) && (gstGetStateCnt-- != 0)) ;

    printf("validateStateWithMsTimeout - PIPELINE gst_element_get_state - FAILURE : State = %d, Pending = %d\n", gst_current, gst_pending);
    return gst_current;
}

int main( int argc, char **argv )
{
   int result= -1;
   int argidx, len;
   const char *uri= 0;
   struct sigaction sigAction;
   AppCtx *ctx= 0;
   const char *demuxName= 0;
   const char *langMain= "eng";
   const char *langSAP= 0;
   const char *btDeviceName= 0;
   bool takeDemuxToNull= false;
   bool verbose= false;

   printf("asplayer: v1.0\n\n" );

   if ( argc < 2 )
   {
      showUsage();
      goto exit;
   }

   argidx= 1;   
   while ( argidx < argc )
   {
      if ( argv[argidx][0] == '-' )
      {
         switch( argv[argidx][1] )
         {
            case '-':
               len= strlen(argv[argidx]);
               if ( (len == 6) && !strncmp( argv[argidx], "--lang", len ) )
               {
                  if ( argidx+1 < argc )
                  {
                     langMain= argv[++argidx];
                  }
               }
               else if ( (len == 5) && !strncmp( argv[argidx], "--sap", len ) )
               {
                  if ( argidx+1 < argc )
                  {
                     langSAP= argv[++argidx];
                  }
               }
               #ifdef USE_BT
               else if ( (len == 4) && !strncmp( argv[argidx], "--bt", len ) )
               {
                  if ( argidx+1 < argc )
                  {
                     btDeviceName= argv[++argidx];
                  }
               }
               #endif
               else if ( (len == 7) && !strncmp( argv[argidx], "--demux", len ) )
               {
                  if ( argidx+1 < argc )
                  {
                     demuxName= argv[++argidx];
                     takeDemuxToNull= true;
                  }
               }
               else if ( (len == 9) && !strncmp( argv[argidx], "--verbose", len ) )
               {
                  verbose= true;
               }
               break;
            case '?':
               showUsage();
               goto exit;
            default:
               printf( "unknown option %s\n\n", argv[argidx] );
               exit( -1 );
               break;
         }
      }
      else
      {
         if ( !uri )
         {
            uri= argv[argidx];
         }
         else
         {
            printf( "ignoring extra argument: %s\n", argv[argidx] );
         }
      }
      
      ++argidx;
   }

   if ( !uri )
   {
      printf( "missing uri argument\n" );
      goto exit;
   }
         
   printf( "playing asset: %s\n", uri );

   ctx= (AppCtx*)calloc( 1, sizeof(AppCtx) );
   if ( !ctx )
   {
      printf("Error: unable to allocate application context\n");
      goto exit;
   }
   ctx->verbose= verbose;
   ctx->demuxName= demuxName;
   ctx->takeDemuxToNull= takeDemuxToNull;
   ctx->languageMain= langMain;
   ctx->languageSAP= langSAP;
   ctx->mediaURI= uri;
   ctx->btDeviceName= btDeviceName;

   if ( ctx->languageSAP && ctx->btDeviceName )
   {
      #ifdef USE_BT
      if ( !btInit( ctx ) )
      {
         printf("Error: unable to initialize bluetooth\n");
         goto exit;
      }
      #endif
#if 1
      if ( !captureInit( ctx ) )
      {
         printf("Error: unable to initialize audio capture\n");
         goto exit;
      }
#endif
   }

   if ( createPipeline( ctx ) )
   {
      printf("pipeline created\n");
      ctx->loop= g_main_loop_new(NULL,FALSE);

      if ( GST_STATE_CHANGE_FAILURE != gst_element_set_state(ctx->pipeline, GST_STATE_PLAYING) )
      {
         if(GST_STATE_PLAYING != validateStateWithMsTimeout(ctx, GST_STATE_PLAYING, 100)) { // 100 ms
            printf("validateStateWithMsTimeout - FAILED GstState %d\n", GST_STATE_PLAYING);
         }

#if 0
         if ( ctx->languageSAP && ctx->btDeviceName ) {
            if ( !captureInit( ctx ) ) {
               printf("Error: unable to initialize audio capture\n");
               goto exit;
            }
         }
#endif

         if ( ctx->loop )
         {
            sigAction.sa_handler= signalHandler;
            sigemptyset(&sigAction.sa_mask);
            sigAction.sa_flags= SA_RESETHAND;
            sigaction(SIGINT, &sigAction, NULL);

            while( !g_quit )
            {
               g_main_context_iteration( NULL, FALSE );

               usleep( 10000 );
            }
         }
      }      
   }
  
exit:

   if ( ctx )
   {
      destroyPipeline( ctx );

      captureTerm( ctx );
      #ifdef USE_BT
      btTerm( ctx );
      #endif

      if ( ctx->loop )
      {
         g_main_loop_unref(ctx->loop);
         ctx->loop= 0;
      }
      
      free( ctx );
   }
   
   return result;
}

