/*
 * If not stated otherwise in this file or this component's Licenses.txt file the
 * following copyright and licenses apply:
 *
 * Copyright 2018 RDK Management
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

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include <signal.h>
#include <sys/time.h>
#include <unistd.h>

#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H

#include "audioserver.h"
#include "essos.h"

#include <map>
#include <vector>
#include <string>

#define FONT_SIZE (24)
#define FONT_LEADING (2)

#define WINDOW_BORDER_COLOR (0xFFFFFFFF)
#define WINDOW_BACKGROUND_COLOR (0x80404040)
#define WINDOW_TITLEBAR_COLOR (0xFF404040)
#define SELECTED_BACKGROUND_COLOR (0x60F0F080)
#define SELECTED_TEXT_COLOR (0xFF101010)
#define NORMAL_TEXT_COLOR (0xFFFFFFFF)

#define STATUS_PERIOD (1000)

class DrawableTextLine;
class Session;
class SessionList;

typedef struct _AppGlyph
{
   int bitmapLeft;
   int bitmapTop;
   int bitmapWidth;
   int bitmapHeight;
   int advanceX;
   int advanceY;
   float textureX1;
   float textureY1;
   float textureX2;
   float textureY2;
} AppGlyph;

class AppFont
{
   public:
   bool italic;
   int fontHeight;
   int fontAscent;
   int fontDescent;
   std::map<int,AppGlyph> *glyphMap;
   int atlasWidth;
   int atlasHeight;
   GLuint atlasTextureId;
};

typedef struct AppCtx_
{
   EssCtx *essCtx;
   int windowX;
   int windowY;
   int windowWidth;
   int windowHeight;
   int borderWidth;
   int clientX;
   int clientY;
   int clientWidth;
   int clientHeight;
   bool dirty;
   GLuint shaderVertFill;
   GLuint shaderFragFill;
   GLuint progFill;
   GLuint attrFillVertex;
   GLuint uniFillMatrix;  
   GLuint uniFillTarget;
   GLuint uniFillColor;
   bool haveFT;
   bool haveFace;
   FT_Library ft;
   FT_Face ftFace;
   AppFont font;
   GLuint shaderVertGlyph;
   GLuint shaderFragGlyph;
   GLuint progGlyph;
   GLuint attrGlyphVertex;
   GLuint attrGlyphTextureVertex;
   GLuint uniGlyphMatrix;  
   GLuint uniGlyphTarget;
   GLuint uniGlyphColor;
   GLuint uniGlyphAlpha;
   GLuint uniGlyphTexture;
   DrawableTextLine *title;
   SessionList *sessionList;
   Session *sessionSelected;
   AudSrv audsrvObserver;
   long long nextStatusTime;
   bool globalMuted;
   float globalVolume;
   DrawableTextLine *labelGlobal;
   int globalMutedX;
   int globalMutedY;
   int globalMutedW;
   int globalMutedH;
   DrawableTextLine *fieldGlobalMuted;
   int globalVolumeDownX;
   int globalVolumeDownY;
   int globalVolumeDownW;
   int globalVolumeDownH;
   DrawableTextLine *labelGlobalVolumeDown;
   DrawableTextLine *fieldGlobalVolume;
   int globalVolumeUpX;
   int globalVolumeUpY;
   int globalVolumeUpW;
   int globalVolumeUpH;
   DrawableTextLine *labelGlobalVolumeUp;
   bool showSession;
   bool sessionMuted;
   float sessionVolume;
   DrawableTextLine *labelSession;
   int sessionMutedX;
   int sessionMutedY;
   int sessionMutedW;
   int sessionMutedH;
   DrawableTextLine *fieldSessionMuted;
   int sessionVolumeDownX;
   int sessionVolumeDownY;
   int sessionVolumeDownW;
   int sessionVolumeDownH;
   DrawableTextLine *labelSessionVolumeDown;
   DrawableTextLine *fieldSessionVolume;
   int sessionVolumeUpX;
   int sessionVolumeUpY;
   int sessionVolumeUpW;
   int sessionVolumeUpH;
   DrawableTextLine *labelSessionVolumeUp;
} AppCtx;

static void signalHandler(int signum);
static void terminated( void * );
static void keyPressed( void *userData, unsigned int key );
static void keyReleased( void *userData, unsigned int );
static void pointerMotion( void *userData, int, int );
static void pointerButtonPressed( void *userData, int button, int x, int y );
static void pointerButtonReleased( void *userData, int, int, int );
static bool createShaders( AppCtx *ctx,
                           const char *vertSrc, GLuint *vShader,
                           const char *fragSrc, GLuint *fShader );
static bool linkProgram( AppCtx *ctx, GLuint prog );
static bool setupFill( AppCtx *ctx );
static bool setupGL( AppCtx *ctx );
static bool initAppFont( AppCtx *ctx, AppFont *font, bool italic );
static void termAppFont( AppCtx *ctx, AppFont *font );
static bool initText( AppCtx *ctx );
static void termText( AppCtx *ctx );
static void fillRect( AppCtx *ctx, int x, int y, int w, int h, unsigned argb );
static bool initTitle( AppCtx *ctx, std::string title );
static void termTitle( AppCtx *ctx );
static void enumerateSessionsCallback( void *userData, int result, int count, AudSrvSessionInfo *sessionInfo );
static bool initAudioServer( AppCtx *ctx );
static void termAudioServer( AppCtx *ctx );
static void toggleSessionSelect( AppCtx *ctx, Session *session );
static void updateStatus( AppCtx *ctx );
static void drawWindowBackground( AppCtx *ctx );
static void drawTitle( AppCtx *ctx );
static void drawGlobalStatus( AppCtx *ctx );
static void drawSessionStatus( AppCtx *ctx );
static bool renderGL( AppCtx *ctx );
static void showUsage();

static bool gRunning;
static int gDisplayWidth;
static int gDisplayHeight;

static void signalHandler(int signum)
{
   printf("signalHandler: signum %d\n", signum);
   gRunning= false;
}

static long long getCurrentTimeMillis(void)
{
   struct timeval tv;
   long long utcCurrentTimeMillis;

   gettimeofday(&tv,0);
   utcCurrentTimeMillis= tv.tv_sec*1000LL+(tv.tv_usec/1000LL);

   return utcCurrentTimeMillis;
}

class DrawableTextLine
{
   public:
      DrawableTextLine( AppCtx *ctx )
        : ctx(ctx)
      {
         font= &ctx->font;
         centered= false;
         argb= 0xFFFFFFFF;
         dirty= true;
      }
     ~DrawableTextLine(){}
      void draw();
      void setCentered( bool centered )
      {
         this->centered= centered;
         setDirty();
      }
      void setColor( unsigned int argb )
      {
         this->argb= argb;
      }
      void setText( std::string text )
      {
         this->text= text;
         setDirty();
      }
      void setPosition( int x, int y )
      {
         this->x= x;
         this->y= y;
         setDirty();
      }
      void setBounds( int width, int height )
      {
         this->width= width;
         this->height= height;
      }
      void setDirty()
      {
         this->dirty= true;
         ctx->dirty= true;
      }
      void getRect( int &x, int &y, int &w, int &h )
      {
         x= this->x;
         y= this->y;
         w= this->width;
         h= this->height;
      }
      
   private:
      bool prepare( std::string str, int x, int y );

      AppCtx *ctx;
      AppFont *font;
      int x;
      int y;
      int width;
      int height;
      int numRects;
      std::vector<float> vert;
      std::vector<float> uv;
      int boxX1, boxY1, boxX2, boxY2;
      unsigned int argb;
      std::string text;
      bool centered;
      bool dirty;
};

bool DrawableTextLine::prepare( std::string str, int x, int y )
{
   float cx, cy, width;
   float vx1, vy1, vx2, vy2;
   int codePoint;
   int len= str.length();
   width= 0;
   cx= x;
   cy= y;
   boxX1= cx;
   boxY1= ctx->windowHeight;
   boxY2= 0;
   numRects= 0;
   vert.clear();
   uv.clear();
   if ( centered )
   {
      for( int i= 0; i < len; ++i )
      {
         codePoint= str.at(i);
         std::map<int,AppGlyph>::iterator it= font->glyphMap->find( codePoint );
         if ( it != font->glyphMap->end() )
         {
            AppGlyph *g= &it->second;
            if ( g )
            {
               width += (g->advanceX>>6);
            }
         }
      }
      cx= x + (this->width-width)/2;
      float h= ctx->font.fontHeight;
      if ( height > h )
      {
         float adjust= (height-h)/2;
         cy= y+adjust+ctx->font.fontAscent;
      }
   }
   else
   {
      cy= y+ctx->font.fontAscent;
   }

   boxX1= cx;
   boxY1= ctx->windowHeight;
   boxY2= 0;
   for( int i= 0; i < len; ++i )
   {
      codePoint= str.at(i);
      std::map<int,AppGlyph>::iterator it= font->glyphMap->find( codePoint );
      if ( it != font->glyphMap->end() )
      {
         AppGlyph *g= &it->second;
         if ( g )
         {
            if ( g->bitmapWidth && g->bitmapHeight )
            {
               vx1= cx+g->bitmapLeft;
               vy1= cy-g->bitmapTop;
               vx2= vx1+g->bitmapWidth;
               vy2= vy1+g->bitmapHeight;
               if (vy1 < boxY1) boxY1= vy1;
               if (vy2 > boxY2) boxY2= vy2;
               vert.push_back(vx1);
               vert.push_back(vy1);
               vert.push_back(vx2);
               vert.push_back(vy2);
               vert.push_back(vx1);
               vert.push_back(vy2);
               vert.push_back(vx1);
               vert.push_back(vy1);
               vert.push_back(vx2);
               vert.push_back(vy1);
               vert.push_back(vx2);
               vert.push_back(vy2);
               uv.push_back(g->textureX1);
               uv.push_back(g->textureY1);
               uv.push_back(g->textureX2);
               uv.push_back(g->textureY2);
               uv.push_back(g->textureX1);
               uv.push_back(g->textureY2);
               uv.push_back(g->textureX1);
               uv.push_back(g->textureY1);
               uv.push_back(g->textureX2);
               uv.push_back(g->textureY1);
               uv.push_back(g->textureX2);
               uv.push_back(g->textureY2);
               //printf("%c: (%f, %f, %f, %f) (%f, %f, %f, %f)\n", codePoint, vx1, vy1, vx2, vy2, g->textureX1, g->textureY1, g->textureX2, g->textureY2);
               ++numRects;
            }
            cx += (g->advanceX>>6);
         }
      }
   }
   boxX2= cx;

   boxX1 -= 4;
   boxX2 += (font->italic ? 8 : 4);
   boxY1 -= 4;
   boxY2 += 6;

   int nominalHeight= font->fontHeight-10;
   if ( boxY2-boxY1 < nominalHeight )
   {
      boxY2= boxY1+nominalHeight;
   }
}

void DrawableTextLine::draw()
{
   if ( dirty )
   {
      prepare( text, x, y );
      dirty= false;
   }

   float color[4]=
   {
      ((float)((argb>>16)&0xFF))/255.0f,
      ((float)((argb>>8)&0xFF))/255.0f,
      ((float)((argb)&0xFF))/255.0f,
      ((float)((argb>>24)&0xFF))/255.0f,
   };

   const float identityMatrix[4][4]=
   {
      {1, 0, 0, 0},
      {0, 1, 0, 0},
      {0, 0, 1, 0},
      {0, 0, 0, 1}
   };

   glUseProgram( ctx->progGlyph );
   glUniformMatrix4fv( ctx->uniGlyphMatrix, 1, GL_FALSE, (GLfloat*)identityMatrix );
   glUniform2f( ctx->uniGlyphTarget, ctx->windowWidth, ctx->windowHeight );
   glUniform4fv( ctx->uniGlyphColor, 1, color );
   glUniform1f( ctx->uniGlyphAlpha, 1.0f);

   glActiveTexture( GL_TEXTURE0 ); 
   glBindTexture( GL_TEXTURE_2D, font->atlasTextureId );
   glUniform1i( ctx->uniGlyphTexture, 0 );
   glVertexAttribPointer( ctx->attrGlyphVertex, 2, GL_FLOAT, GL_FALSE, 0, &vert[0] );
   glVertexAttribPointer( ctx->attrGlyphTextureVertex, 2, GL_FLOAT, GL_FALSE, 0, &uv[0] );
   glEnableVertexAttribArray( ctx->attrGlyphVertex );
   glEnableVertexAttribArray( ctx->attrGlyphTextureVertex );
   glDrawArrays(GL_TRIANGLES, 0, 6*numRects);
   glDisableVertexAttribArray( ctx->attrGlyphVertex );
   glDisableVertexAttribArray( ctx->attrGlyphTextureVertex );
}

class Session
{
   public:
      Session( AppCtx *ctx )
        : ctx(ctx), pidField(ctx), typeField(ctx), nameField(ctx)
      {
         memset( &this->info, 0, sizeof(AudSrvSessionInfo) );
         setSelected(false);
      }
     ~Session()
      {
      }
      void setInfo( AudSrvSessionInfo *info );
      int sessionType()
      {
         return info.sessionType;
      }
      void setSelected( bool selected )
      {
         this->selected= selected;
         unsigned int color= selected ? SELECTED_TEXT_COLOR : NORMAL_TEXT_COLOR;
         pidField.setColor( color );
         typeField.setColor( color );
         nameField.setColor( color );
      }
      char *name()
      {
         return info.sessionName;
      }
      void draw();
      bool match( AudSrvSessionInfo *info )
      {
         bool match= ((info->pid == this->info.pid) &&
                      (info->sessionType == this->info.sessionType) &&
                      !strcmp(info->sessionName, this->info.sessionName) );
         return match;
      }
      bool hitTest( int x, int y )
      {
         bool hit= ((x > boundsX) && (x < boundsX+boundsW) &&
                    (y > boundsY) && (y < boundsY+boundsH) );
         return hit;
      }
      bool prepare( int x, int y );
   private:
      AppCtx *ctx;
      AudSrvSessionInfo info;
      bool selected;
      int boundsX;
      int boundsY;
      int boundsW;
      int boundsH;
      DrawableTextLine pidField;
      DrawableTextLine typeField;
      DrawableTextLine nameField;
};

void Session::setInfo( AudSrvSessionInfo *info )
{
   char work[24];
   
   this->info= *info;
   sprintf( work, "%d", info->pid );
   pidField.setText( work );
   switch( info->sessionType )
   {
      case AUDSRV_SESSION_Primary:
         typeField.setText("primary");
         break;
      case AUDSRV_SESSION_Secondary:
         typeField.setText("secondary");
         break;
      case AUDSRV_SESSION_Effect:
         typeField.setText("effect");
         break;
      case AUDSRV_SESSION_Capture:
         typeField.setText("capture");
         break;
      default:
         break;
   }
   nameField.setText( info->sessionName );
}

void Session::draw()
{
   if ( selected )
   {
      fillRect( ctx, boundsX, boundsY, boundsW, boundsH, SELECTED_BACKGROUND_COLOR );
   }
   pidField.draw();
   typeField.draw();
   nameField.draw();
}

bool Session::prepare( int x, int y )
{
   boundsX= x;
   boundsY= y;
   boundsW= ctx->clientWidth;
   boundsH= ctx->font.fontHeight;

   pidField.setPosition( x, y );
   pidField.setBounds( ctx->clientWidth/5, ctx->font.fontHeight );
   
   typeField.setPosition( x+ctx->clientWidth/5, y );
   typeField.setBounds( ctx->clientWidth/5, ctx->font.fontHeight );

   nameField.setPosition( x+2*ctx->clientWidth/5, y );
   nameField.setBounds( 3*ctx->clientWidth/5, ctx->font.fontHeight );
}

class SessionList
{
   public:
      SessionList( AppCtx *ctx )
        : ctx(ctx), dirty(true)
      {
      }
     ~SessionList()
      {
      }
      bool addSession( AudSrvSessionInfo *info );
      void removeSession( AudSrvSessionInfo *info );
      void draw();
      Session *selectFromPoint( int x, int y );
      
   private:
      AppCtx *ctx;
      bool dirty;
      std::vector<Session*> session;
};

bool SessionList::addSession( AudSrvSessionInfo *info )
{
   bool result= false;
   Session *newSession= new Session( ctx );
   if ( newSession )
   {
      newSession->setInfo( info );
      session.push_back( newSession );
      dirty= true;
      ctx->dirty= true;
   }
   return result;
}

void SessionList::removeSession( AudSrvSessionInfo *info )
{
   for( std::vector<Session*>::iterator it= session.begin();
        it != session.end();
        ++it )
   {
      Session *sessionTry= (Session*)(*it);
      if ( sessionTry->match( info ) )
      {
         session.erase(it);
         delete sessionTry;
         if ( ctx->sessionSelected == sessionTry )
         {
            ctx->sessionSelected= 0;
            ctx->showSession= false;
         }
         dirty= true;
         ctx->dirty= true;
         break;
      }
   }
}

void SessionList::draw()
{
   if ( dirty )
   {
      int x= ctx->clientX+6;
      int y= ctx->clientY;
      
      dirty= false;      
      for( std::vector<Session*>::iterator it= session.begin();
           it != session.end();
           ++it )
      {
         Session *session= (Session*)(*it);
         session->prepare( x, y );
         y += ctx->font.fontHeight;
      }
   }

   for( std::vector<Session*>::iterator it= session.begin();
        it != session.end();
        ++it )
   {
      Session *session= (Session*)(*it);
      session->draw();
   }   
}

Session *SessionList::selectFromPoint( int x, int y )
{
   Session *sessionHit= 0;

   for( std::vector<Session*>::iterator it= session.begin();
        it != session.end();
        ++it )
   {
      Session *sessionTry= (Session*)(*it);
      if ( sessionTry->hitTest( x, y ) )
      {
         sessionHit= sessionTry;
         break;
      }
   }

   return sessionHit;
}

static const char *vertFillSrc=
  "uniform mat4 matrix;\n"
  "uniform vec2 targetSize;\n"
  "attribute vec2 vertex;\n"
  "void main()\n"
  "{\n"
  "  vec4 pos= matrix * vec4(vertex, 0, 1);\n"
  "  vec4 scale= vec4( targetSize, 1, 1) * vec4( 0.5, -0.5, 1, 1);\n"
  "  pos= pos / scale;\n"
  "  pos= pos - vec4( 1, -1, 0, 0 );\n"
  "  gl_Position=  pos;\n"
  "}\n";

static const char *fragFillSrc=
  "#ifdef GL_ES\n"
  "precision mediump float;\n"
  "#endif\n"
  "uniform vec4 color;\n"
  "void main()\n"
   "{\n"
   "  gl_FragColor= color;\n"
   "}\n";

static const char *vertexShaderGlyph=
  "uniform mat4 matrix;\n"
  "uniform vec2 targetSize;\n"
  "attribute vec2 vertex;\n"
  "attribute vec2 textureVertex;\n"
  "varying vec2 uv;\n"
  "void main()\n"
  "{\n"
  "  vec4 pos= matrix * vec4(vertex, 0, 1);\n"
  "  vec4 scale= vec4( targetSize, 1, 1) * vec4( 0.5, -0.5, 1, 1);\n"
  "  pos= pos / scale;\n"
  "  pos= pos - vec4( 1, -1, 0, 0 );\n"
  "  gl_Position=  pos;\n"
  "  uv= textureVertex;\n"
  "}\n";

static const char *fragmentShaderGlyph=
  "#ifdef GL_ES\n"
  "precision mediump float;\n"
  "#endif\n"
  "uniform float alpha;\n"
  "uniform vec4 color;\n"
  "uniform sampler2D texture;\n"
  "varying vec2 uv;\n"
  "void main()\n"
  "{\n"
  "  float a= alpha*texture2D(texture, uv).a;\n"
  "  gl_FragColor= color*a;\n"
  "}\n";

static bool createShaders( AppCtx *ctx,
                           const char *vertSrc, GLuint *vShader,
                           const char *fragSrc, GLuint *fShader )
{
   bool result= false;
   int pass;
   GLint status;
   GLuint shader, type, vshader, fshader;
   GLsizei length;
   const char *src, *typeName;
   char log[1000];

   for( pass= 0; pass < 2; ++pass )
   {
      type= (pass == 0) ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
      src= (pass == 0) ? vertSrc : fragSrc;
      typeName= (pass == 0) ? "vertex" : "fragment";

      shader= glCreateShader( type );
      if ( !shader )
      {
         printf("Error: glCreateShader failed for %s shader\n", typeName);
         goto exit;
      }

      glShaderSource( shader, 1, (const char **)&src, NULL );
      glCompileShader( shader );
      glGetShaderiv( shader, GL_COMPILE_STATUS, &status );
      if ( !status )
      {
         glGetShaderInfoLog( shader, sizeof(log), &length, log );
         printf("Error compiling %s shader: %*s\n",
                typeName,
                length,
                log );
      }

      if ( pass == 0 )
         *vShader= shader;
      else
         *fShader= shader;
   }

   result= true;

exit:
   
   return result;
}

static bool linkProgram( AppCtx *ctx, GLuint prog )
{
   bool result= false;
   GLint status;

   glLinkProgram(prog);
   glGetProgramiv(prog, GL_LINK_STATUS, &status);
   if (!status)
   {
      char log[1000];
      GLsizei len;
      glGetProgramInfoLog(prog, 1000, &len, log);
      printf("Error: linking:\n%*s\n", len, log);
      goto exit;
   }

   result= true;

exit:
   return result;
}

static bool setupFill( AppCtx *ctx )
{
   bool result= true;

   if ( !createShaders( ctx, vertFillSrc, &ctx->shaderVertFill, fragFillSrc, &ctx->shaderFragFill ) )
   {
      goto exit;
   }

   ctx->progFill= glCreateProgram();
   glAttachShader( ctx->progFill, ctx->shaderVertFill );
   glAttachShader( ctx->progFill, ctx->shaderFragFill );

   ctx->attrFillVertex= 0;
   glBindAttribLocation(ctx->progFill, ctx->attrFillVertex, "vertex");

   if ( !linkProgram( ctx, ctx->progFill ) )
   {
      goto exit;
   }

   ctx->uniFillMatrix= glGetUniformLocation( ctx->progFill, "matrix" );
   if ( ctx->uniFillTarget == -1 )
   {
      printf("Error: uniform 'natrix' error\n");
      goto exit;
   }

   ctx->uniFillTarget= glGetUniformLocation( ctx->progFill, "targetSize" );
   if ( ctx->uniFillTarget == -1 )
   {
      printf("Error: uniform 'targetSize' error\n");
      goto exit;
   }

   ctx->uniFillColor= glGetUniformLocation( ctx->progFill, "color" );
   if ( ctx->uniFillColor == -1 )
   {
      printf("Error: uniform 'color' error\n");
      goto exit;
   }

exit:
   return result;
}

static bool setupGL( AppCtx *ctx )
{
   bool result= false;

   if ( !setupFill( ctx ) )
   {
      goto exit;
   }

   result= true;

exit:
   return result;
}

static int codePoints[]=
{
   0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x2F,
   0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, 0x3F,
   0x40, 0x41, 0x42, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E, 0x4F,
   0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x5C, 0x5D, 0x5E, 0x5F,
   0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B, 0x6C, 0x6D, 0x6E, 0x6F,
   0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x7C, 0x7D, 0x7E, 0x7F,
   0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x8C, 0x8D, 0x8E, 0x8F,
   0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B, 0x9C, 0x9D, 0x9E, 0x9F,
   0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0xAC, 0xAD, 0xAE, 0xAF,
   0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xBC, 0xBD, 0xBE, 0xBF,
   0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB, 0xCC, 0xCD, 0xCE, 0xCF,
   0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xDC, 0xDD, 0xDE, 0xDF,
   0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
   0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA, 0xFB, 0xFC, 0xFD, 0xFE, 0xFF,
};

static bool initAppFont( AppCtx *ctx, AppFont *font, bool italic )
{
   bool result= false;
   int rc, imax;
   FT_Matrix transform;

   if ( ctx && font )
   {
      font->italic= italic;

      transform.xx= 0x10000L;
      transform.yx= 0x00000L;
      transform.xy= (italic ? (FT_Fixed)(0.375 * 65536) : 0x00000L);
      transform.yy= 0x10000L;

      FT_Set_Transform( ctx->ftFace, &transform, NULL );

      FT_Size_Metrics* metrics= &ctx->ftFace->size->metrics;
      font->fontHeight= metrics->height >> 6;
      font->fontAscent=  metrics->ascender >> 6;
      font->fontDescent= -metrics->descender >> 6;
      //printf("fontHeight %d ascent %d descent %d\n", font->fontHeight, font->fontAscent, font->fontDescent );

      font->glyphMap= new std::map<int,AppGlyph>();
      if ( !font->glyphMap )
      {
         printf("Error: unable to alloc glyph map\n");
         goto exit;
      }

      // Gather glyph metrics
      int totalWidth= 0, maxHeight= 0;
      imax= sizeof(codePoints)/sizeof(int);
      for( int i= 0; i < imax; ++i )
      {
         int codePoint;
         AppGlyph glyph;

         codePoint= codePoints[i];

         rc= FT_Load_Char( ctx->ftFace, codePoint, FT_LOAD_RENDER );
         if ( rc != 0 )
         {
            printf("Error: unable to load glyph for char %d\n", codePoint );
            goto exit;
         }
         FT_GlyphSlot gs= ctx->ftFace->glyph;

         glyph.bitmapLeft= gs->bitmap_left;
         glyph.bitmapTop= gs->bitmap_top;
         glyph.bitmapWidth= gs->bitmap.width;
         glyph.bitmapHeight= gs->bitmap.rows;
         glyph.advanceX= gs->advance.x;
         glyph.advanceY= gs->advance.y;
         glyph.textureX1= 0.0f;
         glyph.textureY1= 0.0f;
         glyph.textureX2= 0.0f;
         glyph.textureY2= 0.0f;
         //printf("codePoint %04X: left %d top %d w %d h %d advancex %d (%d) advancey %d (%d)\n", codePoint, glyph.bitmapLeft, glyph.bitmapTop, glyph.bitmapWidth, glyph.bitmapHeight, glyph.advanceX, glyph.advanceX>>6, glyph.advanceY, glyph.advanceY>>6 );

         totalWidth += glyph.bitmapWidth;
         if ( glyph.bitmapHeight > maxHeight ) maxHeight= glyph.bitmapHeight;         

         font->glyphMap->insert( std::pair<int,AppGlyph>(codePoint,glyph) );
      }

      //printf("totalWidth %d maxHeight %d\n", totalWidth, maxHeight );
      int textureBytesPerPixel= 1;
      int textureWidth= 1024;
      int textureStride= textureWidth*textureBytesPerPixel;
      int textureHeight= (((totalWidth+1023)/1024)+1)*textureBytesPerPixel*maxHeight;
      //printf("texture size (%d,%d)\n", textureWidth, textureHeight );

      unsigned char *bitmap= (unsigned char*)calloc( 1, textureWidth*4*textureHeight );
      if ( !bitmap )
      {
         printf("Error: unable to allocate memory for font atlas\n");
         goto exit;
      }

      // Build atlas texture
      int atlasX= 0;
      int atlasY= 0;
      imax= sizeof(codePoints)/sizeof(int);
      for( int i= 0; i < imax; ++i )
      {
         int codePoint;

         codePoint= codePoints[i];

         rc= FT_Load_Char( ctx->ftFace, codePoint, FT_LOAD_RENDER );
         if ( rc != 0 )
         {
            printf("Error: unable to load glyph for char %d\n", codePoint );
            goto exit;
         }
         FT_GlyphSlot gs= ctx->ftFace->glyph;

         std::map<int,AppGlyph>::iterator it= font->glyphMap->find( codePoint );
         if ( it != font->glyphMap->end() )
         {
            AppGlyph *g= &it->second;
            if ( g->bitmapWidth )
            {
               if ( atlasX+g->bitmapWidth >= textureWidth )
               {
                  atlasX= 0;
                  atlasY += maxHeight;
               }
               unsigned char *dest= bitmap+atlasY*textureStride+atlasX*textureBytesPerPixel;
               unsigned char *src= gs->bitmap.buffer;
               for( int i= 0; i < g->bitmapHeight; ++i )
               {
                  memcpy( dest, src, g->bitmapWidth*textureBytesPerPixel );
                  dest += textureStride;
                  src += g->bitmapWidth;
               }

               g->textureX1= (float)atlasX/(float)textureWidth;
               g->textureY1= (float)atlasY/(float)textureHeight;
               g->textureX2= (float)(atlasX+g->bitmapWidth)/(float)textureWidth;
               g->textureY2= (float)(atlasY+g->bitmapHeight)/(float)textureHeight;
               //printf("codePoint %04X (%c): (%f,%f,%f,%f)\n", codePoint, codePoint, g->textureX1, g->textureY1, g->textureX2, g->textureY2 );

               atlasX += g->bitmapWidth;
            }
         }
      }

      glGenTextures( 1, &font->atlasTextureId );
      //printf("atlasTextureId %d atlasX %d atlasY %d\n", font->atlasTextureId, atlasX, atlasY);

      glActiveTexture(GL_TEXTURE0);
      glBindTexture(GL_TEXTURE_2D, font->atlasTextureId);
      glTexImage2D( GL_TEXTURE_2D,
                    0, //level
                    GL_ALPHA, //internalFormat
                    textureWidth,
                    textureHeight,
                    0, // border
                    GL_ALPHA, //format
                    GL_UNSIGNED_BYTE,
                    bitmap );
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
      glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
      //printf("glError %d\n", glGetError());

      free( bitmap );      

      font->atlasWidth= textureWidth;
      font->atlasHeight= textureHeight;

      result= true;
   }

exit:
   return result;
}

static void termAppFont( AppCtx *ctx, AppFont *font )
{
   if ( font->glyphMap )
   {
      font->glyphMap->clear();
      delete font->glyphMap;
      font->glyphMap= 0;
   }
}

static bool initText( AppCtx *ctx )
{
   bool result= false;
   int rc, imax;

   if ( FT_Init_FreeType(&ctx->ft) == 0 )
   {
      ctx->haveFT= true;

      rc= FT_New_Face( ctx->ft, "/usr/share/fonts/XFINITYSansTT.ttf", 0, &ctx->ftFace );
      if ( rc != 0 )
      {
         printf("Error: unable to open font\n");
         goto exit;
      }
      ctx->haveFace= true;

      rc= FT_Set_Pixel_Sizes( ctx->ftFace, 0, FONT_SIZE );
      if ( rc != 0 )
      {
         printf("Error: unable to set font size\n");
         goto exit;
      }

      result= initAppFont( ctx, &ctx->font, false );
      if ( !result )
      {
         goto exit;
      }

      if ( !createShaders( ctx, vertexShaderGlyph, &ctx->shaderVertGlyph, fragmentShaderGlyph, &ctx->shaderFragGlyph ) )
      {
         goto exit;
      }

      ctx->progGlyph= glCreateProgram();
      glAttachShader( ctx->progGlyph, ctx->shaderVertGlyph );
      glAttachShader( ctx->progGlyph, ctx->shaderFragGlyph );

      ctx->attrGlyphVertex= 0;
      glBindAttribLocation(ctx->progGlyph, ctx->attrGlyphVertex, "vertex");

      ctx->attrGlyphTextureVertex= 1;
      glBindAttribLocation(ctx->progGlyph, ctx->attrGlyphTextureVertex, "textureVertex");

      if ( !linkProgram( ctx, ctx->progGlyph ) )
      {
         goto exit;
      }

      ctx->uniGlyphMatrix= glGetUniformLocation( ctx->progGlyph, "matrix" );
      if ( ctx->uniGlyphTarget == -1 )
      {
         printf("Error: uniform 'natrix' error\n");
         goto exit;
      }

      ctx->uniGlyphTarget= glGetUniformLocation( ctx->progGlyph, "targetSize" );
      if ( ctx->uniGlyphTarget == -1 )
      {
         printf("Error: uniform 'targetSize' error\n");
         goto exit;
      }

      ctx->uniGlyphColor= glGetUniformLocation( ctx->progGlyph, "color" );
      if ( ctx->uniGlyphColor == -1 )
      {
         printf("Error: uniform 'color' error\n");
         goto exit;
      }

      ctx->uniGlyphAlpha= glGetUniformLocation( ctx->progGlyph, "alpha" );
      if ( ctx->uniGlyphAlpha == -1 )
      {
         printf("Error: uniform 'alpha' error\n");
         goto exit;
      }

      ctx->uniGlyphTexture= glGetUniformLocation( ctx->progGlyph, "texture" );
      if ( ctx->uniGlyphTexture == -1 )
      {
         printf("Error: uniform 'texture' error\n");
         goto exit;
      }
   }

exit:

   return result;
}

static void termText( AppCtx *ctx )
{
   termAppFont( ctx, &ctx->font );

   if ( ctx->haveFace )
   {
      FT_Done_Face( ctx->ftFace );
      ctx->haveFace= false;
   }
   if ( ctx->haveFT )
   {
      FT_Done_FreeType( ctx->ft );
      ctx->haveFT= false;
   }
}

static void fillRect( AppCtx *ctx, int x, int y, int w, int h, unsigned argb )
{
   GLfloat verts[4][2]= {
      { (float)x,   (float)y },
      { (float)x+w, (float)y },
      { (float)x,   (float)y+h },
      { (float)x+w, (float)y+h }
   };

   float color[4]=
   {
      ((float)((argb>>16)&0xFF))/255.0f,
      ((float)((argb>>8)&0xFF))/255.0f,
      ((float)((argb)&0xFF))/255.0f,
      ((float)((argb>>24)&0xFF))/255.0f,
   };

   const float identityMatrix[4][4]=
   {
      {1, 0, 0, 0},
      {0, 1, 0, 0},
      {0, 0, 1, 0},
      {0, 0, 0, 1}
   };

   glGetError();
   glUseProgram( ctx->progFill );
   glUniformMatrix4fv( ctx->uniFillMatrix, 1, GL_FALSE, (GLfloat*)identityMatrix );
   glUniform2f( ctx->uniFillTarget, ctx->windowWidth, ctx->windowHeight );
   glUniform4fv( ctx->uniFillColor, 1, color );
   glVertexAttribPointer( ctx->attrFillVertex, 2, GL_FLOAT, GL_FALSE, 0, verts );
   glEnableVertexAttribArray( ctx->attrFillVertex );
   glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
   glDisableVertexAttribArray( ctx->attrFillVertex );
   int err= glGetError();
   if ( err )
   {
      printf("glError: %d\n", err);
   }
}

static bool initTitle( AppCtx *ctx, std::string title )
{
   bool result= false;

   ctx->title= new DrawableTextLine( ctx );
   if ( ctx->title )
   {
      int titleHeight= ctx->font.fontHeight+4;
      ctx->title->setText( title );
      ctx->title->setPosition( ctx->windowX + ctx->borderWidth, ctx->windowY + ctx->borderWidth );
      ctx->title->setBounds( ctx->windowWidth-2*ctx->borderWidth, titleHeight );
      ctx->title->setCentered(true);
      ctx->title->setColor( 0xFF00FFB0 );
      result= true;
   }

   return result;
}

static void termTitle( AppCtx *ctx )
{
   if ( ctx->title )
   {
      delete ctx->title;
      ctx->title= 0;
   }
}

static void enumerateSessionsCallback( void *userData, int result, int count, AudSrvSessionInfo *sessionInfo )
{
   AppCtx *ctx= (AppCtx*)userData;
   if ( result == 0 )
   {
      for( int i= 0; i < count; ++i )
      {
         ctx->sessionList->addSession( &sessionInfo[i] );
      }
      printf("\n");
   }
}

static void sessionEventCallback( void *userData, int event, AudSrvSessionInfo *sessionInfo )
{
   AppCtx *ctx= (AppCtx*)userData;
   switch ( event )
   {
      case AUDSRV_SESSIONEVENT_Removed:
         if ( ctx->sessionList )
         {
            ctx->sessionList->removeSession( sessionInfo );
         }
         break;
      case AUDSRV_SESSIONEVENT_Added:
         if ( ctx->sessionList )
         {
            ctx->sessionList->addSession( sessionInfo );
         }
         break;
      default:
         break;
   }
}

static bool initAudioServer( AppCtx *ctx )
{
   bool result= false;
   
   const char *serverName= getenv("AUDSRV_NAME");
   ctx->audsrvObserver= AudioServerConnect( serverName );
   if ( !ctx->audsrvObserver )
   {
      printf("failed to create observer session\n");
      goto exit;
   }

   ctx->sessionList= new SessionList(ctx);
   if ( !ctx->sessionList )
   {
      printf("failed to create SessionList\n");
      goto exit;
   }

   result= AudioServerEnumerateSessions( ctx->audsrvObserver, enumerateSessionsCallback, ctx );
   if ( !result )
   {
      printf("session enumeration failed\n");
      goto exit;
   }

   result= AudioServerEnableSessionEvent( ctx->audsrvObserver, sessionEventCallback, ctx );
   if ( !result )
   {
      printf("session event registration failed\n");
      goto exit;
   }

exit:
   return result;
}

static void termAudioServer( AppCtx *ctx )
{
   if ( ctx->sessionList )
   {
      delete ctx->sessionList;
      ctx->sessionList= 0;
   }

   if ( ctx->audsrvObserver )
   {
      AudioServerDisableSessionEvent( ctx->audsrvObserver );      
      AudioServerDisconnect( ctx->audsrvObserver );
      ctx->audsrvObserver= 0;
   }
}

static void toggleSessionSelect( AppCtx *ctx, Session *session )
{
   if ( ctx && session )
   {
      if ( ctx->sessionSelected )
      {
         AudioServerSessionDetach( ctx->audsrvObserver );
         ctx->sessionSelected->setSelected(false);
      }
      if ( session != ctx->sessionSelected )
      {
         ctx->sessionSelected= session;
         ctx->sessionSelected->setSelected(true);
         AudioServerSessionAttach( ctx->audsrvObserver, session->name() );
         switch ( session->sessionType() )
         {
            case AUDSRV_SESSION_Primary:
            case AUDSRV_SESSION_Secondary:
            case AUDSRV_SESSION_Effect:
               ctx->showSession= true;
               ctx->nextStatusTime= 0;
               break;
            default:
               ctx->showSession= false;
               break;
         }
      }
      else
      {
         ctx->sessionSelected= 0;
         ctx->showSession= false;
      }
      ctx->dirty= true;
   }
}

static void sessionStatusCallback( void *userData, int result, AudSrvSessionStatus *sessionStatus )
{
   AppCtx *ctx= (AppCtx*)userData;

   if ( ctx )
   {
      if ( ctx->globalMuted != sessionStatus->globalMuted )
      {
         ctx->globalMuted= sessionStatus->globalMuted;
         if ( ctx->fieldGlobalMuted )
         {
            ctx->fieldGlobalMuted->setText( ctx->globalMuted ? "Muted" : "Unmuted" );
         }
         ctx->dirty= true;
      }
      if ( ctx->globalVolume != sessionStatus->globalVolume )
      {
         ctx->globalVolume= sessionStatus->globalVolume;
         if ( ctx->fieldGlobalVolume )
         {
            char work[6];
            sprintf( work, "%d", (int)(ctx->globalVolume*100+0.5) );
            ctx->fieldGlobalVolume->setText(work);
         }
         ctx->dirty= true;
      }
      if ( result == 0 )
      {
         if ( ctx->sessionMuted != sessionStatus->muted )
         {
            ctx->sessionMuted= sessionStatus->muted;
            if ( ctx->fieldSessionMuted )
            {
               ctx->fieldSessionMuted->setText( ctx->sessionMuted ? "Muted" : "Unmuted" );
            }
            ctx->dirty= true;
         }
         if ( ctx->sessionVolume != sessionStatus->volume )
         {
            ctx->sessionVolume= sessionStatus->volume;
            if ( ctx->fieldSessionVolume )
            {
               char work[6];
               sprintf( work, "%d", (int)(ctx->sessionVolume*100+0.5) );
               ctx->fieldSessionVolume->setText(work);
            }
            ctx->dirty= true;
         }
      }
   }
}

static void updateStatus( AppCtx *ctx )
{
   long long now= getCurrentTimeMillis();
   if ( now >= ctx->nextStatusTime )
   {
      ctx->nextStatusTime= now+STATUS_PERIOD;
      AudioServerGetSessionStatus( ctx->audsrvObserver, sessionStatusCallback, ctx );
   }
}

static void drawWindowBackground( AppCtx *ctx )
{
   int x= ctx->windowX, y= ctx->windowY, w= ctx->windowWidth, h= ctx->windowHeight;
   int bw= ctx->borderWidth;

   fillRect( ctx, x, y, w, h, WINDOW_BACKGROUND_COLOR );
   fillRect( ctx, x, y, w, bw, WINDOW_BORDER_COLOR );
   fillRect( ctx, x, y, bw, h, WINDOW_BORDER_COLOR );
   fillRect( ctx, x+w-bw, y, bw, h, WINDOW_BORDER_COLOR );
   fillRect( ctx, x, y+h-bw, w, bw, WINDOW_BORDER_COLOR );
}

static void drawTitle( AppCtx *ctx )
{
   if ( ctx->title )
   {
      int x, y, w, h;

      ctx->title->getRect( x, y, w, h );
      ctx->clientX= ctx->windowX+ctx->borderWidth;
      ctx->clientY= ctx->windowY+ctx->borderWidth+h;
      ctx->clientWidth= ctx->windowWidth - 2*ctx->borderWidth;
      ctx->clientHeight= ctx->windowHeight - 2*ctx->borderWidth;

      fillRect( ctx, ctx->clientX, ctx->windowY+ctx->borderWidth, ctx->clientWidth, h, WINDOW_TITLEBAR_COLOR );
      ctx->title->draw();
   }
}

static void drawGlobalStatus( AppCtx *ctx )
{
   if ( !ctx->labelGlobal )
   {
      int x, y, w, h;

      x= ctx->clientX+6;
      y= ctx->clientY+ctx->clientHeight-2*ctx->font.fontHeight-4;
      w= ctx->clientWidth/5;
      h= ctx->font.fontHeight;
      ctx->labelGlobal= new DrawableTextLine(ctx);
      if ( ctx->labelGlobal )
      {
         ctx->labelGlobal->setPosition( x, y );
         ctx->labelGlobal->setBounds( w, h );
         ctx->labelGlobal->setText( "Global:" );
      }

      x += w;
      ctx->fieldGlobalMuted= new DrawableTextLine(ctx);
      if ( ctx->fieldGlobalMuted )
      {
         ctx->globalMuted= false;
         ctx->globalMutedX= x;
         ctx->globalMutedY= y;
         ctx->globalMutedW= w;
         ctx->globalMutedH= h;
         ctx->fieldGlobalMuted->setPosition( x, y );
         ctx->fieldGlobalMuted->setBounds( w, h );
         ctx->fieldGlobalMuted->setText( ctx->globalMuted ? "Muted" : "Unmuted" );
      }

      x += w;
      ctx->labelGlobalVolumeDown= new DrawableTextLine(ctx);
      if ( ctx->labelGlobalVolumeDown )
      {
         ctx->globalVolumeDownX= x;
         ctx->globalVolumeDownY= y;
         ctx->globalVolumeDownW= w;
         ctx->globalVolumeDownH= h;
         ctx->labelGlobalVolumeDown->setPosition( x, y );
         ctx->labelGlobalVolumeDown->setBounds( w, h );
         ctx->labelGlobalVolumeDown->setText( "<<" );
      }

      x += w;
      ctx->fieldGlobalVolume= new DrawableTextLine(ctx);
      if ( ctx->fieldGlobalVolume )
      {
         ctx->globalVolume= 1.0;
         ctx->fieldGlobalVolume->setPosition( x, y );
         ctx->fieldGlobalVolume->setBounds( w, h );
         ctx->fieldGlobalVolume->setText( "100" );
      }
      
      x += w;
      ctx->labelGlobalVolumeUp= new DrawableTextLine(ctx);
      if ( ctx->labelGlobalVolumeUp )
      {
         ctx->globalVolumeUpX= x;
         ctx->globalVolumeUpY= y;
         ctx->globalVolumeUpW= w;
         ctx->globalVolumeUpH= h;
         ctx->labelGlobalVolumeUp->setPosition( x, y );
         ctx->labelGlobalVolumeUp->setBounds( w, h );
         ctx->labelGlobalVolumeUp->setText( ">>" );
      }
   }

   if ( ctx->labelGlobal ) ctx->labelGlobal->draw();
   if ( ctx->fieldGlobalMuted ) ctx->fieldGlobalMuted->draw();
   if ( ctx->labelGlobalVolumeDown ) ctx->labelGlobalVolumeDown->draw();
   if ( ctx->fieldGlobalVolume ) ctx->fieldGlobalVolume->draw();
   if ( ctx->labelGlobalVolumeUp ) ctx->labelGlobalVolumeUp->draw();
}

static void drawSessionStatus( AppCtx *ctx )
{
   if ( !ctx->labelSession )
   {
      int x, y, w, h;

      x= ctx->clientX+6;
      y= ctx->clientY+ctx->clientHeight-3*ctx->font.fontHeight-4;
      w= ctx->clientWidth/5;
      h= ctx->font.fontHeight;
      ctx->labelSession= new DrawableTextLine(ctx);
      if ( ctx->labelSession )
      {
         ctx->labelSession->setPosition( x, y );
         ctx->labelSession->setBounds( w, h );
         ctx->labelSession->setText( "Session:" );
      }

      x += w;
      ctx->fieldSessionMuted= new DrawableTextLine(ctx);
      if ( ctx->fieldSessionMuted )
      {
         ctx->sessionMuted= false;
         ctx->sessionMutedX= x;
         ctx->sessionMutedY= y;
         ctx->sessionMutedW= w;
         ctx->sessionMutedH= h;
         ctx->fieldSessionMuted->setPosition( x, y );
         ctx->fieldSessionMuted->setBounds( w, h );
         ctx->fieldSessionMuted->setText( ctx->sessionMuted ? "Muted" : "Unmuted" );
      }

      x += w;
      ctx->labelSessionVolumeDown= new DrawableTextLine(ctx);
      if ( ctx->labelSessionVolumeDown )
      {
         ctx->sessionVolumeDownX= x;
         ctx->sessionVolumeDownY= y;
         ctx->sessionVolumeDownW= w;
         ctx->sessionVolumeDownH= h;
         ctx->labelSessionVolumeDown->setPosition( x, y );
         ctx->labelSessionVolumeDown->setBounds( w, h );
         ctx->labelSessionVolumeDown->setText( "<<" );
      }

      x += w;
      ctx->fieldSessionVolume= new DrawableTextLine(ctx);
      if ( ctx->fieldSessionVolume )
      {
         ctx->sessionVolume= 1.0;
         ctx->fieldSessionVolume->setPosition( x, y );
         ctx->fieldSessionVolume->setBounds( w, h );
         ctx->fieldSessionVolume->setText( "100" );
      }
      
      x += w;
      ctx->labelSessionVolumeUp= new DrawableTextLine(ctx);
      if ( ctx->labelSessionVolumeUp )
      {
         ctx->sessionVolumeUpX= x;
         ctx->sessionVolumeUpY= y;
         ctx->sessionVolumeUpW= w;
         ctx->sessionVolumeUpH= h;
         ctx->labelSessionVolumeUp->setPosition( x, y );
         ctx->labelSessionVolumeUp->setBounds( w, h );
         ctx->labelSessionVolumeUp->setText( ">>" );
      }
   }

   if ( ctx->showSession )
   {
      if ( ctx->labelSession ) ctx->labelSession->draw();
      if ( ctx->fieldSessionMuted ) ctx->fieldSessionMuted->draw();
      if ( ctx->labelSessionVolumeDown ) ctx->labelSessionVolumeDown->draw();
      if ( ctx->fieldSessionVolume ) ctx->fieldSessionVolume->draw();
      if ( ctx->labelSessionVolumeUp ) ctx->labelSessionVolumeUp->draw();
   }
}

static bool renderGL( AppCtx *ctx )
{
   bool result= false;

   glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
   glClear(GL_COLOR_BUFFER_BIT);
   glFrontFace( GL_CW );
   glEnable(GL_BLEND);
   glBlendFuncSeparate( GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE );

   drawWindowBackground( ctx );

   drawTitle( ctx );

   if ( ctx->sessionList )
   {
      ctx->sessionList->draw();
   }

   drawSessionStatus( ctx );
   drawGlobalStatus( ctx );

   result= true;

exit:
   return result;
}

static void terminated( void * )
{
   printf("terminated event\n");
   gRunning= false;
}

static EssTerminateListener terminateListener=
{
   terminated
};

static void keyPressed( void *userData, unsigned int key )
{
   AppCtx *ctx= (AppCtx*)userData;
}

static void keyReleased( void *userData, unsigned int )
{
   AppCtx *ctx= (AppCtx*)userData;
}

static EssKeyListener keyListener=
{
   keyPressed,
   keyReleased
};

static void pointerMotion( void *userData, int, int )
{
   AppCtx *ctx= (AppCtx*)userData;
}

static void pointerButtonPressed( void *userData, int button, int x, int y )
{
   AppCtx *ctx= (AppCtx*)userData;
   if ( ctx )
   {
      Session *session= ctx->sessionList->selectFromPoint( x, y);
      if ( session )
      {
         toggleSessionSelect( ctx, session );
      }
      else if ( (x > ctx->globalMutedX) && (x < ctx->globalMutedX+ctx->globalMutedW) &&
                (y > ctx->globalMutedY) && (y < ctx->globalMutedY+ctx->globalMutedH) )
      {
         AudioServerGlobalMute( ctx->audsrvObserver, !ctx->globalMuted );
         ctx->nextStatusTime= 0;
      }
      else if ( (x > ctx->globalVolumeDownX) && (x < ctx->globalVolumeDownX+ctx->globalVolumeDownW) &&
                (y > ctx->globalVolumeDownY) && (y < ctx->globalVolumeDownY+ctx->globalVolumeDownH) )
      {
         float newVolume= ctx->globalVolume - 0.05;
         if ( newVolume < 0.0 ) newVolume= 0.0;
         AudioServerGlobalVolume( ctx->audsrvObserver, newVolume );
         ctx->nextStatusTime= 0;
      }
      else if ( (x > ctx->globalVolumeUpX) && (x < ctx->globalVolumeUpX+ctx->globalVolumeUpW) &&
                (y > ctx->globalVolumeUpY) && (y < ctx->globalVolumeUpY+ctx->globalVolumeUpH) )
      {
         float newVolume= ctx->globalVolume + 0.05;
         if ( newVolume > 1.0 ) newVolume= 1.0;
         AudioServerGlobalVolume( ctx->audsrvObserver, newVolume );
         ctx->nextStatusTime= 0;
      }
      else if ( ctx->showSession )
      {
         if ( (x > ctx->sessionMutedX) && (x < ctx->sessionMutedX+ctx->sessionMutedW) &&
              (y > ctx->sessionMutedY) && (y < ctx->sessionMutedY+ctx->sessionMutedH) )
         {
            AudioServerMute( ctx->audsrvObserver, !ctx->sessionMuted );
            ctx->nextStatusTime= 0;
         }
         else if ( (x > ctx->sessionVolumeDownX) && (x < ctx->sessionVolumeDownX+ctx->sessionVolumeDownW) &&
                   (y > ctx->sessionVolumeDownY) && (y < ctx->sessionVolumeDownY+ctx->sessionVolumeDownH) )
         {
            float newVolume= ctx->sessionVolume - 0.05;
            if ( newVolume < 0.0 ) newVolume= 0.0;
            AudioServerVolume( ctx->audsrvObserver, newVolume );
            ctx->nextStatusTime= 0;
         }
         else if ( (x > ctx->sessionVolumeUpX) && (x < ctx->sessionVolumeUpX+ctx->sessionVolumeUpW) &&
                   (y > ctx->sessionVolumeUpY) && (y < ctx->sessionVolumeUpY+ctx->sessionVolumeUpH) )
         {
            float newVolume= ctx->sessionVolume + 0.05;
            if ( newVolume > 1.0 ) newVolume= 1.0;
            AudioServerVolume( ctx->audsrvObserver, newVolume );
            ctx->nextStatusTime= 0;
         }
      }
   }
}

static void pointerButtonReleased( void *userData, int, int, int )
{
   AppCtx *ctx= (AppCtx*)userData;
}

static EssPointerListener pointerListener=
{
   pointerMotion,
   pointerButtonPressed,
   pointerButtonReleased
};

static void showUsage()
{
   printf("usage:\n");
   printf(" asmonitor [options]\n" );
   printf("where [options] are:\n" );
   printf("  --wayland : run as wayland client\n");
   printf("  --rect x,y,w,h : set initial window rect\n" );
   printf("  -? : show usage\n" );
   printf("\n" );   
}

int main( int argc, char **argv )
{
   int nRC= 0;
   AppCtx *ctx= 0;
   int argidx, len;
   bool error= false;

   printf("asmonitor v1.0\n");
   ctx= (AppCtx*)calloc( 1, sizeof(AppCtx) );
   if ( !ctx )
   {
      printf("Error: no memory for app context\n");
      goto exit;
   }

   ctx->windowX= 200;
   ctx->windowY= 200;
   ctx->windowWidth= 640;
   ctx->windowHeight= 360;
   ctx->borderWidth= 4;

   ctx->essCtx= EssContextCreate();
   if ( !ctx->essCtx )
   {
      printf("Error: EssContextCreate failed\n");
      goto exit;
   }

   argidx= 1;   
   while ( argidx < argc )
   {
      if ( argv[argidx][0] == '-' )
      {
         switch( argv[argidx][1] )
         {
            case '-':
               len= strlen(argv[argidx]);
               if ( (len == 6) && !strncmp( (const char*)argv[argidx], "--rect", len) )
               {
                  if ( argidx+1 < argc )
                  {
                     int x, y, w, h;
                     if ( sscanf( argv[++argidx], "%d,%d,%d,%d", &x, &y, &w, &h ) == 4 )
                     {
                        ctx->windowX= x;
                        ctx->windowY= y;
                        ctx->windowWidth= w;
                        ctx->windowHeight= h;
                     }
                  }
               }
               break;
            case '?':
               showUsage();
               goto exit;
            default:
               printf( "unknown option %s\n\n", argv[argidx] );
               exit( -1 );
               break;
         }
      }
      else
      {
         printf( "ignoring extra argument: %s\n", argv[argidx] );
      }
      ++argidx;
   }

   if ( !EssContextSetName( ctx->essCtx, "asmonitor" ) )
   {
      error= true;
   }

   if ( !EssContextSetUseWayland( ctx->essCtx, true ) )
   {
      error= true;
   }

   if ( !EssContextSetInitialWindowSize( ctx->essCtx, ctx->windowWidth, ctx->windowHeight ) )
   {
      error= true;
   }

   if ( !EssContextSetWindowPosition( ctx->essCtx, ctx->windowX, ctx->windowY ) )
   {
      error= true;
   }
   ctx->windowX= 0;
   ctx->windowY= 0;

   if ( !EssContextSetTerminateListener( ctx->essCtx, ctx, &terminateListener ) )
   {
      error= true;
   }

   if ( !EssContextSetKeyListener( ctx->essCtx, ctx, &keyListener ) )
   {
      error= true;
   }

   if ( !EssContextSetPointerListener( ctx->essCtx, ctx, &pointerListener ) )
   {
      error= true;
   }

   if ( !error )
   {
      struct sigaction sigint;

      sigint.sa_handler= signalHandler;
      sigemptyset(&sigint.sa_mask);
      sigint.sa_flags= SA_RESETHAND;
      sigaction(SIGINT, &sigint, NULL);

      if ( !EssContextStart( ctx->essCtx ) )
      {
         error= true;
      }
      else
      if ( !EssContextGetDisplaySize( ctx->essCtx, &gDisplayWidth, &gDisplayHeight ) )
      {
         error= true;
      }
      else
      if ( !setupGL( ctx ) )
      {
         error= true;
      }
      else
      if ( !initText( ctx ) )
      {
         error= true;
      }
      else
      if ( !initTitle( ctx, "asmonitor" ) )
      {
         error= true;
      }
      else
      if ( !initAudioServer( ctx ) )
      {
         error= true;
      }

      if ( !error )
      {
         gRunning= true;
         while( gRunning )
         {
            updateStatus( ctx );
            if ( ctx->dirty )
            {
               ctx->dirty= false;
               renderGL( ctx );
               EssContextUpdateDisplay( ctx->essCtx );
            }
            EssContextRunEventLoopOnce( ctx->essCtx );
         }
      }
   }

   if ( error )
   {
      const char *detail= EssContextGetLastErrorDetail( ctx->essCtx );
      printf("Essos error: (%s)\n", detail );
   }

exit:

   if ( ctx )
   {
      termAudioServer(ctx);
      termTitle(ctx);
      termText(ctx);

      if ( ctx->essCtx )
      {
         EssContextDestroy( ctx->essCtx );
      }

      free( ctx );
   }

   return nRC;
}

