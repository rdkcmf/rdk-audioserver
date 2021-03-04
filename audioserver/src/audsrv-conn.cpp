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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/socket.h>

#include "audsrv-conn.h"
#include "audsrv-logger.h"
#include "audsrv-protocol.h"

//#define AUDSRV_DUMP_TRAFFIC

static void dumpBuffer( unsigned char *p, int len );

AudsrvConn* audsrv_conn_init( int fd, unsigned sendBufferSize, unsigned recvBufferSize )
{
   AudsrvConn *conn= 0;
   bool error= true;
   
   conn= (AudsrvConn*)calloc( 1, sizeof(AudsrvConn) );
   if ( conn )
   {
      conn->fdSocket= fd;
      
      conn->sendbuff= (unsigned char*)malloc( sendBufferSize );
      if ( !conn->sendbuff )
      {
         ERROR("unable to allocate AudsrvConn send buffer, size %d bytes", sendBufferSize );
         goto exit;
      }
      conn->sendCapacity= sendBufferSize;
      
      conn->recvbuff= (unsigned char*)malloc( recvBufferSize );
      if ( !conn->recvbuff )
      {
         ERROR("unable to allocate AudsrvConn receive buffer, size %d bytes", recvBufferSize );
         goto exit;
      }
      conn->recvCapacity= recvBufferSize;

      error= false;
   }
   else
   {
      ERROR("unable allocate new AudsrvConn");
   }

exit:

   if ( error )
   {
      audsrv_conn_term( conn );
      conn= 0;
   }   

   return conn;
} 

void audsrv_conn_term( AudsrvConn *conn )
{
   if ( conn )
   {
      if ( conn->sendbuff )
      {
         free( conn->sendbuff );
         conn->sendbuff= 0;
      }
      if ( conn->recvbuff )
      {
         free( conn->recvbuff );
         conn->recvbuff= 0;
      }
      free( conn );
   }
}

int audsrv_conn_put_u16( unsigned char *p, unsigned short n )
{
   p[0]= (n>>8);
   p[1]= (n&0xFF);
   p[2]= 0x00;
   p[3]= 0x00;
   
   return 4;
}

int audsrv_conn_put_u32( unsigned char *p, unsigned n )
{
   p[0]= (n>>24);
   p[1]= (n>>16);
   p[2]= (n>>8);
   p[3]= (n&0xFF);
   
   return 4;
}

int audsrv_conn_put_u64( unsigned char *p, unsigned long long n )
{
   p[0]= (n>>56);
   p[1]= (n>>48);
   p[2]= (n>>40);
   p[3]= (n>>32);
   p[4]= (n>>24);
   p[5]= (n>>16);
   p[6]= (n>>8);
   p[7]= (n&0xFF);
   
   return 8;
}

int audsrv_conn_put_string( unsigned char *p, const char *s )
{
   int len= strlen(s)+1;
   
   strcpy( (char*)p, s );
   
   return ( (len+3)&~3 );
}

int audsrv_conn_send( AudsrvConn *conn, unsigned char *data1, int len1, unsigned char *data2, int len2 )
{
   int sentLen= 0;
   
   if ( data1 && len1 )
   {
      struct msghdr msg;
      struct iovec iov[2];
      int vcount= 1;

      dumpBuffer( data1, len1 );
         
      iov[0].iov_base= data1;
      iov[0].iov_len= len1;
      if ( data2 && len2 )
      {
         vcount= 2;
         iov[1].iov_base= data2;
         iov[1].iov_len= len2;
      }
      
      msg.msg_name= NULL;
      msg.msg_namelen= 0;
      msg.msg_iov= iov;
      msg.msg_iovlen= vcount;
      msg.msg_control= NULL;
      msg.msg_controllen= 0;
      msg.msg_flags= 0;

      do
      {
         sentLen= sendmsg( conn->fdSocket, &msg, 0 );
      }
      while ( (sentLen < 0) && (errno == EINTR));   
   }
   
   return sentLen;
}

void audsrv_conn_get_buffer( AudsrvConn *conn, int maxlen, unsigned char **data, unsigned *datalen )
{
   unsigned char *start;
   unsigned avail= 0;
   
   if ( conn->count )
   {
      if ( conn->head < conn->tail )
      {
         avail= conn->tail-conn->head;
      }
      else
      {
         avail= conn->recvCapacity-conn->head;
      }
      start= &conn->recvbuff[conn->head];
   }
   
   if ( avail > 0 )
   {
      if ( avail > maxlen ) avail= maxlen;
      
      *data= start;
      *datalen= avail;
      
      conn->head= ((conn->head+avail)%conn->recvCapacity);
      conn->count -= avail;
   }
   else
   {
      *data= 0;
      *datalen= 0;
   }
}

void audsrv_conn_get_string( AudsrvConn *conn, char *s )
{
   unsigned char c;
   int head= conn->head;
   int len= 0;
   int paddedLen, padCount;
   
   do
   {
      c= conn->recvbuff[head];
      head= ((head+1)%conn->recvCapacity);
      *(s++)= c;
      ++len;
   }
   while( c != 0 );
   
   paddedLen= ((len+3)&~3);
   padCount= paddedLen-len;
   head= ((head+padCount)%conn->recvCapacity);
   
   conn->head= head;
   conn->count -= paddedLen;
}

unsigned audsrv_conn_get_u16( AudsrvConn *conn )
{
   unsigned n;
   int head= conn->head;
   
   n= conn->recvbuff[head];
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   head= ((head+3)%conn->recvCapacity);

   conn->head= head;
   conn->count -= 4;
   
   return n;
}

unsigned audsrv_conn_get_u32( AudsrvConn *conn )
{
   unsigned n;
   int head= conn->head;
   
   n= conn->recvbuff[head];
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   head= ((head+1)%conn->recvCapacity);

   conn->head= head;      
   conn->count -= 4;
   
   return n;
}

unsigned audsrv_conn_peek_u32( AudsrvConn *conn )
{
   unsigned n;
   int head= conn->head;
   
   n= conn->recvbuff[head];
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   
   return n;
}

unsigned long long audsrv_conn_get_u64( AudsrvConn *conn )
{
   unsigned long long n;
   int head= conn->head;
   
   n= conn->recvbuff[head];
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   head= ((head+1)%conn->recvCapacity);
   n= ((n << 8) | conn->recvbuff[head]);
   head= ((head+1)%conn->recvCapacity);

   conn->head= head;      
   conn->count -= 8;
   
   return n;
}

void audsrv_conn_skip( AudsrvConn *conn, int n )
{
   conn->head= ((conn->head+n)%conn->recvCapacity);
   conn->count -= n;
}

int audsrv_conn_recv( AudsrvConn *conn )
{
   struct msghdr msg;
   struct iovec iov[2];
   int vcount;
   int len= -1;
   
   if ( !conn->peerDisconnected && (conn->count < conn->recvCapacity) )
   {
      vcount= 1;
      if ( conn->tail < conn->head )
      {
         iov[0].iov_base= &conn->recvbuff[conn->tail];
         iov[0].iov_len= conn->head-conn->tail;
      }
      else if ( conn->tail >= conn->head )
      {
         iov[0].iov_base= &conn->recvbuff[conn->tail];
         iov[0].iov_len= conn->recvCapacity-conn->tail;
         if ( conn->head )
         {
            vcount= 2;
            iov[1].iov_base= &conn->recvbuff[0];
            iov[1].iov_len= conn->head;
         }
      }
      
      msg.msg_name= NULL;
      msg.msg_namelen= 0;
      msg.msg_iov= iov;
      msg.msg_iovlen= vcount;
      msg.msg_control= NULL;
      msg.msg_controllen= 0;
      msg.msg_flags= 0;

      do
      {
         len= recvmsg( conn->fdSocket, &msg, 0 );
      }
      while ( (len < 0) && (errno == EINTR));
      
      if ( len > 0 )
      {
         conn->count += len;
         conn->tail= ((conn->tail + len) % conn->recvCapacity);
      }
      else
      {
         conn->peerDisconnected= true;
      }
   }
   
   return len;
}

static void dumpBuffer( unsigned char *p, int len )
{
   #ifdef AUDSRV_DUMP_TRAFFIC
   int i, c, col;
   
   col= 0;
   for( i= 0; i < len; ++i )
   {
      if ( col == 0 ) printf("%04X: ", i);
       
      c= p[i];
      
      printf("%02X ", c);
      
      if ( col == 7 ) printf( " - " );
      
      if ( col == 15 ) printf( "\n" );
      
      ++col;
      if ( col >= 16 ) col= 0;      
   }
   
   if ( col > 0 ) printf("\n");
   #else
   
   (void)(p);
   (void)(len);
   
   #endif
}

/** @} */
/** @} */

