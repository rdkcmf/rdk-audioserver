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


#ifndef __GST_AUDSRVSINK_H__
#define __GST_AUDSRVSINK_H__

#include <gst/gst.h>
#include <gst/base/gstbasesink.h>

#include "audioserver.h"

G_BEGIN_DECLS

#define GST_TYPE_AUDSRV_SINK \
  (gst_audsrv_sink_get_type())
#define GST_AUDSRV_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj),GST_TYPE_AUDSRV_SINK,GstAudsrvSink))
#define GST_AUDSRV_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST((klass),GST_TYPE_AUDSRV_SINK,GstAudsrvSinkClass))
#define GST_AUDSRV_SINK_GET_CLASS(obj)     (G_TYPE_INSTANCE_GET_CLASS ((obj), GST_TYPE_AUDSRV_SINK, GstAudsrvSinkClass))
#define GST_IS_AUDSRV_SINK(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE((obj),GST_TYPE_AUDSRV_SINK))
#define GST_IS_AUDSRV_SINK_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE((klass),GST_TYPE_AUDSRV_SINK))

typedef struct _GstAudsrvSink GstAudsrvSink;
typedef struct _GstAudsrvSinkClass GstAudsrvSinkClass;

#define AUDSRV_UNUSED( x ) ((void)x)

#define PROP_SOC_BASE (100)

#include "gstaudsrvsink-soc.h"

typedef struct _GstAudsrvWavParams
{
   gint codec;
   gint channelCount;
   gint sampleRate;
   gint bitsPerSample;
   bool signedData;
} GstAudsrvWavParams;

struct _GstAudsrvSink
{
   GstBaseSink parent;
   GstPadEventFunction parentEventFunc;
   GstPadQueryFunction parentQueryFunc;
   GstCaps *caps;
   gboolean initialized;
   gboolean mayDumpPackets;
   gboolean dumpingPackets;
   int dumpPacketCount;
   int dumpPacketLimit;
   gboolean asyncStateChange;
   gboolean ownSession;
   guint sessionType;
   gboolean sessionPrivate;
   gchar *sessionName;
   gboolean readyToRender;
   gboolean playing;
   gboolean playPending;
   gboolean expectFakeBuffers;
   gboolean tunnelData;
   GstPad *peerPad;
   gboolean audioOnly;
   gboolean isFlushing;
   gboolean eosDetected;

   GstAudsrvWavParams wavParams;
   
   gboolean mute;
   float volume;
   long long lastSyncTime;
   gboolean haveFirstAudioTime;
   long long firstAudioTime;
   GstSegment segment;
   gint64 position;
   
   AudSrv audsrv;
   AudSrvAudioInfo audioInfo;

   struct _GstAudsrvSinkSoc soc;
};

typedef void (*dispose_func)(GObject *object);

struct _GstAudsrvSinkClass
{
  GstBaseSinkClass parent_class;
  dispose_func parent_dispose;
};

GType gst_audsrv_sink_get_type (void);

gboolean gst_audsrv_sink_process_buffer( GstAudsrvSink *sink, guint8 *data, guint size );
gboolean gst_audsrv_sink_process_buffer_handle( GstAudsrvSink *sink, guint64 handle );
gboolean gst_audsrv_sink_parse_wav( GstAudsrvSink *sink, GstBuffer *buffer );

G_END_DECLS

#endif /* __GST_AUDSRVSINK_H__ */


/** @} */
/** @} */
