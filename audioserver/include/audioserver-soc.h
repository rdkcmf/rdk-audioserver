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
* @defgroup audioserver-soc
* @{
* @defgroup audsrv
* @{
**/

#ifndef _AUDIOSERVER_SOC_H
#define _AUDIOSERVER_SOC_H

#include "audioserver.h"

typedef void* AudSrvSoc;
typedef void* AudSrvSocClient;

typedef void (*AudioServerSocFirstAudio)( void *userData );
typedef void (*AudioServerSocPTSError)( void *userData, unsigned count );
typedef void (*AudioServerSocUnderflow)( void *userData, unsigned count, unsigned bufferedBytes, unsigned queuedFrames );
typedef void (*AudioServerSocEOS)( void *userData );
typedef void (*AudioServerSocCaptureData)( void *userData, AudSrvCaptureParameters *params, unsigned char *data, int datalen );

bool AudioServerSocInit();
void AudioServerSocTerm();
AudSrvSoc AudioServerSocOpen();
void AudioServerSocClose( AudSrvSoc audsrvsoc );
bool AudioServerSocGlobalMute( AudSrvSoc audsrvsoc, bool mute );
bool AudioServerSocGlobalVolume( AudSrvSoc audsrvsoc, float volume );
AudSrvSocClient AudioServerSocOpenClient( AudSrvSoc audsrvsoc, unsigned type, bool isPrivate, const char *sessionName );
void AudioServerSocCloseClient( AudSrvSocClient audsrvsocclient );
bool AudioServerSocSetAudioInfo( AudSrvSocClient audsrvsocclient, AudSrvAudioInfo *audioInfo );
bool AudioServerSocBasetime( AudSrvSocClient audsrvsocclient, unsigned long long basetime );
bool AudioServerSocPlay( AudSrvSocClient audsrvsocclient );
bool AudioServerSocStop( AudSrvSocClient audsrvsocclient );
bool AudioServerSocPause( AudSrvSocClient audsrvsocclient, bool pause );
bool AudioServerSocFlush( AudSrvSocClient audsrvsocclient );
bool AudioServerSocAudioSync( AudSrvSocClient audsrvsocclient, unsigned stc );
bool AudioServerSocAudioData( AudSrvSocClient audsrvsocclient, unsigned char *data, unsigned len );
bool AudioServerSocAudioDataHandle( AudSrvSocClient audsrvsocclient, unsigned long long dataHandle );
bool AudioServerSocMute( AudSrvSocClient audsrvsocclient, bool mute );
bool AudioServerSocVolume( AudSrvSocClient audsrvsocclient, float volume );
bool AudioServerSocGetStatus( AudSrvSoc audsrvsoc, AudSrvSocClient audsrvsocclient, AudSrvSessionStatus *status );
void AudioServerSocEnableEOSDetection( AudSrvSocClient audsrvsocclient, AudioServerSocEOS cb, void *userData );
void AudioServerSocDisableEOSDetection( AudSrvSocClient audsrvsocclient );
void AudioServerSocSetFirstAudioFrameCallback( AudSrvSocClient audsrvsocclient, AudioServerSocFirstAudio cb, void *userData );
void AudioServerSocSetPTSErrorCallback( AudSrvSocClient audsrvsocclient, AudioServerSocPTSError cb, void *userData );
void AudioServerSocSetUnderflowCallback( AudSrvSocClient audsrvsocclient, AudioServerSocUnderflow cb, void *userData );
bool AudioServerSocSetCaptureCallback( AudSrvSocClient audsrvsocclient, const char *sessionName, AudioServerSocCaptureData cb, AudSrvCaptureParameters *params, void *userData );

#endif

/** @} */
/** @} */

