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
* @defgroup audsrv-conn
* @{
**/

#ifndef _AUDSRV_CONN_H
#define _AUDSRV_CONN_H

typedef struct _AudsrvConn
{
   int fdSocket;
   unsigned sendCapacity;
   unsigned char *sendbuff;
   unsigned recvCapacity;
   unsigned char *recvbuff;
   int head; // read from head
   int tail; // write to tail
   int count;
   bool peerDisconnected;
} AudsrvConn;


AudsrvConn* audsrv_conn_init( int fd, unsigned sendBufferSize, unsigned recvBufferSize );
void audsrv_conn_term( AudsrvConn *conn );
int audsrv_conn_put_u16( unsigned char *p, unsigned short n );
int audsrv_conn_put_u32( unsigned char *p, unsigned n );
int audsrv_conn_put_u64( unsigned char *p, unsigned long long n );
int audsrv_conn_put_string( unsigned char *p, const char *s );
int audsrv_conn_send( AudsrvConn *conn, unsigned char *data1, int len1, unsigned char *data2, int len2 );
void audsrv_conn_get_buffer( AudsrvConn *conn, int maxlen, unsigned char **data, unsigned *datalen );
void audsrv_conn_get_string( AudsrvConn *conn, char *s );
unsigned audsrv_conn_get_u16( AudsrvConn *conn );
unsigned audsrv_conn_get_u32( AudsrvConn *conn );
unsigned audsrv_conn_peek_u32( AudsrvConn *conn );
unsigned long long audsrv_conn_get_u64( AudsrvConn *conn );
void audsrv_conn_skip( AudsrvConn *conn, int n );
int audsrv_conn_recv( AudsrvConn *conn );

#endif


