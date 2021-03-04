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

#include "audioserver.h"

#ifndef _AUDSRV_PROTOCOL_H
#define _AUDSRV_PROTOCOL_H

/* ------------------------------------------------------------------------
 Protocol message syntax

  message format
   LEN:4 ID:4 VERSION:4 PARAMS:N
   
   LEN is number of bytes of paramter data following VERSION (N)
  
  PARAMS
   PARAM [PARAMS]
  
  PARAM
   LEN:4 TYPE:4 DATA:N
   
   LEN is number of bytes of data following TYPE (N)
  
  TYPE
  one of
    Buffer
    String
    U16
    U32
    
  Buffer
    N bytes
    
  String
    Zero terminated ASCII string followed by pad bytes to bring length to a multiple of 4
    
  U16
    Two byte unsigned bigendian value followed by two pad bytes
    
  U32
    Four byte unsigned bigendian value
  
  init
  LEN:4 ID:4 VERSION:4 SESSIONTYPE:U16 ISPRIVATE:U16 SESSIONNAME:String
  
  audio info
  LEN:4 ID:4 VERSION:4 MIME:String Codec:U16 PID:U16 PTSOffset:U32 DataId:U64 TimingId:U64 PrivateId:U64

  audio sync
  LEN:4 ID:4 VERSION:4 Time:U64 STC:U64
  
  audio data
  LEN:4 ID:4 VERSION:4 Buffer
  
  audio datahandle
  LEN:4 ID:4 VERSION:4 DataHandle:U64
 ------------------------------------------------------------------------ */

typedef enum _AUDSRV_TYPE
{
   AUDSRV_TYPE_Buffer= 0,
   AUDSRV_TYPE_String,
   AUDSRV_TYPE_U16,
   AUDSRV_TYPE_U32,
   AUDSRV_TYPE_U64,
} AUDSRV_TYPE;

typedef enum _AUDSRV_MSG
{
   AUDSRV_MSG_Init= 0,
   AUDSRV_MSG_AudioInfo,
   AUDSRV_MSG_Basetime,
   AUDSRV_MSG_Play,
   AUDSRV_MSG_Stop,
   AUDSRV_MSG_Pause,
   AUDSRV_MSG_UnPause,
   AUDSRV_MSG_Flush,
   AUDSRV_MSG_AudioSync,
   AUDSRV_MSG_AudioData,
   AUDSRV_MSG_AudioDataHandle,
   AUDSRV_MSG_Mute,
   AUDSRV_MSG_UnMute,
   AUDSRV_MSG_Volume,
   AUDSRV_MSG_EnableEOS,
   AUDSRV_MSG_DisableEOS,
   AUDSRV_MSG_StartCapture,
   AUDSRV_MSG_StopCapture,
   AUDSRV_MSG_EOSDetected,
   AUDSRV_MSG_FirstAudio,
   AUDSRV_MSG_PtsError,
   AUDSRV_MSG_Underflow,
   AUDSRV_MSG_CaptureParameters,
   AUDSRV_MSG_CaptureData,
   AUDSRV_MSG_CaptureDone,
   AUDSRV_MSG_EnumSessions,
   AUDSRV_MSG_EnumSessionsResults,
   AUDSRV_MSG_GetStatus,
   AUDSRV_MSG_GetStatusResults,
   AUDSRV_MSG_EnableSessionEvent,
   AUDSRV_MSG_DisableSessionEvent,
   AUDSRV_MSG_SessionEvent
} AUDSRV_MSG;

#define AUDSRV_MSG_HDR_LEN (4+4+4)
#define AUDSRV_MSG_TYPE_HDR_LEN (4+4)
#define AUDSRV_MSG_BUFFER_LEN(n) (n)
#define AUDSRV_MSG_STRING_LEN(s) (((strlen(s)+1)+3)&~3)
#define AUDSRV_MSG_U16_LEN (4)
#define AUDSRV_MSG_U32_LEN (4)
#define AUDSRV_MSG_U64_LEN (8)

#define AUDSRV_MSG_Init_Version (1)
#define AUDSRV_MSG_AudioInfo_Version (1)
#define AUDSRV_MSG_Basetime_Version (1)
#define AUDSRV_MSG_Play_Version (1)
#define AUDSRV_MSG_Stop_Version (1)
#define AUDSRV_MSG_Pause_Version (1)
#define AUDSRV_MSG_UnPause_Version (1)
#define AUDSRV_MSG_Flush_Version (1)
#define AUDSRV_MSG_AudioSync_Version (1)
#define AUDSRV_MSG_AudioData_Version (1)
#define AUDSRV_MSG_AudioDataHandle_Version (1)
#define AUDSRV_MSG_Mute_Version (1)
#define AUDSRV_MSG_UnMute_Version (1)
#define AUDSRV_MSG_Volume_Version (1)
#define AUDSRV_MSG_EnableEOS_Version (1)
#define AUDSRV_MSG_DisableEOS_Version (1)
#define AUDSRV_MSG_StartCapture_Version (1)
#define AUDSRV_MSG_StopCapture_Version (1)
#define AUDSRV_MSG_EOSDetected_Version (1)
#define AUDSRV_MSG_FirstAudio_Version (1)
#define AUDSRV_MSG_PtsError_Version (1)
#define AUDSRV_MSG_Underflow_Version (1)
#define AUDSRV_MSG_CaptureParameters_Version (1)
#define AUDSRV_MSG_CaptureData_Version (1)
#define AUDSRV_MSG_CaptureDone_Version (1)
#define AUDSRV_MSG_EnumSessions_Version (1)
#define AUDSRV_MSG_EnumSessionsResults_Version (1)
#define AUDSRV_MSG_GetStatus_Version (1)
#define AUDSRV_MSG_GetStatusResults_Version (1)
#define AUDSRV_MSG_EnableSessionEvent_Version (1)
#define AUDSRV_MSG_DisableSessionEvent_Version (1)
#define AUDSRV_MSG_SessionEvent_Version (1)

/* 
 * AUDSRV_MSG_Init
 *
 * LEN ID VERSION U16 U16 String
 */

/* 
 * AUDSRV_MSG_AudioInfo
 *
 * LEN ID VERSION String U16 U16 U32 U64 U64 U64
 */

/* 
 * AUDSRV_MSG_Basetime
 *
 * LEN ID VERSION U64
 */

/* 
 * AUDSRV_MSG_Play
 *
 * LEN ID VERSION
 */

/* 
 * AUDSRV_MSG_Stop
 *
 * LEN ID VERSION
 */

/* 
 * AUDSRV_MSG_Pause
 *
 * LEN ID VERSION
 */

/* 
 * AUDSRV_MSG_Flush
 *
 * LEN ID VERSION
 */

/* 
 * AUDSRV_MSG_UnPause
 *
 * LEN ID VERSION
 */

/* 
 * AUDSRV_MSG_AudioSync
 *
 * LEN ID VERSION U64 U64
 */

/* 
 * AUDSRV_MSG_AudioData
 *
 * LEN ID VERSION Buffer
 */

/* 
 * AUDSRV_MSG_AudioDataHandle
 *
 * LEN ID VERSION U64
 */

/* 
 * AUDSRV_MSG_Mute
 *
 * LEN ID VERSION global:U16 sessionName:String
 */

/* 
 * AUDSRV_MSG_UnMute
 *
 * LEN ID VERSION global:U16 sessionName:String
 */

/* 
 * AUDSRV_MSG_Volume
 *
 * LEN ID VERSION U32 U32 sessionName:String
 */

/* 
 * AUDSRV_MSG_EnableEOS
 *
 * LEN ID VERSION
 */

/* 
 * AUDSRV_MSG_DisableEOS
 *
 * LEN ID VERSION
 */

/* 
 * AUDSRV_MSG_StartCapture
 *
 * LEN ID VERSION
 */

/* 
 * AUDSRV_MSG_StopCapture
 *
 * LEN ID VERSION
 */

/* 
 * AUDSRV_MSG_EOSDetected
 *
 * LEN ID VERSION
 */

/* 
 * AUDSRV_MSG_FirstAudio
 *
 * LEN ID VERSION
 */

/* 
 * AUDSRV_MSG_PtsError
 *
 * LEN ID VERSION count:U32
 */

/* 
 * AUDSRV_MSG_Underflow
 *
 * LEN ID VERSION count:U32 buffered_bytes:U32 queued_frames:U32
 */

/* 
 * AUDSRV_MSG_CaptureParameters
 *
 * LEN ID VERSION version:U32 num_channels:U16 bits_per_sample:U16 sample_rate:U32 output_delay:U32
 */

/* 
 * AUDSRV_MSG_CaptureData
 *
 * LEN ID VERSION Buffer
 */

/* 
 * AUDSRV_MSG_CaptureDone
 *
 * LEN ID VERSION
 */

/*
 * AUDSRV_MSG_EnumSessions
 *
 * LEN ID VERSIOM token:U64
 */

/*
 * AUDSRV_MSG_EnumSessionsResults
 *
 * LEN ID VERSIOM token:U64 result:U16 session-count:U32 [pid:U32, session-type:U16, name:String]*session-count
 */

/* 
 * AUDSRV_MSG_GetStatus
 *
 * LEN ID VERSION token:U64 sessionName:String
 */

/*
 * AUDSRV_MSG_GetStatusResults
 *
 * LEN ID VERSION token:U64 result:U16 glob_muted:U16 glob_vol_num:U32 glob_vol_denom:U32 ready:U16 [playing:U16 muted:U16 vol_num:U32 vol_denom:U32 sessionName:String]
 */

/*
 * AUDSRV_MSG_EnableSessionEvent
 *
 * LEN ID VERSION
 */

/*
 * AUDSRV_MSG_DisableSessionEvent,
 *
 * LEN ID VERSION
 */

/*
 *  AUDSRV_MSG_SessionEvent
 *
 * LEN ID VERSION event:U16 pid:U32 session-type:U16 name:String
 */
 
 #endif

