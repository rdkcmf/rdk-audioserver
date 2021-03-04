#/*
# * Copyright (C) 2016 RDK Management
# *
# * This library is free software; you can redistribute it and/or
# * modify it under the terms of the GNU Lesser General Public
# * License as published by the Free Software Foundation;
# * version 2.1 of the License.
# *
# * This library is distributed in the hope that it will be useful,
# * but WITHOUT ANY WARRANTY; without even the implied warranty of
# * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
# * Lesser General Public License for more details.
# *
# * You should have received a copy of the GNU Lesser General Public
# * License along with this library; if not, write to the Free Software
# * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
#*/

SUBDIRS =

plugin_LTLIBRARIES = libgstaudsrvsink.la

libgstaudsrvsink_la_SOURCES = gstaudsrvsink.c
libgstaudsrvsink_la_CFLAGS = $(GST_CFLAGS) -O2 -Wall -DGCC4_XXX -DRMF_OSAL_LITTLE_ENDIAN -x c++
libgstaudsrvsink_la_LDFLAGS = $(GST_LIBS) $(GSTBASE_LIBS) -L$(STAGING_DIR_TARGET)$(plugindir) -lrt -laudioserver -laudioserver-sink-soc
libgstaudsrvsink_la_LDFLAGS += -module -avoid-version
