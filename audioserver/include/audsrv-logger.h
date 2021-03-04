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

#ifndef _AUDSRV_LOGGER_H
#define _AUDSRV_LOGGER_H

#include <stdarg.h>

#define INT_FATAL(FORMAT, ...)      audsrv_printf(0, "FATAL: %s:%d " FORMAT "\n", __FILE__, __LINE__, __VA_ARGS__)
#define INT_ERROR(FORMAT, ...)      audsrv_printf(0, "ERROR: %s:%d " FORMAT "\n", __FILE__, __LINE__, __VA_ARGS__)
#define INT_WARNING(FORMAT, ...)    audsrv_printf(1, "WARN: %s:%d " FORMAT "\n", __FILE__, __LINE__, __VA_ARGS__)
#define INT_INFO(FORMAT, ...)       audsrv_printf(2, "INFO: %s:%d " FORMAT "\n", __FILE__, __LINE__, __VA_ARGS__)
#define INT_DEBUG(FORMAT, ...)      audsrv_printf(3, "DEBUG: %s:%d " FORMAT "\n", __FILE__, __LINE__, __VA_ARGS__)
#define INT_TRACE1(FORMAT, ...)     audsrv_printf(4, "TRACE: %s:%d " FORMAT "\n", __FILE__, __LINE__, __VA_ARGS__)
#define INT_TRACE2(FORMAT, ...)     audsrv_printf(5, "TRACE: %s:%d " FORMAT "\n", __FILE__, __LINE__, __VA_ARGS__)
#define INT_TRACE3(FORMAT, ...)     audsrv_printf(6, "TRACE: %s:%d " FORMAT "\n", __FILE__, __LINE__, __VA_ARGS__)

#define FATAL(...)                  INT_FATAL(__VA_ARGS__, "")
#define ERROR(...)                  INT_ERROR(__VA_ARGS__, "")
#define WARNING(...)                INT_WARNING(__VA_ARGS__, "")
#define INFO(...)                   INT_INFO(__VA_ARGS__, "")
#define DEBUG(...)                  INT_DEBUG(__VA_ARGS__, "")
#define TRACE1(...)                 INT_TRACE1(__VA_ARGS__, "")
#define TRACE2(...)                 INT_TRACE2(__VA_ARGS__, "")
#define TRACE3(...)                 INT_TRACE3(__VA_ARGS__, "")

void audsrv_set_log_level( int level );

int audsrv_get_log_level();

void audsrv_printf( int level, const char *fmt, ... );

#endif

