/*
 * Copyright (C) 2016 RDK Management
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

/**
* @defgroup audioserver
* @{
* @defgroup audsrvsink
* @{
**/


#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "gstaudsrvsink.h"

#include <stdlib.h>
#include <stdio.h>
#include <limits.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <sys/time.h>

#define GST_PACKAGE_ORIGIN "http://gstreamer.net/"

#ifndef SOC_SPECIFIC_CAPS
  #define SOC_SPECIFIC_CAPS ""
#endif

#define AUDSRV_SINK_CAPS \
        SOC_SPECIFIC_CAPS

static GstStaticPadTemplate gst_audsrv_sink_pad_template =
GST_STATIC_PAD_TEMPLATE ("sink",
    GST_PAD_SINK,
    GST_PAD_ALWAYS,
    GST_STATIC_CAPS(AUDSRV_SINK_CAPS));

GST_DEBUG_CATEGORY (gst_audsrv_sink_debug);
#define GST_CAT_DEFAULT gst_audsrv_sink_debug

enum
{
   PROP_0,
   PROP_MUTE,
   PROP_VOLUME,
   PROP_SESSION,
   PROP_SESSION_TYPE,
   PROP_SESSION_PRIVATE,
   PROP_SESSION_NAME
};

static long long getCurrentTimeMillis(void)
{
   struct timeval tv;
   long long utcCurrentTimeMillis;

   gettimeofday(&tv,0);
   utcCurrentTimeMillis= tv.tv_sec*1000LL+(tv.tv_usec/1000LL);

   return utcCurrentTimeMillis;
}

static long long getCurrentTimeMicro()
{
   struct timeval tv;
   long long utcCurrentTimeMicro;

   gettimeofday(&tv,0);
   utcCurrentTimeMicro= tv.tv_sec*1000000LL+tv.tv_usec;

   return utcCurrentTimeMicro;
}

#ifdef USE_GST1
#define gst_audsrv_sink_parent_class parent_class
G_DEFINE_TYPE (GstAudsrvSink, gst_audsrv_sink, GST_TYPE_BASE_SINK);
#else
GST_BOILERPLATE (GstAudsrvSink, gst_audsrv_sink, GstBaseSink,
    GST_TYPE_BASE_SINK);

static void
gst_audsrv_sink_base_init (gpointer g_class)
{
   GstElementClass *gstelement_class= GST_ELEMENT_CLASS (g_class);

   GST_DEBUG_CATEGORY_INIT(gst_audsrv_sink_debug, "audsrvsink", 0,
      "audio server sink element");

  gst_element_class_add_pad_template (gstelement_class,
      gst_static_pad_template_get (&gst_audsrv_sink_pad_template));

  gst_element_class_set_details_simple (gstelement_class, "Audio Server Sink",
      "Sink",
      "Accepts audio data as an input to audio server",
      "Comcast");
}
#endif

static void gst_audsrv_sink_term(GstAudsrvSink *sink);
static void gst_audsrv_sink_dispose(GObject *object);
static void gst_audsrv_sink_set_property(GObject *object,
                guint prop_id, const GValue *value, GParamSpec *pspec);
static void gst_audsrv_sink_get_property(GObject *object,
                guint prop_id, GValue *value, GParamSpec* pspec);
static GstStateChangeReturn gst_audsrv_sink_change_state(GstElement *element, GstStateChange transition);
static gboolean gst_audsrv_sink_start(GstBaseSink *bsink);
static gboolean gst_audsrv_sink_stop(GstBaseSink *bsink);
static GstFlowReturn gst_audsrv_sink_render(GstBaseSink *bsink, GstBuffer *buf);
#ifdef USE_GST1
static gboolean gst_audsrv_sink_event(GstPad *pad, GstObject *parent, GstEvent *event);
#else
static gboolean gst_audsrv_sink_event(GstPad *pad, GstEvent *event);
#endif
static gboolean gst_audsrv_sink_query(GstElement *element, GstQuery *query);
static gboolean gst_audsrv_sink_prepare_to_render( GstAudsrvSink *sink, GstBuffer *buffer );
static void audsrv_eos_detected( void *userData );

static void
gst_audsrv_sink_class_init(GstAudsrvSinkClass *klass)
{
   GObjectClass *gobject_class= G_OBJECT_CLASS(klass);
   #ifdef USE_GST1
   GstElementClass *gstelement_class= GST_ELEMENT_CLASS(klass);
   #endif
   GstBaseSinkClass *gstbasesink_class= GST_BASE_SINK_CLASS(klass);

   klass->parent_dispose= gobject_class->dispose;

   gobject_class->dispose= gst_audsrv_sink_dispose;
   gobject_class->set_property= gst_audsrv_sink_set_property;
   gobject_class->get_property= gst_audsrv_sink_get_property;

   gstelement_class->change_state= gst_audsrv_sink_change_state;
   gstelement_class->query= gst_audsrv_sink_query;

   gstbasesink_class->get_times= 0;
   gstbasesink_class->start= gst_audsrv_sink_start;
   gstbasesink_class->stop= gst_audsrv_sink_stop;
   gstbasesink_class->render= gst_audsrv_sink_render;

   g_object_class_install_property(gobject_class, PROP_MUTE,
      g_param_spec_boolean("mute", "mute", "Mute this audio stream",
          FALSE,
          (GParamFlags)G_PARAM_READWRITE));

   g_object_class_install_property(gobject_class, PROP_VOLUME,
      g_param_spec_float("volume", "volume", "Volume level: 0.0 - 1.0",
          0.0, 1.0, 1.0,
          (GParamFlags)G_PARAM_READWRITE));

   g_object_class_install_property (gobject_class, PROP_SESSION,
      g_param_spec_pointer (
          "session",
          "session handle",
          "AudioServer session handle.",
          (GParamFlags)G_PARAM_READWRITE ));

   g_object_class_install_property(gobject_class, PROP_SESSION_TYPE,
      g_param_spec_uint("session-type", "session type", "audio session is primary (1), secondary (2), or effect(3)",
          1, 3, 1,
          (GParamFlags)G_PARAM_READWRITE));

   g_object_class_install_property(gobject_class, PROP_SESSION_PRIVATE,
      g_param_spec_boolean("session-private", "session private", "session is private (TRUE) or not (FALSE)",
          FALSE,
          (GParamFlags)G_PARAM_READWRITE));

   g_object_class_install_property(gobject_class, PROP_SESSION_NAME,
       g_param_spec_string ("session-name", "session name",
           "name used to identify this session",
           NULL, 
           (GParamFlags)G_PARAM_WRITABLE));

   GST_DEBUG_CATEGORY_INIT(gst_audsrv_sink_debug, "audsrvsink", 0, "audsrvsink element");

   #ifdef USE_GST1
   gst_element_class_add_pad_template(gstelement_class,
      gst_static_pad_template_get(&gst_audsrv_sink_pad_template));
   gst_element_class_set_static_metadata (gstelement_class, "Audio Server Sink",
      "Sink",
      "Accepts audio data as an input to audio server",
      "Comcast");
   #endif

   gst_audsrv_sink_soc_class_init(klass);
}

static void
#ifdef USE_GST1
gst_audsrv_sink_init (GstAudsrvSink *sink)
#else
gst_audsrv_sink_init (GstAudsrvSink *sink, GstAudsrvSinkClass *g_class)
#endif
{
   sink->parentEventFunc= GST_PAD_EVENTFUNC(GST_BASE_SINK_PAD(sink));
   gst_pad_set_event_function(GST_BASE_SINK_PAD(sink), GST_DEBUG_FUNCPTR(gst_audsrv_sink_event));
   gst_base_sink_set_sync(GST_BASE_SINK(sink), FALSE);
   gst_base_sink_set_async_enabled(GST_BASE_SINK(sink), FALSE);
   sink->caps= NULL;
   sink->mayDumpPackets= FALSE;
   sink->dumpingPackets= FALSE;
   sink->dumpPacketCount= 0;
   sink->dumpPacketLimit= 10;
   sink->asyncStateChange= FALSE;
   sink->ownSession= FALSE;
   sink->sessionType= AUDSRV_SESSION_Primary;
   sink->sessionPrivate= FALSE;
   sink->sessionName= 0;
   sink->audsrv= 0;
   if ( getenv("AUDSRVSINK_DUMP_PACKETS") )
   {
      sink->mayDumpPackets= TRUE;
   }
   sink->readyToRender= FALSE;
   sink->playing= FALSE;
   sink->playPending= FALSE;
   sink->expectFakeBuffers= FALSE;
   sink->tunnelData= TRUE;
   sink->peerPad= NULL;
   sink->mute= FALSE;
   sink->volume= 1.0;
   sink->lastSyncTime= -1LL;
   sink->audioOnly= FALSE;
   sink->isFlushing= FALSE;
   sink->eosDetected= FALSE;
   sink->haveFirstAudioTime= FALSE;
   sink->firstAudioTime= 0LL;
   sink->position= 0LL;
   sink->wavParams.codec= 0;
   sink->wavParams.channelCount= 0;
   sink->wavParams.sampleRate= 0;
   sink->wavParams.bitsPerSample= 0;
   sink->wavParams.signedData= true;
   memset( &sink->audioInfo, 0, sizeof(sink->audioInfo) );
   
   if ( gst_audsrv_sink_soc_init(sink) )
   {
      sink->initialized= TRUE;
   }
   else
   {
      GST_ERROR("element soc specific initialization failed");
   }
}

static void gst_audsrv_sink_term(GstAudsrvSink *sink)
{
   sink->initialized= FALSE;

   if ( sink->audsrv && sink->ownSession )
   {
      AudioServerDisconnect( sink->audsrv );
      sink->audsrv= 0;
   }
   
   gst_audsrv_sink_soc_term( sink );

   if ( sink->caps )
   {
      gst_caps_unref( sink->caps );
      sink->caps= NULL;
   }
   if ( sink->peerPad )
   {
      gst_object_unref( sink->peerPad );
      sink->peerPad= NULL;
   }
   if ( sink->sessionName )
   {
      free( sink->sessionName );
   }
}

static void
gst_audsrv_sink_dispose(GObject *object)
{
   GstAudsrvSink *sink= GST_AUDSRV_SINK(object);

   if ( sink->initialized )
   {
      gst_audsrv_sink_term( sink );
   }

   GST_AUDSRV_SINK_GET_CLASS(sink)->parent_dispose(object);
}

static void
gst_audsrv_sink_set_property(GObject *object,
    guint prop_id, const GValue *value, GParamSpec *pspec)
{
   GstAudsrvSink *sink= GST_AUDSRV_SINK(object);

   switch (prop_id) 
   {
      case PROP_MUTE:
         sink->mute= g_value_get_boolean(value);
         if ( sink->audsrv )
         {
            if ( !AudioServerMute( sink->audsrv, sink->mute) )
            {
               GST_ERROR("AudioServerMute %d failed", sink->mute);
            }
         }
         break;
      case PROP_VOLUME:
         sink->volume= g_value_get_float(value);
         if ( sink->audsrv )
         {
            if ( !AudioServerVolume( sink->audsrv, sink->volume) )
            {
               GST_ERROR("AudioServerVolume %f failed", sink->volume);
            }
         }
         break;
      case PROP_SESSION:
         sink->audsrv= (AudSrv)g_value_get_pointer(value);
         if ( sink->audsrv )
         {
            bool isPrivate= false;
            if ( AudioServerGetSessionIsPrivate( sink->audsrv, &isPrivate ) )
            {
               sink->sessionPrivate= isPrivate;
            }
            int sessionType= -1;
            if ( AudioServerGetSessionType( sink->audsrv, &sessionType ) )
            {
               sink->sessionType= sessionType;
            }
         }
         break;
      case PROP_SESSION_TYPE:
         if ( !sink->audsrv )
         {
            sink->sessionType= g_value_get_uint(value);
         }
         break;
      case PROP_SESSION_PRIVATE:
         if ( !sink->audsrv )
         {
            sink->sessionPrivate= g_value_get_boolean(value);
         }
         break;
      case PROP_SESSION_NAME:
         if ( !sink->audsrv )
         {
            const gchar *str= g_value_get_string(value);
            if ( sink->sessionName )
            {
               free(sink->sessionName);
               sink->sessionName= 0;
            }
            if ( str )
            {
               if ( strlen(str) > AUDSRV_MAX_SESSION_NAME_LEN )
               {
                  GST_ERROR("session name is too long: %d, max %d", strlen(str), AUDSRV_MAX_SESSION_NAME_LEN);
               }
               else
               {
                  sink->sessionName= strdup(str);
               }
            }
         }
         break;
      default:
         if ( !gst_audsrv_sink_soc_set_property(object, prop_id, value, pspec) )
         {
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
         }
         break;
   }
}

static void
gst_audsrv_sink_get_property(GObject *object,
    guint prop_id, GValue *value, GParamSpec* pspec)
{
   GstAudsrvSink *sink= GST_AUDSRV_SINK(object);

   switch (prop_id) 
   {
      case PROP_MUTE:
         g_value_set_boolean(value, sink->mute);
         break;
      case PROP_VOLUME:
         g_value_set_float(value, sink->volume);
         break;
      case PROP_SESSION:
         g_value_set_pointer(value, sink->audsrv);
         break;
      case PROP_SESSION_TYPE:
         g_value_set_uint(value, sink->sessionType);
         break;
      case PROP_SESSION_PRIVATE:
         g_value_set_boolean(value, sink->sessionPrivate);
         break;
      default:
         if ( !gst_audsrv_sink_soc_get_property(object, prop_id, value, pspec) )
         {
            G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
         }
         break;
   }
}

static GstStateChangeReturn
gst_audsrv_sink_change_state(GstElement *element, GstStateChange transition)
{
   GstStateChangeReturn result= GST_STATE_CHANGE_SUCCESS;
   GstAudsrvSink *sink= GST_AUDSRV_SINK(element);
   gboolean passToDefault= FALSE;
   
   GST_DEBUG_OBJECT(element, "audsrvsink: change state from %s to %s\n", 
      gst_element_state_get_name(GST_STATE_TRANSITION_CURRENT (transition)),
      gst_element_state_get_name(GST_STATE_TRANSITION_NEXT (transition)));

   sink->asyncStateChange= FALSE;

   switch (transition) 
   {
      case GST_STATE_CHANGE_NULL_TO_READY:
         if ( !gst_audsrv_sink_soc_null_to_ready( sink, &passToDefault ) )
         {
            result= GST_STATE_CHANGE_FAILURE;
         }
         break;
      case GST_STATE_CHANGE_READY_TO_PAUSED:
         sink->eosDetected= FALSE;
         if ( gst_audsrv_sink_soc_ready_to_paused( sink, &passToDefault ) )
         {
            if ( sink->readyToRender )
            {
               if ( AudioServerPlay( sink->audsrv ) )
               {
                  sink->playing= TRUE;
               }
               else
               {
                  GST_ERROR("AudioServerPlay failed");
               }
            }
            else
            {
               sink->playPending= TRUE;
            }  
         }
         else
         {
            result= GST_STATE_CHANGE_FAILURE;
         }
         break;
      case GST_STATE_CHANGE_PAUSED_TO_PLAYING:
         sink->eosDetected= FALSE;
         if ( gst_audsrv_sink_soc_paused_to_playing( sink, &passToDefault ) )
         {
            if ( sink->readyToRender )
            {
               if ( !AudioServerPause( sink->audsrv, false ) )
               {
                  GST_ERROR("AudioServerPause false failed");
               }
            }
         }
         else
         {
            result= GST_STATE_CHANGE_FAILURE;
         }
         break;
      case GST_STATE_CHANGE_PLAYING_TO_PAUSED:
         if ( gst_audsrv_sink_soc_playing_to_paused( sink, &passToDefault ) )
         {
            if ( !AudioServerPause( sink->audsrv, true ) )
            {
               GST_ERROR("AudioServerPause true failed");
            }
         }
         else
         {
            result= GST_STATE_CHANGE_FAILURE;
         }
         break;
      case GST_STATE_CHANGE_PAUSED_TO_READY:
         if ( gst_audsrv_sink_soc_paused_to_ready( sink, &passToDefault ) )
         {
            if ( sink->audsrv )
            {
               AudioServerDisableEOSDetection( sink->audsrv );
               if ( AudioServerStop( sink->audsrv ) )
               {
                  sink->playing= false;
               }
               else
               {
                  GST_ERROR("AudioServerStop failed");
               }
            }
            sink->readyToRender= false;
         }
         else
         {
            result= GST_STATE_CHANGE_FAILURE;
         }
         break;
      case GST_STATE_CHANGE_READY_TO_NULL:
         if ( !gst_audsrv_sink_soc_ready_to_null( sink, &passToDefault ) )
         {
            result= GST_STATE_CHANGE_FAILURE;
         }
         break;
      default:
         break;
   }

   if ( result == GST_STATE_CHANGE_FAILURE )
   {
      return result;
   }

   if ( passToDefault )
   {
      result= GST_ELEMENT_CLASS(parent_class)->change_state(element, transition);
   }
   
   if ( sink->asyncStateChange )
   {
      gst_element_post_message( element,
                                #ifdef USE_GST1
                                gst_message_new_async_start( GST_OBJECT_CAST(element))
                                #else
                                gst_message_new_async_start( GST_OBJECT_CAST(element), FALSE)
                                #endif
                               );
                               
      result= GST_STATE_CHANGE_ASYNC;
   }
   
   return result;
}

static gboolean
gst_audsrv_sink_start(GstBaseSink *bsink)
{
   gboolean result= FALSE;
   GstAudsrvSink *sink= GST_AUDSRV_SINK(bsink);
   int sessionType;
   
   if ( sink )
   {
      if ( !sink->audsrv )
      {
         const char *serverName= getenv("AUDSRV_NAME");
         
         sink->audsrv= AudioServerConnect( serverName );
         if ( !sink->audsrv )
         {
            GST_ERROR("failed to connect to audio server: name (%s)", (serverName ? serverName : "null") );
            goto exit;
         }
         sink->ownSession= TRUE;
         
         sessionType= sink->sessionType;
         if ( !AudioServerInitSession( sink->audsrv, sessionType, sink->sessionPrivate, sink->sessionName ) )
         {
            GST_ERROR("AudioServerInitSession failed: sessionType %d private %d name (%s)", 
                       sessionType, sink->sessionPrivate, sink->sessionName );
            goto exit;
         }
      }
      
      if ( sink->audsrv )
      {
         result= TRUE;
      }
      else
      {
         GST_ERROR("sink element has no session");
      }
   }

exit:
   
   return result;
}

static gboolean
gst_audsrv_sink_stop(GstBaseSink *bsink)
{
   GstAudsrvSink *sink= GST_AUDSRV_SINK(bsink);
   
   if ( sink )
   {
      if ( sink->audsrv && sink->ownSession )
      {
         AudioServerDisconnect( sink->audsrv );
         sink->audsrv= 0;
      }
   }
   
   return TRUE;
}

static GstFlowReturn
gst_audsrv_sink_render(GstBaseSink *bsink, GstBuffer *buffer)
{
   GstAudsrvSink *sink= GST_AUDSRV_SINK(bsink);
   #ifdef USE_GST1
   GstMapInfo map;
   #endif
   guint size;
   guint8 *data;
   
   if ( sink )
   {
      if ( !sink->readyToRender )
      {
         sink->readyToRender= gst_audsrv_sink_prepare_to_render( sink, buffer );
         GST_LOG("readyToRemder: %d\n", sink->readyToRender);
         
         if ( sink->readyToRender )
         {
            if ( !AudioServerSetAudioInfo( sink->audsrv, &sink->audioInfo ) )
            {
               GST_ERROR("AudioServerSetAudioInfo failed");
            }

            if ( sink->ownSession )
            {
               if ( !AudioServerVolume( sink->audsrv, sink->volume) )
               {
                  GST_ERROR("AudioServerVolume %f failed", sink->volume);
               }

               if ( !AudioServerMute( sink->audsrv, sink->mute) )
               {
                  GST_ERROR("AudioServerMute %d failed", sink->mute);
               }
            }
                  
            if ( sink->playPending )
            {
               if ( AudioServerPlay( sink->audsrv ) )
               {
                  sink->playing= TRUE;
                  sink->playPending= FALSE;
               }
               else
               {
                  GST_ERROR("AudioServerPlay failed");
               }
            }
         }
      }
      
      if ( sink->readyToRender && !sink->expectFakeBuffers )
      {
         #ifdef USE_GST1
         gst_buffer_map (buffer, &map, GST_MAP_READ);
         size= map.size;
         data= map.data;
         #else
         size= GST_BUFFER_SIZE(buffer);
         data= GST_BUFFER_DATA(buffer);
         #endif
         
         if ( !gst_audsrv_sink_process_buffer( sink, data, size ) )
         {
            GST_ERROR( "Error processing buffer" );
            return GST_FLOW_ERROR;
         }         

         #ifdef USE_GST1
         gst_buffer_unmap (buffer, &map);
         #endif
      }
      
      if ( sink->readyToRender )
      {
         gst_audsrv_sink_soc_render( sink, buffer );
      }
   }
   
   return GST_FLOW_OK;
}

#ifdef USE_GST1
static gboolean gst_audsrv_sink_event(GstPad *pad, GstObject *parent, GstEvent *event)
{
#else
static gboolean gst_audsrv_sink_event(GstPad *pad, GstEvent *event)
{
   GstObject *parent= gst_pad_get_parent(pad);
#endif
   GstAudsrvSink *sink= GST_AUDSRV_SINK(parent);
   gboolean result= TRUE;
   gboolean passToDefault= FALSE;

   switch (GST_EVENT_TYPE(event))
   {
      case GST_EVENT_CAPS:
         {
            GstCaps *caps;
            
            gst_event_parse_caps(event, &caps);
            
            if ( sink->caps )
            {
               gst_caps_unref( sink->caps );
               sink->caps= NULL;
            }
            sink->caps= gst_caps_copy( caps );
            if ( sink->caps )
            {
               const GstStructure *str;
               gchar *capsAsString;
               const gchar *type= 0;
               
               capsAsString= gst_caps_to_string(sink->caps);
               if ( capsAsString )
               {
                  GST_LOG( "new caps: (%s)\n", capsAsString );
                  g_free( capsAsString );
               }

               str= gst_caps_get_structure (caps, 0);
               if ( str )
               {
                  type= gst_structure_get_name( str );
                  GST_LOG("caps type (%s)\n", type);
                  if ( type )
                  {
                     gst_audsrv_sink_soc_set_type( sink, type );
                  }
               }
            }
         }
         break;
      case GST_EVENT_FLUSH_START:
         sink->isFlushing= TRUE;
         sink->eosDetected= FALSE;
         sink->haveFirstAudioTime= FALSE;
         gst_audsrv_sink_soc_flush( sink );
         if ( sink->audsrv )
         {
            AudioServerDisableEOSDetection( sink->audsrv );
            if ( !AudioServerFlush( sink->audsrv ) )
            {
               GST_ERROR("AudioServerFlush failed");
            }
         }
         passToDefault= TRUE;
         break;
      case GST_EVENT_FLUSH_STOP:
         sink->isFlushing= FALSE;
         sink->eosDetected= FALSE;
         sink->lastSyncTime= -1LL;            
         passToDefault= TRUE;
         break;
      #ifdef USE_GST1
      case GST_EVENT_SEGMENT:
      #else
      case GST_EVENT_NEWSEGMENT:
      #endif
         {
            #ifdef USE_GST1
            const GstSegment *segment;
            gst_event_parse_segment(event, &segment);
            sink->segment.format= segment->format;
            sink->segment.rate= segment->rate;
            sink->segment.applied_rate= segment->applied_rate;
            sink->segment.start= segment->start;
            sink->segment.stop= segment->stop;
            sink->segment.position= segment->position;
            sink->segment.time= segment->time;
            GST_LOG("SEGMENT format %d (%d) GST_FORMAT_TIME %d\n", segment->format, sink->segment.format, GST_FORMAT_TIME);
            GST_LOG("SEGMENT rate %f applied_rate %f\n", segment->rate, segment->applied_rate);
            GST_LOG("SEGMENT start %lld stop %lld position %lld time %lld\n", segment->start, segment->stop, segment->position, segment->time);
            #else
            gboolean update;
            gdouble rate, applied_rate;
            GstFormat format;
            gint64 start, stop, position;
            gst_event_parse_new_segment_full(event, &update, &rate, &applied_rate, &format, &start, &stop, &position);
            gst_segment_set_newsegment_full( &sink->segment, update, rate, applied_rate, format, start, stop, position);
            #endif
            
            gst_audsrv_sink_soc_segment( sink );
            
            passToDefault= TRUE;
         }
         break;
      case GST_EVENT_EOS:
         if ( !sink->eosDetected && sink->audsrv )
         {
            if ( !AudioServerEnableEOSDetection( sink->audsrv, audsrv_eos_detected, sink ) )
            {
               GST_ERROR("AudioServerEnableEOSDetection failed" );
               passToDefault= TRUE;
            }
         }
         else
         {
            passToDefault= TRUE;
         }
         break;
      default:
         passToDefault= TRUE;
         break;
   }
   
   if ( passToDefault )
   {
      if ( sink->parentEventFunc )
      {
         #ifdef USE_GST1
         result= sink->parentEventFunc(pad, parent, event);
         #else
         result= sink->parentEventFunc(pad, event);
         #endif
      }
      else
      {
         #ifdef USE_GST1
         result= gst_pad_event_default(pad, parent, event);
         #else
         result= gst_pad_event_default(pad, event);
         #endif
      }
   }
   else
   {
      gst_event_unref( event );
   }

   #ifndef USE_GST1
   gst_object_unref(sink);
   #endif
   
   return result;
}

static gboolean gst_audsrv_sink_query(GstElement *element, GstQuery *query)
{
   GstAudsrvSink *sink= GST_AUDSRV_SINK(element);

   switch (GST_QUERY_TYPE(query)) 
   {
      case GST_QUERY_POSITION:
         {
            GstFormat format;

            gst_query_parse_position(query, &format, NULL);

            if ( GST_FORMAT_BYTES == format )
            {
               return GST_ELEMENT_CLASS(parent_class)->query(element, query);
            }
            else
            {
               if ( sink->audioOnly )
               {
                  gint64 position;
                  gst_audsrv_sink_soc_update_position( sink );
                  position= sink->position;
                  GST_DEBUG_OBJECT(sink, "POSITION: %" GST_TIME_FORMAT, GST_TIME_ARGS (position));
                  gst_query_set_position(query, GST_FORMAT_TIME, position);
                  return TRUE;
               }
               else
               {
                  // Let position come from video
                  return FALSE;
               }
            }
         }
         break;

      case GST_QUERY_CUSTOM:
      case GST_QUERY_DURATION:
      case GST_QUERY_SEEKING:
      case GST_QUERY_RATE:
         if (sink->peerPad)
         {
            return gst_pad_query(sink->peerPad, query);
         }
              
      default:
         return GST_ELEMENT_CLASS(parent_class)->query (element, query);
   }
}

static gboolean gst_audsrv_sink_prepare_to_render( GstAudsrvSink *sink, GstBuffer *buffer )
{
   gboolean result= FALSE;
   if ( !sink->peerPad )
   {
      GstPad *pad= GST_BASE_SINK_PAD(sink);
      sink->peerPad= gst_pad_get_peer( pad );
   }
   result= gst_audsrv_sink_soc_prepare_to_render( sink, buffer );
   
   return result;
}

gboolean gst_audsrv_sink_process_buffer( GstAudsrvSink *sink, guint8 *data, guint size )
{
   gboolean result= FALSE;
   long long stc, now, diff;
   
   if ( sink->mayDumpPackets )
   {
      FILE *pFile= fopen("/opt/audtrigger", "rt");
      if ( pFile )
      {
         unsigned n;

         if ( fscanf( pFile, "%u", &n ) == 1 )
         {
            sink->dumpPacketLimit= n;
         }
         sink->dumpingPackets= TRUE;
         sink->dumpPacketCount= 0;            
         fclose(pFile);
         remove("/opt/audtrigger");
         remove("/opt/aud-caps.txt");
         if ( sink->caps )
         {
            pFile= fopen("/opt/aud-caps.txt", "wt");
            if ( pFile )
            {
               gchar *capsAsString= gst_caps_to_string(sink->caps);
               fprintf( pFile, "%s", capsAsString );
               g_free( capsAsString );
               fclose(pFile);
            }
         }
      }
      if ( sink->dumpingPackets )
      {
         if ( sink->dumpPacketCount < sink->dumpPacketLimit )
         {
            char work[64];
            
            sprintf( work, "/opt/aud-packet-%d.dat", ++sink->dumpPacketCount );
            pFile= fopen( work, "wb" );
            if ( pFile )
            {
               fwrite( data, 1, size, pFile );
               fclose( pFile );
            }
            printf("audsrvsink: %lld: wrote packet %d\n", getCurrentTimeMillis(), sink->dumpPacketCount );
         }
         else
         {
            sink->dumpingPackets= FALSE;
         }
      }
   }

   if ( !sink->tunnelData )
   {
      now= getCurrentTimeMicro();
      diff= now-sink->lastSyncTime;
      if ( (sink->lastSyncTime < 0) || (diff >= 1000000LL) )
      {
         stc= gst_audsrv_sink_soc_get_stc( sink );
         if ( AudioServerAudioSync( sink->audsrv, now, stc ) )
         {
            sink->lastSyncTime= now;
         }
         else
         {
            GST_ERROR("AudioServerAudioSync failed");
         }
      }
   
      if ( AudioServerAudioData( sink->audsrv, data, size ) )
      {
         result= TRUE;
      }
   }
   else
   {
      result= TRUE;
   }
   
   return result;
}

gboolean gst_audsrv_sink_process_buffer_handle( GstAudsrvSink *sink, guint64 handle )
{
   gboolean result= FALSE;

   if ( AudioServerAudioDataHandle( sink->audsrv, handle ) )
   {
      result= TRUE;
   }
   
   return result;
}

static void audsrv_eos_detected( void *userData )
{
   GstAudsrvSink *sink= (GstAudsrvSink*)userData;
   
   sink->eosDetected= TRUE;

   GstPad *pad= GST_BASE_SINK_PAD(sink);
   #ifdef USE_GST1
   GstObject *parent= gst_pad_get_parent(pad);
   gst_audsrv_sink_event(pad, parent, gst_event_new_eos());
   gst_object_unref(parent);
   #else
   gst_audsrv_sink_event(pad, gst_event_new_eos());
   #endif
}

#define readLE16( p ) ((p)[0]|((p)[1]<<8))
#define readLE32( p ) ((p)[0]|((p)[1]<<8)|((p)[2]<<16)|((p)[3]<<24))

static bool getWavInfo( unsigned char *data, int dataLen, int *codec, int *channelCount, int *sampleRate, int *bitsPerSample, int *offsetData )
{
   bool isWav= false;
   unsigned char *chunkHdr;
   unsigned char *subChunkHdr;
   unsigned char *work;
   int subChunkSize;

   if ( dataLen >= 12 )
   {
      chunkHdr= &data[0];   
      if ( 
           (strncmp( "RIFF", (const char*)&chunkHdr[0], 4 ) == 0) &&
           (strncmp( "WAVE", (const char*)&chunkHdr[8], 4 ) == 0)
         )
      {
         *offsetData= 12;
         for( ; ; )
         {
            if ( dataLen >= *offsetData+8 )
            {
               subChunkHdr= &data[*offsetData];
               *offsetData += 8;

               subChunkSize= readLE32( &subChunkHdr[4] );
               if ( strncmp( "fmt ", (const char*)&subChunkHdr[0], 4 ) == 0 )
               {
                  if ( dataLen >= *offsetData+16 )
                  {
                     work= &data[*offsetData];
                     *offsetData += subChunkSize;

                     *codec= readLE16( &work[0] );
                     *channelCount= readLE16( &work[2] );
                     *sampleRate= readLE32( &work[4] );
                     *bitsPerSample= readLE16( &work[14] );

                     isWav= true;
                  }
                  break;
               }
               else
               {
                  // skip over sub-chunk data
                  *offsetData += subChunkSize;
               }
            }
         }
      }
   }
      
   return isWav;
} 

gboolean gst_audsrv_sink_parse_wav( GstAudsrvSink *sink, GstBuffer *buffer )
{
   gboolean result= FALSE;

   if ( sink && buffer )
   {
      int wavCodec, wavChannelCount, wavSampleRate, wavBitsPerSample, wavDataOffset;

      #ifdef USE_GST1
      GstMapInfo map;
      #endif
      guint size;
      guint8 *data;

      #ifdef USE_GST1
      gst_buffer_map (buffer, &map, GST_MAP_READ);
      size= map.size;
      data= map.data;
      #else
      size= GST_BUFFER_SIZE(buffer);
      data= GST_BUFFER_DATA(buffer);
      #endif

      if ( getWavInfo( data, size, &wavCodec, &wavChannelCount, &wavSampleRate, &wavBitsPerSample, &wavDataOffset) )
      {
         sink->wavParams.codec= wavCodec;
         sink->wavParams.channelCount= wavChannelCount;
         sink->wavParams.bitsPerSample= wavBitsPerSample;
         sink->wavParams.signedData= true;
         sink->wavParams.sampleRate= wavSampleRate;

         result= TRUE;
      }

      #ifdef USE_GST1
      gst_buffer_unmap (buffer, &map);
      #endif
   }

   return result;
}

static gboolean
audsrvsink_init(GstPlugin *plugin)
{
  gst_element_register(plugin, "audsrvsink", GST_RANK_NONE,
      gst_audsrv_sink_get_type());

  return TRUE;
}

#ifndef PACKAGE
#define PACKAGE "myfirstaudsrvsink"
#endif

GST_PLUGIN_DEFINE (
    GST_VERSION_MAJOR,
    GST_VERSION_MINOR,
    #ifdef USE_GST1
    audsrvsink,
    #else
    "audsrvsink",
    #endif
    "Accepts audio data as an input to audio server",
    audsrvsink_init, 
    VERSION, 
    "LGPL", 
    PACKAGE_NAME,
    GST_PACKAGE_ORIGIN )

/** @} */
/** @} */
