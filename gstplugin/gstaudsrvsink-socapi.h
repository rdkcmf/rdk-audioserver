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


#ifndef __GST_AUDSRVSINK_SOCAPI_H__
#define __GST_AUDSRVSINK_SOCAPI_H__

void gst_audsrv_sink_soc_class_init(GstAudsrvSinkClass *klass);
gboolean gst_audsrv_sink_soc_init( GstAudsrvSink *sink );
void gst_audsrv_sink_soc_term( GstAudsrvSink *sink );
gboolean gst_audsrv_sink_soc_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec);
gboolean gst_audsrv_sink_soc_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec);
void gst_audsrv_sink_soc_set_type( GstAudsrvSink *sink, const gchar *type );
gboolean gst_audsrv_sink_soc_prepare_to_render( GstAudsrvSink *sink, GstBuffer *buffer );
gboolean gst_audsrv_sink_soc_null_to_ready( GstAudsrvSink *sink, gboolean *passToDefault );
gboolean gst_audsrv_sink_soc_ready_to_paused( GstAudsrvSink *sink, gboolean *passToDefault );
gboolean gst_audsrv_sink_soc_paused_to_playing( GstAudsrvSink *sink, gboolean *passToDefault );
gboolean gst_audsrv_sink_soc_playing_to_paused( GstAudsrvSink *sink, gboolean *passToDefault );
gboolean gst_audsrv_sink_soc_paused_to_ready( GstAudsrvSink *sink, gboolean *passToDefault );
gboolean gst_audsrv_sink_soc_ready_to_null( GstAudsrvSink *sink, gboolean *passToDefault );
long long gst_audsrv_sink_soc_get_stc( GstAudsrvSink *sink );
void gst_audsrv_sink_soc_update_position( GstAudsrvSink *sink );
void gst_audsrv_sink_soc_render( GstAudsrvSink *sink, GstBuffer *buffer );
void gst_audsrv_sink_soc_segment( GstAudsrvSink *sink );
void gst_audsrv_sink_soc_flush( GstAudsrvSink *sink );

#endif
