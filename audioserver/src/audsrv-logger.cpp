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
* @defgroup audioserver
* @{
* @defgroup audsrv-logger
* @{
**/

#include <stdlib.h>
#include <stdio.h>
#include <sys/time.h>

#include "audsrv-logger.h"

static int g_activeLevel= 2;

static long long getCurrentTimeMillis(void)
{
   struct timeval tv;
   long long utcCurrentTimeMillis;

   gettimeofday(&tv,0);
   utcCurrentTimeMillis= tv.tv_sec*1000LL+(tv.tv_usec/1000LL);

   return utcCurrentTimeMillis;
}

void audsrv_set_log_level( int level )
{
   g_activeLevel= level;
}

int audsrv_get_log_level()
{
   return g_activeLevel;
}

void audsrv_printf( int level, const char *fmt, ... )
{
   if ( level <= g_activeLevel )
   {
      va_list argptr;
      printf("%lld: ", getCurrentTimeMillis());
      va_start( argptr, fmt );
      vprintf( fmt, argptr );
      va_end( argptr );
   }
}

/** @} */
/** @} */

