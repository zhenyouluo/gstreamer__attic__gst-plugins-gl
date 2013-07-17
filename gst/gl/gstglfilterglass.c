/*
 * GStreamer
 * Copyright (C) 2008 Julien Isorce <julien.isorce@gmail.com>
 * Inspired from http://www.mdk.org.pl/2007/11/17/gl-colorspace-conversions
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin St, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

/**
 * SECTION:element-glfilterglass
 *
 * Map textures on moving glass.
 *
 * <refsect2>
 * <title>Examples</title>
 * |[
 * gst-launch -v videotestsrc ! glupload ! glfilterglass ! glimagesink
 * ]| A pipeline inspired from http://www.mdk.org.pl/2007/11/17/gl-colorspace-conversions
 * FBO is required.
 * |[
 * gst-launch -v videotestsrc ! glupload ! glfilterglass ! "video/x-raw-gl, width=640, height=480" ! glimagesink
 * ]| The scene is greater than the input size.
 * </refsect2>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <math.h>
#include "gstglfilterglass.h"

#define GST_CAT_DEFAULT gst_gl_filter_glass_debug
GST_DEBUG_CATEGORY_STATIC (GST_CAT_DEFAULT);

enum
{
  PROP_0
};

#define DEBUG_INIT \
  GST_DEBUG_CATEGORY_INIT (gst_gl_filter_glass_debug, "glfilterglass", 0, "glfilterglass element");

G_DEFINE_TYPE_WITH_CODE (GstGLFilterGlass, gst_gl_filter_glass,
    GST_TYPE_GL_FILTER, DEBUG_INIT);

static void gst_gl_filter_glass_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec);
static void gst_gl_filter_glass_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec);

static void gst_gl_filter_glass_reset (GstGLFilter * filter);
static gboolean gst_gl_filter_glass_init_shader (GstGLFilter * filter);
static gboolean gst_gl_filter_glass_filter_texture (GstGLFilter * filter,
    guint in_tex, guint out_tex);

static void gst_gl_filter_glass_draw_background_gradient ();
static void gst_gl_filter_glass_draw_video_plane (GstGLFilter * filter,
    gint width, gint height, guint texture, gfloat center_x, gfloat center_y,
    gfloat start_alpha, gfloat stop_alpha, gboolean reversed, gfloat rotation);

static void gst_gl_filter_glass_callback (gpointer stuff);

/* *INDENT-OFF* */
static const gchar *glass_fragment_source =
    "#extension GL_ARB_texture_rectangle : enable\n"
    "uniform sampler2DRect tex;\n"
    "uniform float width, height;\n"
    "varying float alpha;\n"
    "void main () {\n"
    "  float p = 0.0525;\n"
    "  float L1 = p*width;\n"
    "  float L2 = width - L1;\n"
    "  float L3 = height - L1;\n"
    "  float w = 1.0;\n"
    "  float r = L1;\n"
    "  if (gl_TexCoord[0].x < L1 && gl_TexCoord[0].y < L1)\n"
    "      r = sqrt( (gl_TexCoord[0].x - L1) * (gl_TexCoord[0].x - L1) + (gl_TexCoord[0].y - L1) * (gl_TexCoord[0].y - L1) );\n"
    "  else if (gl_TexCoord[0].x > L2 && gl_TexCoord[0].y < L1)\n"
    "      r = sqrt( (gl_TexCoord[0].x - L2) * (gl_TexCoord[0].x - L2) + (gl_TexCoord[0].y - L1) * (gl_TexCoord[0].y - L1) );\n"
    "  else if (gl_TexCoord[0].x > L2 && gl_TexCoord[0].y > L3)\n"
    "      r = sqrt( (gl_TexCoord[0].x - L2) * (gl_TexCoord[0].x - L2) + (gl_TexCoord[0].y - L3) * (gl_TexCoord[0].y - L3) );\n"
    "  else if (gl_TexCoord[0].x < L1 && gl_TexCoord[0].y > L3)\n"
    "      r = sqrt( (gl_TexCoord[0].x - L1) * (gl_TexCoord[0].x - L1) + (gl_TexCoord[0].y - L3) * (gl_TexCoord[0].y - L3) );\n"
    "  if (r > L1)\n"
    "      w = 0.0;\n"
    "  vec4 color = texture2DRect (tex, gl_TexCoord[0].st);\n"
    "  gl_FragColor = vec4(color.rgb, alpha * w);\n"
    "}\n";

static const gchar *glass_vertex_source =
    "uniform float yrot;\n"
    "uniform float width, height;\n"
    "const float fovy = 80.0;\n"
    "const float znear = 1.0;\n"
    "const float zfar = 5000.0;\n"
    "varying float alpha;\n"
    "void main () {\n"
    "   float f = 1.0/(tan(radians(fovy/2.0)));\n"
    "   float rot = radians (yrot);\n"
    "   float aspect = width/height;\n"
    "   // replacement for gluPerspective\n"
    "   mat4 perspective = mat4 (\n"
    "            f/aspect, 0.0,  0.0,                      0.0,\n"
    "            0.0,      f,    0.0,                      0.0,\n"
    "            0.0,      0.0, (znear+zfar)/(znear-zfar), 2.0*znear*zfar/(znear-zfar),\n"
    "            0.0,      0.0, -1.0,                      0.0 );\n"
    "   mat4 trans = mat4 (\n"
    "            1.0, 0.0, 0.0, 0.0,\n"
    "            0.0, 1.0, 0.0, 0.0,\n"
    "            0.0, 0.0, 1.0, -3.0,\n"
    "            0.0, 0.0, 0.0, 1.0 );\n"
    "   mat4 rotation = mat4 (\n"
    "            cos(rot),  0.0, sin(rot), 0.0,\n"
    "            0.0,       1.0, 0.0,      0.0,\n"
    "            -sin(rot), 0.0, cos(rot), 0.0,\n"
    "            0.0,       0.0, 0.0,      1.0 );\n"
    "  gl_Position = trans * perspective * rotation * gl_ModelViewProjectionMatrix * gl_Vertex;\n"
    "  gl_TexCoord[0] = gl_MultiTexCoord0;\n"
    "  alpha = gl_Color.a;\n"
    "}\n";

static const gchar * passthrough_vertex = 
    "void main () {\n"
    "  gl_Position = gl_ModelViewProjectionMatrix * gl_Vertex;\n"
    "  gl_FrontColor = gl_Color;\n"
    "}\n";

static const gchar * passthrough_fragment = 
    "void main () {\n"
    "  gl_FragColor = gl_Color;\n"
    "}\n";
/* *INDENT-ON* */

static void
gst_gl_filter_glass_class_init (GstGLFilterGlassClass * klass)
{
  GObjectClass *gobject_class;
  GstElementClass *element_class;

  gobject_class = (GObjectClass *) klass;
  element_class = GST_ELEMENT_CLASS (klass);

  gobject_class->set_property = gst_gl_filter_glass_set_property;
  gobject_class->get_property = gst_gl_filter_glass_get_property;

  gst_element_class_set_metadata (element_class, "OpenGL glass filter",
      "Filter/Effect/Video", "Glass Filter",
      "Julien Isorce <julien.isorce@gmail.com>");

  GST_GL_FILTER_CLASS (klass)->filter_texture =
      gst_gl_filter_glass_filter_texture;
  GST_GL_FILTER_CLASS (klass)->onInitFBO = gst_gl_filter_glass_init_shader;
  GST_GL_FILTER_CLASS (klass)->onReset = gst_gl_filter_glass_reset;
}

static void
gst_gl_filter_glass_init (GstGLFilterGlass * filter)
{
  filter->shader = NULL;
  filter->timestamp = 0;
}

static void
gst_gl_filter_glass_reset (GstGLFilter * filter)
{
  GstGLFilterGlass *glass_filter = GST_GL_FILTER_GLASS (filter);

  //blocking call, wait the opengl thread has destroyed the shader
  gst_gl_display_del_shader (filter->display, glass_filter->shader);
  gst_gl_display_del_shader (filter->display, glass_filter->passthrough_shader);
}

static void
gst_gl_filter_glass_set_property (GObject * object, guint prop_id,
    const GValue * value, GParamSpec * pspec)
{
  //GstGLFilterGlass *filter = GST_GL_FILTER_GLASS (object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static void
gst_gl_filter_glass_get_property (GObject * object, guint prop_id,
    GValue * value, GParamSpec * pspec)
{
  //GstGLFilterGlass *filter = GST_GL_FILTER_GLASS (object);

  switch (prop_id) {
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, prop_id, pspec);
      break;
  }
}

static gboolean
gst_gl_filter_glass_init_shader (GstGLFilter * filter)
{
  gboolean ret;
  GstGLFilterGlass *glass_filter = GST_GL_FILTER_GLASS (filter);

  //blocking call, wait the opengl thread has compiled the shader
  ret =
      gst_gl_display_gen_shader (filter->display, glass_vertex_source,
      glass_fragment_source, &glass_filter->shader);
  if (ret)
    ret =
        gst_gl_display_gen_shader (filter->display, passthrough_vertex,
        passthrough_fragment, &glass_filter->passthrough_shader);

  return ret;
}

static gboolean
gst_gl_filter_glass_filter_texture (GstGLFilter * filter, guint in_tex,
    guint out_tex)
{
  GstGLFilterGlass *glass_filter = GST_GL_FILTER_GLASS (filter);
  glass_filter->in_tex = in_tex;

  //blocking call, use a FBO
  gst_gl_display_use_fbo_v2 (filter->display,
      GST_VIDEO_INFO_WIDTH (&filter->out_info),
      GST_VIDEO_INFO_HEIGHT (&filter->out_info),
      filter->fbo, filter->depthbuffer, out_tex,
      gst_gl_filter_glass_callback, (gpointer) glass_filter);

  return TRUE;
}

static gint64
get_time (void)
{
  static GTimeVal val;
  g_get_current_time (&val);

  return (val.tv_sec * G_USEC_PER_SEC) + val.tv_usec;
}

static void
gst_gl_filter_glass_draw_background_gradient (GstGLFilterGlass * glass)
{
  GstGLFilter *filter = GST_GL_FILTER (glass);
  GstGLFuncs *gl = filter->display->gl_vtable;

/* *INDENT-OFF* */
  gfloat mesh[] = {
  /* |       Vertex       |        Color         | */
      -1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
       1.0f, -1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f,
       1.0f,  0.8f, 0.0f, 0.0f, 0.0f, 0.2f, 1.0f,
      -1.0f,  0.8f, 0.0f, 0.0f, 0.0f, 0.2f, 1.0f,
      -1.0f,  1.0f, 0.0f, 0.0f, 0.0f, 0.2f, 1.0f,
       1.0f,  1.0f, 0.0f, 0.0f, 0.0f, 0.2f, 1.0f,
  };
/* *INDENT-ON* */

  GLushort indices[] = {
    0, 1, 2,
    0, 2, 3,
    2, 3, 4,
    2, 4, 5
  };

  gl->ClientActiveTexture (GL_TEXTURE0);
  gl->EnableClientState (GL_VERTEX_ARRAY);
  gl->EnableClientState (GL_COLOR_ARRAY);

  gl->VertexPointer (3, GL_FLOAT, 7 * sizeof (gfloat), mesh);
  gl->ColorPointer (4, GL_FLOAT, 7 * sizeof (gfloat), &mesh[3]);

  gl->DrawElements (GL_TRIANGLES, 12, GL_UNSIGNED_SHORT, indices);

  gl->DisableClientState (GL_VERTEX_ARRAY);
  gl->DisableClientState (GL_COLOR_ARRAY);
}

static void
gst_gl_filter_glass_draw_video_plane (GstGLFilter * filter,
    gint width, gint height, guint texture,
    gfloat center_x, gfloat center_y,
    gfloat start_alpha, gfloat stop_alpha, gboolean reversed, gfloat rotation)
{
  GstGLFilterGlass *glass_filter = GST_GL_FILTER_GLASS (filter);
  GstGLFuncs *gl = filter->display->gl_vtable;

  gfloat w = (gfloat) width;
  gfloat h = (gfloat) height;
  gfloat topy = reversed ? center_y - 1.0f : center_y + 1.0f;
  gfloat bottomy = reversed ? center_y + 1.0f : center_y - 1.0f;

/* *INDENT-OFF* */
  gfloat mesh[] = {
 /*|           Vertex          |TexCoord0|      Colour               |*/
    center_x-1.6, topy,    0.0, 0.0, h,   1.0, 1.0, 1.0, start_alpha,
    center_x+1.6, topy,    0.0, w,   h,   1.0, 1.0, 1.0, start_alpha,
    center_x+1.6, bottomy, 0.0, w,   0.0, 1.0, 1.0, 1.0, stop_alpha,
    center_x-1.6, bottomy, 0.0, 0.0, 0.0, 1.0, 1.0, 1.0, stop_alpha,
  };
/* *INDENT-ON* */

  GLushort indices[] = {
    0, 1, 2,
    0, 2, 3
  };

  gl->ActiveTexture (GL_TEXTURE0);
  gl->Enable (GL_TEXTURE_RECTANGLE_ARB);
  gl->BindTexture (GL_TEXTURE_RECTANGLE_ARB, texture);
  gl->Disable (GL_TEXTURE_RECTANGLE_ARB);

  gst_gl_shader_set_uniform_1i (glass_filter->shader, "tex", 0);
  gst_gl_shader_set_uniform_1f (glass_filter->shader, "width", (gfloat) width);
  gst_gl_shader_set_uniform_1f (glass_filter->shader, "height",
      (gfloat) height);
  gst_gl_shader_set_uniform_1f (glass_filter->shader, "yrot", rotation);

  gl->ClientActiveTexture (GL_TEXTURE0);
  gl->EnableClientState (GL_TEXTURE_COORD_ARRAY);
  gl->EnableClientState (GL_VERTEX_ARRAY);
  gl->EnableClientState (GL_COLOR_ARRAY);

  gl->VertexPointer (3, GL_FLOAT, 9 * sizeof (gfloat), mesh);
  gl->TexCoordPointer (2, GL_FLOAT, 9 * sizeof (gfloat), &mesh[3]);
  gl->ColorPointer (4, GL_FLOAT, 9 * sizeof (gfloat), &mesh[5]);

  gl->DrawElements (GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, indices);

  gl->DisableClientState (GL_TEXTURE_COORD_ARRAY);
  gl->DisableClientState (GL_VERTEX_ARRAY);
  gl->DisableClientState (GL_COLOR_ARRAY);
}

//opengl scene, params: input texture (not the output filter->texture)
static void
gst_gl_filter_glass_callback (gpointer stuff)
{
  static gint64 start_time = 0;
  gfloat rotation;

  GstGLFilter *filter = GST_GL_FILTER (stuff);
  GstGLFilterGlass *glass_filter = GST_GL_FILTER_GLASS (stuff);
  GstGLFuncs *gl = filter->display->gl_vtable;

  gint width = GST_VIDEO_INFO_WIDTH (&filter->out_info);
  gint height = GST_VIDEO_INFO_HEIGHT (&filter->out_info);
  guint texture = glass_filter->in_tex;

  if (start_time == 0)
    start_time = get_time ();
  else {
    gint64 time_left =
        (glass_filter->timestamp / 1000) - (get_time () - start_time);
    time_left -= 1000000 / 25;
    if (time_left > 2000) {
      GST_LOG ("escape");
      return;
    }
  }

  gst_gl_shader_use (glass_filter->passthrough_shader);

  gst_gl_filter_glass_draw_background_gradient (glass_filter);

  //Rotation
  if (start_time != 0) {
    gint64 time_passed = get_time () - start_time;
    rotation = sin (time_passed / 1200000.0) * 45.0f;
  } else {
    rotation = 0.0f;
  }

  gl->Enable (GL_BLEND);
  gl->BlendFunc (GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  gst_gl_shader_use (glass_filter->shader);

  //Reflection
  gst_gl_filter_glass_draw_video_plane (filter, width, height, texture,
      0.0f, 2.0f, 0.3f, 0.0f, TRUE, rotation);

  //Main video
  gst_gl_filter_glass_draw_video_plane (filter, width, height, texture,
      0.0f, 0.0f, 1.0f, 1.0f, FALSE, rotation);

  gst_gl_display_clear_shader (filter->display);

  gl->Disable (GL_TEXTURE_RECTANGLE_ARB);
  gl->Disable (GL_BLEND);
}
