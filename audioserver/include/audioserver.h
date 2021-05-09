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

#ifndef _AUDIOSERVER_H
#define _AUDIOSERVER_H


#ifdef __cplusplus
extern "C"
{
#endif

/**
* @defgroup audioserver
* @{
* @defgroup audsrv
* @{
**/


typedef void* AudSrv;

typedef enum _AUDSRV_SESSION_TYPE
{
   AUDSRV_SESSION_Observer= 0,
   AUDSRV_SESSION_Primary,
   AUDSRV_SESSION_Secondary,
   AUDSRV_SESSION_Effect,
   AUDSRV_SESSION_Capture
} AUDSRV_SESSIONTYPE;

typedef enum _AUDSRV_SESSION_EVENT
{
   AUDSRV_SESSIONEVENT_Removed= 0,
   AUDSRV_SESSIONEVENT_Added
} AUDSRV_SESSION_EVENT;

#define AUDSRV_MAX_SESSION_NAME_LEN (255)

typedef struct _AudSrvSessionInfo
{
   int pid;
   int sessionType;
   char sessionName[AUDSRV_MAX_SESSION_NAME_LEN+1];
} AudSrvSessionInfo;

typedef struct _AudSrvSessionStatus
{
   bool globalMuted;
   float globalVolume;
   bool ready;
   bool playing;
   bool muted;
   float volume;
   char sessionName[AUDSRV_MAX_SESSION_NAME_LEN+1];
} AudSrvSessionStatus;

#define AUDSRV_MAX_MIME_LEN (255)
typedef struct _AudSrvAudioInfo
{
   char mimeType[AUDSRV_MAX_MIME_LEN+1];
   unsigned short codec;
   unsigned short pid;
   unsigned ptsOffset;
   unsigned long long  dataId;
   unsigned long long timingId;
   unsigned long long privateId;
} AudSrvAudioInfo;

typedef struct _AudSrvCaptureParameters
{
   unsigned version;
   unsigned short numChannels;
   unsigned short bitsPerSample;
   unsigned short threshold;
   unsigned sampleRate;
   unsigned outputDelay;
   unsigned fifoSize;
} AudSrvCaptureParameters;

typedef void (*AudioServerEnumSessions)( void *userData, int result, int count, AudSrvSessionInfo *sessionInfo );
typedef void (*AudioServerSessionStatus)( void *userData, int result, AudSrvSessionStatus *sessionStatus );
typedef void (*AudioServerSessionEvent)( void *userData, int event, AudSrvSessionInfo *sessionInfo );
typedef void (*AudioServerFirstAudio)( void *userData );
typedef void (*AudioServerPTSError)( void *userData, unsigned count );
typedef void (*AudioServerUnderflow)( void *userData, unsigned count, unsigned bufferedBytes, unsigned queuedFrames );
typedef void (*AudioServerEOS)( void *userData );
typedef void (*AudioServerCapture)( void *userData, AudSrvCaptureParameters *params, unsigned char *data, int dataLen );
typedef void (*AudioServerCaptureDone)( void *userData );

/**
 * AudioServerInit
 *
 * Initialize the environment for interacting with Audio Server.
 */
bool AudioServerInit( void ); 

/**
 * AudioServerTerm
 *
 * Terminate the environment for interacting with Audio Server.
 */
void AudioServerTerm( void ); 

/**
 * AudioServerConnect
 *
 * Establish a session with an instance of Audio Server.  Specify the known name of the desired instance or NULL
 * to connect using the default name.
 */
AudSrv AudioServerConnect( const char *name );

/**
 * AudioServerDisconnect
 *
 * Tear down an existing session to an Audio Server instance.
 */
void AudioServerDisconnect( AudSrv audsrv );

/**
 * AudioServerInitSesson
 *
 * Establish the properties of a session.  The type is a value from the AUDSRV_SESSIONTYPE enum.  A private
 * session is not mixed with the main audio outputs.  The decoded audio data of a private session is 
 * available via capture.  A session name may be specified which can be used to identify a private
 * session for capture.
 */
bool AudioServerInitSession( AudSrv audsrv, int sessionType, bool isPrivate, const char *sessionName );

/**
 * AudioServerGetSessionType
 *
 * Obtain the session type of a session.
 */
bool AudioServerGetSessionType( AudSrv audsrv, int *sessionType );

/**
 * AudioServerGetSessionIsPrivate
 *
 * Determine if a session is private..
 */
bool AudioServerGetSessionIsPrivate( AudSrv audsrv, bool *sessionIsPrivate );

/**
 * AudioServerSessionAttach
 *
 * Attach an observer session to an existing named session.  While attached to an existing session, the 
 * obserer session can perform actions such as mute and volume control on the attached session. 
 */
bool AudioServerSessionAttach( AudSrv audsrv, const char *sessionName );

/**
 * AudioServerSessionDetach
 *
 * Detach from any name session that is currently attached to this observer session
 */
bool AudioServerSessionDetach( AudSrv audsrv );

/**
 * AudioServerSetAudioInfo
 *
 * Provide the parameters requried to process the audio of this session.  The values that must be provided
 * in the AudSrvAudioInfo structure are platform specific.
 */
bool AudioServerSetAudioInfo( AudSrv audsrv, AudSrvAudioInfo *info );

/**
 * AudioServerBasetime
 *
 * Set the base time for audio playback.
 */
bool AudioServerBasetime( AudSrv audsrv, long long basetime );

/**
 * AudioServerPlay
 *
 * Place the session in the playing state.
 */
bool AudioServerPlay( AudSrv audsrv );

/**
 * AudioServerStop
 *
 * Place the session in the stopped state.
 */
bool AudioServerStop( AudSrv audsrv );

/**
 * AudioServerPause
 *
 * Place the session in the paused state.
 */
bool AudioServerPause( AudSrv audsrv, bool pause );

/**
 * AudioServerFlush
 *
 * Flush any buffered audio data
 */
bool AudioServerFlush( AudSrv audsrv );

/**
 * AudioServerAudioSync
 *
 * Provide audio timing synchronization information.  This method is not currently used and may be deprecated.
 */
bool AudioServerAudioSync( AudSrv audsrv, long long nowMicros, long long stc );

/**
 * AudioServerAudioData
 *
 * Pass a chunk of audio data for playback.
 */
bool AudioServerAudioData( AudSrv audsrv, unsigned char *data, unsigned len );

/**
 * AudioServerAudioDataHandle
 *
 * Pass a chunk of audio data for playback via a platform specific handle.
 */
bool AudioServerAudioDataHandle( AudSrv audsrv, unsigned long long dataHandle );

/**
 * AudioServerGlobalMute
 *
 * Globally mute or unmute
 */
bool AudioServerGlobalMute( AudSrv audsrv, bool mute );

/**
 * AudioServerMute
 *
 * Mute or unmute the session
 */
bool AudioServerMute( AudSrv audsrv, bool mute );

/**
 * AudioServerGlobalVolume
 *
 * Globally set the volume level
 */
bool AudioServerGlobalVolume( AudSrv audsrv, float volume );

/**
 * AudioServerVolume
 *
 * Set the volume level for the session.
 */
bool AudioServerVolume( AudSrv audsrv, float volume );

/**
 * AudioServerEnumerateSessions
 *
 * Get a list of all sessions.
 */
bool AudioServerEnumerateSessions( AudSrv audsrv, AudioServerEnumSessions cb, void *userData );

/**
 * AudioServerGetSessionStatus
 *
 * Get status of session.
 */
bool AudioServerGetSessionStatus( AudSrv audsrv, AudioServerSessionStatus cb, void *userData );

/**
 * AudioServerEnableSessionEvent
 *
 * Provide a callback to be invoked when a session is added or removed.  SessionEvent callbacks may only be
 * registered by observer sessions.
 */
bool AudioServerEnableSessionEvent( AudSrv audsrv, AudioServerSessionEvent cb, void *userData );

/**
 * AudioServerDisableSessionEvent
 *
 * Cancel session event callback registration.
 */
bool AudioServerDisableSessionEvent( AudSrv audsrv );

/**
 * AudioServerEnableEOSDetection
 *
 * Provide a callback to be invoked when EOS is detected.
 */
bool AudioServerEnableEOSDetection( AudSrv audsrv, AudioServerEOS cb, void *userData );

/**
 * AudioServerDisableEOSDetection
 *
 * Cancel EOS callback registration.
 */
bool AudioServerDisableEOSDetection( AudSrv audsrv );

/**
 * AudioServerSetFirstAudioCallback
 *
 * Provide a callback to be invoked when the first audio data for a session is rendered.  Pass
 * NULL to cancel registration.
 */
void AudioServerSetFirstAudioCallback( AudSrv audsrv, AudioServerFirstAudio cb, void *userData );

/**
 * AudioServerSetPTSErrorCallback
 *
 * Provide a callback to be invoked when any PTS errors are detected during playback.  Pass
 * NULL to cancel registration.
 */
void AudioServerSetPTSErrorCallback( AudSrv audsrv, AudioServerPTSError cb, void *userData );

/**
 * AudioServerSetUnderflowCallback
 *
 * Provide a callback to be invoked when any audio data underflow errors are detected during
 * playback.  Pass NULL to cancel registration.
 */
void AudioServerSetUnderflowCallback( AudSrv audsrv, AudioServerUnderflow cb, void *userData );

/**
 * AudioServerStartCapture
 *
 * Start an audio capture session.  Provide a callback to be repeatedly invoked to pass captured
 * audio data along with the data parameters.  Pass NULL for sessionName to capture main mixed audio output 
 * or the name of a private session to capture the unmixed session data.
 */ 
bool AudioServerStartCapture( AudSrv audsrv, const char *sessionName, AudioServerCapture cb, AudSrvCaptureParameters *params, void *userData );

/**
 * AudioServerStopCapture
 *
 * End an audio capture session.  Provide a callback to be invoked to signal the end of capture.  The callback
 * provided by AudioServerStartCapture may continue to be invoked after AudioServerStopCapture is called
 * until the capture done callback is invoked.
 */
bool AudioServerStopCapture( AudSrv audsrv, AudioServerCaptureDone cb, void *userData );

/**
 * AudioServerGetSessionStatus
 *
 * Get status of session.
 */
bool AudioServerGetCaptureSessionStatus( AudSrv audsrv, const char *sessionName, AudioServerSessionStatus cb, void *userData );


/** @} */
/** @} */

#ifdef __cplusplus
} // extern "C"
#endif


#endif
