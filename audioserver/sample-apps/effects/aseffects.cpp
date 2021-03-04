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
#include <gst/gst.h>
#include <signal.h>
#include <sys/time.h>

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
#define WINDOW_TITLEBAR_COLOR (0xFF707070)
#define BUTTON_ACTIVE_COLOR (0x60F0F080)
#define BUTTON_INACTIVE_COLOR (0x60808080)
#define ACTIVE_TEXT_COLOR (0xFF101010)
#define NORMAL_TEXT_COLOR (0xFF101010)

class DrawableTextLine;
typedef struct AppCtx_ AppCtx;

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

typedef struct Pipeline_
{
   AppCtx *ctx;
   GstElement *pipeline;
   GstBus *bus;
   GstElement *filesrc;
   GstElement *queue;
   GstElement *audiosink;
   GstCaps *capsContainer;
   bool eos;
   bool loop;
   bool playing;
   const char *clipName;
} Pipeline;

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
   GMainLoop *loop;
   AudSrv audsrvLoop;
   Pipeline *pipeLineLoop;
   AudSrv audsrvOneShot;
   Pipeline *pipeLineOneShot;
   bool controlsInit;
   DrawableTextLine *labelLoop;
   int loopX;
   int loopY;
   int loopW;
   int loopH;
   bool laserPlaying;
   DrawableTextLine *labelLaser;
   int laserX;
   int laserY;
   int laserW;
   int laserH;
   bool glassPlaying;
   DrawableTextLine *labelGlass;
   int glassX;
   int glassY;
   int glassW;
   int glassH;
   bool doorPlaying;
   DrawableTextLine *labelDoor;
   int doorX;
   int doorY;
   int doorW;
   int doorH;
   bool bonkPlaying;
   DrawableTextLine *labelBonk;
   int bonkX;
   int bonkY;
   int bonkW;
   int bonkH;
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
static gboolean busCallback(GstBus *bus, GstMessage *message, gpointer data);
static Pipeline* createPipeline( AppCtx *ctx, AudSrv audsrv );
static void destroyPipeline( Pipeline *pipeLine );
static bool playClip( Pipeline *pipeLine, const char *fileName );
static bool stopClip( Pipeline *pipeLine );
static bool initGst( AppCtx *ctx );
static void termGst( AppCtx *ctx );
static void fillRect( AppCtx *ctx, int x, int y, int w, int h, unsigned argb );
static bool initTitle( AppCtx *ctx, std::string title );
static void termTitle( AppCtx *ctx );
static void drawWindowBackground( AppCtx *ctx );
static void drawTitle( AppCtx *ctx );
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

static gboolean busCallback(GstBus *bus, GstMessage *message, gpointer data)
{
   Pipeline *pipeLine= (Pipeline*)data;
   
   switch ( GST_MESSAGE_TYPE(message) ) 
   {
      case GST_MESSAGE_ERROR: 
         {
            GError *error;
            gchar *debug;
            
            gst_message_parse_error(message, &error, &debug);
            printf("Error: %s\n", error->message);
            if ( debug )
            {
               printf("Debug info: %s\n", debug);
            }
            g_error_free(error);
            g_free(debug);
         }
         break;
     case GST_MESSAGE_EOS:
         printf( "EOS pipeLine %p\n", pipeLine );
         pipeLine->eos= true;
         if ( pipeLine->loop )
         {
            playClip( pipeLine, pipeLine->clipName );
         }
         else
         {
            stopClip( pipeLine );
            pipeLine->ctx->dirty= true;
         }
         break;
     default:
         break;
    }
    return TRUE;
}

static Pipeline* createPipeline( AppCtx *ctx, AudSrv audsrv )
{
   bool result= false;
   Pipeline *pipeLine= 0;
   static bool gstInit= false;

   if ( !gstInit )
   {
      int argc= 0;
      char **argv= 0;
      gstInit= true;
      gst_init( &argc, &argv );
   }

   pipeLine= (Pipeline*)calloc( 1, sizeof(Pipeline) );
   if ( !pipeLine )
   {
      printf("Error: no memory for pipeLine\n");
      goto exit;
   }
   pipeLine->ctx= ctx;

   pipeLine->pipeline= gst_pipeline_new("pipeline");
   if ( !pipeLine->pipeline )
   {
      printf("Error: unable to create pipeline instance\n" );
      goto exit;
   }

   pipeLine->bus= gst_pipeline_get_bus( GST_PIPELINE(pipeLine->pipeline) );
   if ( !pipeLine->bus )
   {
      printf("Error: unable to get pipeline bus\n");
      goto exit;
   }
   gst_bus_add_watch( pipeLine->bus, busCallback, pipeLine );

   pipeLine->filesrc= gst_element_factory_make( "filesrc", "filesrc" );
   if ( !pipeLine->filesrc )
   {
      g_print("Error: unable to create filesrc instance\n" );
      goto exit;
   }
   gst_object_ref( pipeLine->filesrc );

   pipeLine->queue= gst_element_factory_make( "queue", "queue" );
   if ( !pipeLine->queue )
   {
      printf("Error: unable to create queue instance\n" );
      goto exit;
   }
   gst_object_ref( pipeLine->queue );

   pipeLine->audiosink= gst_element_factory_make( "audsrvsink", "audiosink" );
   if ( !pipeLine->audiosink )
   {
      printf("Error: unable to create audiosink instance\n" );
      goto exit;
   }
   gst_object_ref( pipeLine->audiosink );

   gst_bin_add_many( GST_BIN(pipeLine->pipeline), 
                     pipeLine->filesrc,
                     pipeLine->queue,
                     pipeLine->audiosink,
                     NULL
                   );

   pipeLine->capsContainer= gst_caps_new_simple( "audio/x-wav", NULL );

   if ( !gst_element_link_filtered( pipeLine->filesrc, pipeLine->queue, pipeLine->capsContainer ) )
   {
      printf("Error: unable to link src to queue\n");
      goto exit;
   }

   gst_caps_unref( pipeLine->capsContainer );
   pipeLine->capsContainer= 0;

   if ( !gst_element_link_many( pipeLine->queue, pipeLine->audiosink, NULL ) )
   {
      printf("Error: unable to link queue and audiosink\n");
      goto exit;
   }

   g_object_set( G_OBJECT(pipeLine->audiosink), "audio-pts-offset", 4500, NULL );
   g_object_set( G_OBJECT(pipeLine->audiosink), "session", audsrv, NULL );   

   result= true;   

exit:

   if ( pipeLine->capsContainer )
   {
      gst_caps_unref( pipeLine->capsContainer );
      pipeLine->capsContainer= 0;
   }

   if ( !result )
   {
      destroyPipeline( pipeLine );
      pipeLine= 0;
   }

   return pipeLine;
}

static void destroyPipeline( Pipeline *pipeLine )
{
   if ( pipeLine->pipeline )
   {
      gst_element_set_state(pipeLine->pipeline, GST_STATE_NULL);
   }
   if ( pipeLine->audiosink )
   {
      gst_object_unref( pipeLine->audiosink );
      pipeLine->audiosink= 0;
   }
   if ( pipeLine->queue )
   {
      gst_object_unref( pipeLine->queue );
      pipeLine->queue= 0;
   }
   if ( pipeLine->filesrc )
   {
      gst_object_unref( pipeLine->filesrc );
      pipeLine->filesrc= 0;
   }
   if ( pipeLine->capsContainer )
   {
      gst_caps_unref( pipeLine->capsContainer );
      pipeLine->capsContainer= 0;
   }      
   if ( pipeLine->bus )
   {
      gst_object_unref( pipeLine->bus );
      pipeLine->bus= 0;
   }
   if ( pipeLine->pipeline )
   {
      gst_object_unref( GST_OBJECT(pipeLine->pipeline) );
      pipeLine->pipeline= 0;
   }
}

static bool playClip( Pipeline *pipeLine, const char *fileName )
{
   bool result= false;

   if ( pipeLine->playing || pipeLine->eos )
   {
      pipeLine->playing= false;
      pipeLine->eos= false;
      gst_element_set_state(pipeLine->pipeline, GST_STATE_READY );
   }

   pipeLine->clipName= fileName;
   if ( fileName )
   {
      g_object_set( G_OBJECT(pipeLine->filesrc), "location", fileName, NULL );

      if ( GST_STATE_CHANGE_FAILURE != gst_element_set_state(pipeLine->pipeline, GST_STATE_PLAYING) )
      {
         pipeLine->playing= true;
         result= true;
      }
   }
   else
   {
      result= true;
   }

   return result;
}

static bool stopClip( Pipeline *pipeLine )
{
   playClip( pipeLine, 0 );
}

static bool initGst( AppCtx *ctx )
{
   bool result= false;
   int argc= 0;
   char **argv= 0;
   const char *serverName= getenv("AUDSRV_NAME");

   gst_init( &argc, &argv );

   ctx->loop= g_main_loop_new(NULL,FALSE);
   if ( !ctx->loop )
   {
      printf("Error: unable to create glib main loop\n");
      goto exit;
   }

   ctx->audsrvLoop= AudioServerConnect( serverName );
   if ( !ctx->audsrvLoop )
   {
      printf("Error: unable to connect to audioserver for loop\n");
      goto exit;
   }
   if ( !AudioServerInitSession( ctx->audsrvLoop, AUDSRV_SESSION_Effect, false, "aseffect-loop" ) )
   {
      printf("Error: unable to initialize loop session\n");
      goto exit;
   }

   ctx->pipeLineLoop= createPipeline( ctx, ctx->audsrvLoop );
   if ( !ctx->pipeLineLoop )
   {
      printf("Error: unable to create loop pipeline\n");
      goto exit;
   }
   ctx->pipeLineLoop->loop= true;

   ctx->audsrvOneShot= AudioServerConnect( serverName );
   if ( !ctx->audsrvOneShot )
   {
      printf("Error: unable to connect to audioserver for one-shot\n");
      goto exit;
   }
   if ( !AudioServerInitSession( ctx->audsrvOneShot, AUDSRV_SESSION_Effect, false, "aseffect-oneshot" ) )
   {
      printf("Error: unable to initialize one-shot session\n");
      goto exit;
   }

   ctx->pipeLineOneShot= createPipeline( ctx, ctx->audsrvOneShot );
   if ( !ctx->pipeLineOneShot )
   {
      printf("Error: unable to create oneshot pipeline\n");
      goto exit;
   }

   result= true;

exit:

   return result;
}

static void termGst( AppCtx *ctx )
{
   if ( ctx->pipeLineOneShot )
   {
      destroyPipeline( ctx->pipeLineOneShot );
      ctx->pipeLineOneShot= 0;
   }

   if ( ctx->pipeLineLoop )
   {
      destroyPipeline( ctx->pipeLineLoop );
      ctx->pipeLineLoop= 0;
   }

   if ( ctx->audsrvLoop )
   {
      AudioServerDisconnect( ctx->audsrvLoop );
      ctx->audsrvLoop= 0;
   }

   if ( ctx->audsrvOneShot )
   {
      AudioServerDisconnect( ctx->audsrvOneShot );
      ctx->audsrvOneShot= 0;
   }

   if ( ctx->loop )
   {
      g_main_loop_unref(ctx->loop);
      ctx->loop= 0;
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
   glUniform2f( ctx->uniGlyphTarget, ctx->windowWidth, ctx->windowHeight );
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

static void drawControls( AppCtx *ctx )
{
   bool playing;
   int color;

   if ( !ctx->controlsInit )
   {
      int x, y, w, h;

      x= ctx->clientX+12;
      y= ctx->clientY+12;
      w= ctx->clientWidth/5-12;
      h= ctx->font.fontHeight*2;
      ctx->labelLoop= new DrawableTextLine(ctx);
      if ( ctx->labelLoop )
      {
         ctx->loopX= x;
         ctx->loopY= y;
         ctx->loopW= w;
         ctx->loopH= h;
         ctx->labelLoop->setCentered( true );
         ctx->labelLoop->setPosition( x, y );
         ctx->labelLoop->setBounds( w, h );
         ctx->labelLoop->setText( "Loop" );
      }

      x= ctx->clientX+12;
      y= ctx->clientY+2*(ctx->font.fontHeight+12);
      w= ctx->clientWidth/5-12;
      h= ctx->font.fontHeight*2;
      ctx->labelLaser= new DrawableTextLine(ctx);
      if ( ctx->labelLaser )
      {
         ctx->laserX= x;
         ctx->laserY= y;
         ctx->laserW= w;
         ctx->laserH= h;
         ctx->labelLaser->setCentered( true );
         ctx->labelLaser->setPosition( x, y );
         ctx->labelLaser->setBounds( w, h );
         ctx->labelLaser->setText( "Laser" );
      }

      x= ctx->clientX+12+ctx->clientWidth/5;
      y= ctx->clientY+2*(ctx->font.fontHeight+12);
      w= ctx->clientWidth/5-12;
      h= ctx->font.fontHeight*2;
      ctx->labelGlass= new DrawableTextLine(ctx);
      if ( ctx->labelGlass )
      {
         ctx->glassX= x;
         ctx->glassY= y;
         ctx->glassW= w;
         ctx->glassH= h;
         ctx->labelGlass->setCentered( true );
         ctx->labelGlass->setPosition( x, y );
         ctx->labelGlass->setBounds( w, h );
         ctx->labelGlass->setText( "Glass" );
      }

      x= ctx->clientX+12+2*(ctx->clientWidth/5);
      y= ctx->clientY+2*(ctx->font.fontHeight+12);
      w= ctx->clientWidth/5-12;
      h= ctx->font.fontHeight*2;
      ctx->labelDoor= new DrawableTextLine(ctx);
      if ( ctx->labelDoor )
      {
         ctx->doorX= x;
         ctx->doorY= y;
         ctx->doorW= w;
         ctx->doorH= h;
         ctx->labelDoor->setCentered( true );
         ctx->labelDoor->setPosition( x, y );
         ctx->labelDoor->setBounds( w, h );
         ctx->labelDoor->setText( "Door" );
      }

      x= ctx->clientX+12+3*(ctx->clientWidth/5);
      y= ctx->clientY+2*(ctx->font.fontHeight+12);
      w= ctx->clientWidth/5-12;
      h= ctx->font.fontHeight*2;
      ctx->labelBonk= new DrawableTextLine(ctx);
      if ( ctx->labelBonk )
      {
         ctx->bonkX= x;
         ctx->bonkY= y;
         ctx->bonkW= w;
         ctx->bonkH= h;
         ctx->labelBonk->setCentered( true );
         ctx->labelBonk->setPosition( x, y );
         ctx->labelBonk->setBounds( w, h );
         ctx->labelBonk->setText( "Bonk" );
      }

      ctx->controlsInit= true;
   }

   playing= ctx->pipeLineLoop->playing;
   color= playing ? BUTTON_ACTIVE_COLOR : BUTTON_INACTIVE_COLOR;
   fillRect( ctx, ctx->loopX, ctx->loopY, ctx->loopW, ctx->loopH, color );
   if ( ctx->labelLoop )
   {
      color= playing ? ACTIVE_TEXT_COLOR : NORMAL_TEXT_COLOR;
      ctx->labelLoop->setColor(color);
      ctx->labelLoop->draw();
   }

   playing= (ctx->laserPlaying && ctx->pipeLineOneShot->playing);
   color= playing ? BUTTON_ACTIVE_COLOR : BUTTON_INACTIVE_COLOR;
   fillRect( ctx, ctx->laserX, ctx->laserY, ctx->laserW, ctx->laserH, color );
   if ( ctx->labelLaser )
   {
      color= playing ? ACTIVE_TEXT_COLOR : NORMAL_TEXT_COLOR;
      ctx->labelLaser->setColor(color);
      ctx->labelLaser->draw();
   }
   ctx->laserPlaying= playing;

   playing= (ctx->glassPlaying && ctx->pipeLineOneShot->playing);
   color=  playing ? BUTTON_ACTIVE_COLOR : BUTTON_INACTIVE_COLOR;
   fillRect( ctx, ctx->glassX, ctx->glassY, ctx->glassW, ctx->glassH, color );
   if ( ctx->labelGlass )
   {
      color= playing ? ACTIVE_TEXT_COLOR : NORMAL_TEXT_COLOR;
      ctx->labelGlass->setColor(color);
      ctx->labelGlass->draw();
   }
   ctx->glassPlaying= playing;

   playing= (ctx->doorPlaying && ctx->pipeLineOneShot->playing);
   color=  playing ? BUTTON_ACTIVE_COLOR : BUTTON_INACTIVE_COLOR;
   fillRect( ctx, ctx->doorX, ctx->doorY, ctx->doorW, ctx->doorH, color );
   if ( ctx->labelDoor )
   {
      color= playing ? ACTIVE_TEXT_COLOR : NORMAL_TEXT_COLOR;
      ctx->labelDoor->setColor(color);
      ctx->labelDoor->draw();
   }
   ctx->doorPlaying= playing;

   playing= (ctx->bonkPlaying && ctx->pipeLineOneShot->playing);
   color=  playing ? BUTTON_ACTIVE_COLOR : BUTTON_INACTIVE_COLOR;
   fillRect( ctx, ctx->bonkX, ctx->bonkY, ctx->bonkW, ctx->bonkH, color );
   if ( ctx->labelBonk )
   {
      color= playing ? ACTIVE_TEXT_COLOR : NORMAL_TEXT_COLOR;
      ctx->labelBonk->setColor(color);
      ctx->labelBonk->draw();
   }
   ctx->bonkPlaying= playing;
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

   drawControls( ctx );

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
      if ( (x > ctx->loopX) && (x < ctx->loopX+ctx->loopW) &&
           (y > ctx->loopY) && (y < ctx->loopY+ctx->loopH) )
      {
         if ( ctx->pipeLineLoop->playing )
         {
            stopClip( ctx->pipeLineLoop );
         }
         else
         {
            playClip( ctx->pipeLineLoop, "/usr/share/audioserver/alone.wav" );
         }
         ctx->dirty= true;
      }
      else
      if ( (x > ctx->laserX) && (x < ctx->laserX+ctx->laserW) &&
           (y > ctx->laserY) && (y < ctx->laserY+ctx->laserH) )
      {
         playClip( ctx->pipeLineOneShot, "/usr/share/audioserver/apocalypse.wav" );
         ctx->laserPlaying= true;
         ctx->dirty= true;
      }
      else
      if ( (x > ctx->glassX) && (x < ctx->glassX+ctx->glassW) &&
           (y > ctx->glassY) && (y < ctx->glassY+ctx->glassH) )
      {
         playClip( ctx->pipeLineOneShot, "/usr/share/audioserver/forest.wav" );
         ctx->glassPlaying= true;
         ctx->dirty= true;
      }
      else
      if ( (x > ctx->doorX) && (x < ctx->doorX+ctx->doorW) &&
           (y > ctx->doorY) && (y < ctx->doorY+ctx->doorH) )
      {
         playClip( ctx->pipeLineOneShot, "/usr/share/audioserver/spooky.wav" );
         ctx->doorPlaying= true;
         ctx->dirty= true;
      }
      else
      if ( (x > ctx->bonkX) && (x < ctx->bonkX+ctx->bonkW) &&
           (y > ctx->bonkY) && (y < ctx->bonkY+ctx->bonkH) )
      {
         playClip( ctx->pipeLineOneShot, "/usr/share/audioserver/wow.wav" );
         ctx->bonkPlaying= true;
         ctx->dirty= true;
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
   printf(" aseffects [options]\n" );
   printf("where [options] are:\n" );
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

   printf("aseffects v1.0\n");
   ctx= (AppCtx*)calloc( 1, sizeof(AppCtx) );
   if ( !ctx )
   {
      printf("Error: no memory for app context\n");
      goto exit;
   }

   ctx->windowX= 100;
   ctx->windowY= 100;
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

   if ( !EssContextSetName( ctx->essCtx, "aseffects" ) )
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
      if ( !initTitle( ctx, "aseffects" ) )
      {
         error= true;
      }
      else
      if ( !initGst( ctx ) )
      {
         error= true;
      }

      if ( !error )
      {
         gRunning= true;
         while( gRunning )
         {
            if ( ctx->dirty )
            {
               ctx->dirty= false;
               renderGL( ctx );
               EssContextUpdateDisplay( ctx->essCtx );
            }
            EssContextRunEventLoopOnce( ctx->essCtx );

            g_main_context_iteration( NULL, FALSE );
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

