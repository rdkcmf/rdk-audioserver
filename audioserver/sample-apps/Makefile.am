##########################################################################
# If not stated otherwise in this file or this component's Licenses.txt
# file the following copyright and licenses apply:
#
# Copyright 2018 RDK Management
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
# http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
##########################################################################

AUTOMAKE_OPTIONS = subdir-objects

bin_PROGRAMS = asplayer asmonitor aseffects

asplayer_SOURCES = player/asplayer.cpp
asplayer_CXXFLAGS = $(AM_CXXFLAGS) $(GST_CFLAGS) -I$(srcdir)/include
asplayer_LDFLAGS = $(AM_LDFLAGS) $(GST_LIBS) $(GSTBASE_LIBS) $(GLIB_LIBS) $(GSTAPP_LIBS) $(GMODULE_LIBS) $(LIBFFI_LIBS) $(BTRCORE_LIBS) -laudioserver


asmonitor_SOURCES = monitor/asmonitor.cpp
asmonitor_CXXFLAGS = $(AM_CXXFLAGS) $(GST_CFLAGS) -I$(srcdir)/include $(FREETYPE_CFLAGS)
asmonitor_LDFLAGS = $(AM_LDFLAGS) $(GLIB_LIBS) -lGLESv2 -lessos -laudioserver $(FREETYPE_LIBS)


aseffects_SOURCES = effects/aseffects.cpp
aseffects_CXXFLAGS = $(AM_CXXFLAGS) $(GST_CFLAGS) -I$(srcdir)/include $(FREETYPE_CFLAGS)
aseffects_LDFLAGS = $(AM_LDFLAGS) $(GST_LIBS) $(GSTBASE_LIBS) $(GLIB_LIBS) $(GSTAPP_LIBS) $(GMODULE_LIBS) $(LIBFFI_LIBS) -lGLESv2 -lessos -laudioserver $(FREETYPE_LIBS)


## IPK Generation Support
IPK_GEN_PATH = $(abs_top_builddir)/ipk
IPK_GEN_STAGING_DIR=$(abs_top_builddir)/staging_dir
IPK_GEN_INPUT  = debian-binary control.tar.gz data.tar.gz
IPK_GEN_OUTPUT = audioserver.ipk

package_ipk:
	@echo "Generating IPK Package"
	$(MAKE) prefix=${abs_top_builddir}/staging_dir install
	$(mkdir_p) $(IPK_GEN_STAGING_DIR)/usr/bin/
	cp -af $(IPK_GEN_STAGING_DIR)/bin/* $(IPK_GEN_STAGING_DIR)/usr/bin/
	tar -czvf $(IPK_GEN_PATH)/data.tar.gz -C $(IPK_GEN_STAGING_DIR) usr
	tar -czvf $(IPK_GEN_PATH)/control.tar.gz -C $(IPK_GEN_PATH) control postinst prerm
	cd $(IPK_GEN_PATH) && ar cr $(IPK_GEN_OUTPUT) $(IPK_GEN_INPUT) && cd -
	rm -rf ${abs_top_builddir}/staging_dir
	rm -rf $(abs_top_builddir)/ipk/control.tar.gz
	rm -rf $(abs_top_builddir)/ipk/data.tar.gz
	
