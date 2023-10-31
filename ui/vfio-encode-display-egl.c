#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qapi/error.h"
#include "sysemu/sysemu.h"
#include "ui/console.h"
#include "ui/egl-helpers.h"
#include "ui/egl-context.h"
#include "ui/shader.h"
#include "gst/gst.h"
#include "gst/gstinfo.h"
#include "gst/allocators/gstdmabuf.h"
#include "gst/video/gstvideometa.h"
#include "gst/app/gstappsrc.h"
#include "gst/gl/gl.h"
#include "gst/gl/gstglmemory.h"


typedef struct vfio_encode_gdata{
    GstElement *pipeline;
    GstAllocator *allocator;
    GstElement *source;
    GstElement *sink;
    GstElement *conv;
    GstElement *filter;
    GstBuffer* buffer;
    GstGLVideoAllocationParams *allocation_params;
    GstGLContext *gl_context;
    GstGLDisplay *gl_display;

 //   GstVaDisplay * display;
} vfio_encode_gdata;

typedef struct vfio_encode_dpy {
    DisplayChangeListener dcl;
    QemuGLShader *gls;
    DisplaySurface *ds;
    egl_fb guest_fb;
    egl_fb cursor_fb;
    egl_fb blit_fb;
    bool y_0_top;
    uint32_t pos_x;
    uint32_t pos_y;
    uint32_t primary_width;
    uint32_t primary_height;
    uint32_t primary_stride;
    uint32_t primary_fourcc;
    uint64_t primary_modifier;
    vfio_encode_gdata gdata;
} vfio_encode_dpy;
GstClockTime timestamp = 0;


/* ------------------------------------------------------------------ */
static void vfio_encode_set_gstreamer_params (vfio_encode_dpy *vedpy)
{
    GstVideoInfo *vinfo = gst_video_info_new ();
    gboolean ret = gst_video_info_set_format (vinfo, GST_VIDEO_FORMAT_RGBA, vedpy->primary_width, vedpy->primary_height);
    if (!ret) {
        fprintf (stderr, "set video info ret %d\n", ret);
        exit (EXIT_FAILURE);
    }
    vedpy->gdata.allocation_params=
        gst_gl_video_allocation_params_new_wrapped_texture (vedpy->gdata.gl_context, NULL,
        vinfo, 0, NULL, GST_GL_TEXTURE_TARGET_2D, GST_GL_RGBA8, vedpy->blit_fb.texture,
        NULL, 0);
}

static void vfio_encode_refresh(DisplayChangeListener *dcl)
{
    graphic_hw_update(dcl->con);



}

static void vfio_encode_release_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{
    vfio_encode_dpy *vedpy = container_of(dcl, vfio_encode_dpy, dcl);

    egl_dmabuf_release_texture(dmabuf);
    if (vedpy->guest_fb.dmabuf == dmabuf) {
        vedpy->guest_fb.dmabuf = NULL;
    }
}
/*
static void notify_to_destroy (gpointer user_data)
{
    GST_INFO ("NvBufferDestroy(%d)", *(int *)user_data);
   // NvBufferDestroy(*(int *)user_data);
    g_free(user_data);
}
*/
static void gd_egl_scanout_texture(DisplayChangeListener *dcl,
                            uint32_t backing_id, bool backing_y_0_top,
                            uint32_t backing_width, uint32_t backing_height,
                            uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h,
                            void *d3d_tex2d)
{
    vfio_encode_dpy *vedpy = container_of(dcl, vfio_encode_dpy, dcl);





 
    egl_fb_setup_for_tex(&vedpy->guest_fb, backing_width, backing_height,
                         backing_id, false);
}

static void vfio_encode_scanout_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{
    vfio_encode_dpy *vedpy = container_of(dcl, vfio_encode_dpy, dcl);

  
    egl_dmabuf_import_texture(dmabuf);
    if (!dmabuf->texture) {
        return;
    }


    if(vedpy->primary_width != dmabuf->width|| vedpy->primary_height != dmabuf->height) {
        // size changed
        printf("plane size changed \n");
   //     vedpy->primary_width = dmabuf->width;
    //    vedpy->primary_height = dmabuf->height; 
    //    vfio_encode_set_gstreamer_params(vedpy);
        // TODO maybe change stream?
    }


    gd_egl_scanout_texture(dcl, dmabuf->texture,
                           dmabuf->y0_top,
                           dmabuf->backing_width, dmabuf->backing_height,
                           dmabuf->x, dmabuf->y, dmabuf->width,
                           dmabuf->height, NULL);
    if (dmabuf->allow_fences) {
        vedpy->guest_fb.dmabuf = dmabuf;
    }
    
 


                       
}


static void vfio_encode_cursor_dmabuf(DisplayChangeListener *dcl,
                              QemuDmaBuf *dmabuf, bool have_hot,
                              uint32_t hot_x, uint32_t hot_y)
{
    vfio_encode_dpy *vedpy = container_of(dcl, vfio_encode_dpy, dcl);

    if (dmabuf) {
        egl_dmabuf_import_texture(dmabuf);
        if (!dmabuf->texture) {
            return;
        }
        egl_fb_setup_for_tex(&vedpy->cursor_fb,
                             dmabuf->backing_width, dmabuf->backing_height,
                             dmabuf->texture, false);
    } else {
        egl_fb_destroy(&vedpy->cursor_fb);
    }
}

static void vfio_encode_cursor_position(DisplayChangeListener *dcl,
                                uint32_t pos_x, uint32_t pos_y)
{
    vfio_encode_dpy *vedpy = container_of(dcl, vfio_encode_dpy, dcl);

    vedpy->pos_x = pos_x;
    vedpy->pos_y = pos_y;
}

static GstBuffer * vfio_encode_wrap_gl_texture ( vfio_encode_dpy *vedpy)
{
  GstGLMemoryAllocator *allocator;
  gpointer wrapped[1];
  GstGLFormat formats[1];
  GstBuffer *buffer;
  gboolean ret;

  allocator = gst_gl_memory_allocator_get_default (vedpy->gdata.gl_context);

  buffer = gst_buffer_new ();
  if (!buffer) {
    g_error ("Failed to create new buffer\n");
    return NULL;
  }

  wrapped[0] = ( gpointer ) &vedpy->blit_fb.texture;
  formats[0] = GST_GL_RGBA8;

  /* Wrap the texture into GLMemory. */
  ret = gst_gl_memory_setup_buffer (allocator, buffer, vedpy->gdata.allocation_params,
      formats, wrapped, 1);
  if (!ret) {
    g_error ("Failed to setup gl memory\n");
    return NULL;
  }

  gst_object_unref (allocator);

  return buffer;
}


static void vfio_push_texture( vfio_encode_dpy *vedpy )
{
    GstBuffer * buffer;
    GstFlowReturn ret;


    buffer = vfio_encode_wrap_gl_texture (vedpy);

    GstGLSyncMeta *sync_meta = gst_buffer_get_gl_sync_meta (buffer);
    if (sync_meta) {
        gst_gl_sync_meta_set_sync_point (sync_meta, vedpy->gdata.gl_context);
        gst_gl_sync_meta_wait (sync_meta, vedpy->gdata.gl_context);
    }
    ret = gst_app_src_push_buffer (GST_APP_SRC (vedpy->gdata.source), buffer);
    if (ret != GST_FLOW_OK) {
        g_printerr ("GST_FLOW != OK, return value is %d\n", ret);
    }


}
static void vfio_encode_scanout_texture(DisplayChangeListener *dcl,
                                uint32_t backing_id,
                                bool backing_y_0_top,
                                uint32_t backing_width,
                                uint32_t backing_height,
                                uint32_t x, uint32_t y,
                                uint32_t w, uint32_t h,
                                void *d3d_tex2d)
{
    vfio_encode_dpy *vedpy = container_of(dcl, vfio_encode_dpy, dcl);

    vedpy->y_0_top = backing_y_0_top;

    /* source framebuffer */
    egl_fb_setup_for_tex(&vedpy->guest_fb,
                         backing_width, backing_height, backing_id, false);

    /* dest framebuffer */
    if (vedpy->blit_fb.width  != backing_width ||
        vedpy->blit_fb.height != backing_height) {
        egl_fb_destroy(&vedpy->blit_fb);
        egl_fb_setup_new_tex(&vedpy->blit_fb, backing_width, backing_height);
    }
}

static void vfio_encode_update(DisplayChangeListener *dcl,
                   uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    vfio_encode_dpy *vedpy = container_of(dcl, vfio_encode_dpy, dcl);

    //////
    /* source framebuffer */


    /* dest framebuffer */
  //  if (vedpy->blit_fb.width  != backing_width ||
   //     vedpy->blit_fb.height != backing_height) {
    //    egl_fb_destroy(&vedpy->blit_fb);
     //   egl_fb_setup_new_tex(&vedpy->blit_fb, backing_width, backing_height);
   // }


    ////

    if (!vedpy->guest_fb.texture ){//|| !vedpy->ds) {
        return;
    }
    //assert(surface_format(vedpy->ds) == PIXMAN_x8r8g8b8);

    if (vedpy->cursor_fb.texture) {
        /* have cursor -> render using textures */
        //TODO BOOL?
        egl_texture_blit(vedpy->gls, &vedpy->blit_fb, &vedpy->guest_fb,
                         true);
        egl_texture_blend(vedpy->gls, &vedpy->blit_fb, &vedpy->cursor_fb,
                          true, vedpy->pos_x, vedpy->pos_y,
                          1.0, 1.0);
    } else {
        /* no cursor -> use simple framebuffer blit */
        egl_fb_blit(&vedpy->blit_fb, &vedpy->guest_fb, false);

    }

    vfio_push_texture(vedpy);
}



static void vfio_encode_scanout_disable(DisplayChangeListener *dcl)
{

}


static void vfio_encode_gfx_update(DisplayChangeListener *dcl,
                              int x, int y,
                              int w, int h)
{

}

static const DisplayChangeListenerOps vegl_ops = {
    .dpy_name                = "vfio-encode",
    .dpy_refresh             = vfio_encode_refresh,
    
    .dpy_gl_release_dmabuf   = vfio_encode_release_dmabuf,
    .dpy_gl_scanout_dmabuf   = vfio_encode_scanout_dmabuf,
    .dpy_gl_cursor_dmabuf    = vfio_encode_cursor_dmabuf,
    .dpy_gl_cursor_position  = vfio_encode_cursor_position,
    .dpy_gl_update           = vfio_encode_update,
    .dpy_gl_scanout_disable  = vfio_encode_scanout_disable,
    .dpy_gl_scanout_texture  = vfio_encode_scanout_texture,
    //.dpy_gfx_update_full     = vfio_encode_update_full,
    //.dpy_gfx_replace_surface = vfio_encode_replace_surface,
    .dpy_gfx_update          = vfio_encode_gfx_update,


};


static QEMUGLContext vfio_encode_create_context(DisplayGLCtx *dgc,
                                        QEMUGLParams *params)
{

    return qemu_egl_create_context(dgc, params);
}

static bool
vfio_encode_is_compatible_dcl(DisplayGLCtx *dgc,
                      DisplayChangeListener *dcl)
{
    if (!dcl->ops->dpy_gl_update) {
        /*
         * egl-headless is compatible with all 2d listeners, as it blits the GL
         * updates on the 2d console surface.
         */
        return true;
    }

    return dcl->ops == &vegl_ops;
}

static const DisplayGLCtxOps veglctx_ops = {
    .dpy_gl_ctx_is_compatible_dcl = vfio_encode_is_compatible_dcl,
    .dpy_gl_ctx_create       = vfio_encode_create_context,
    .dpy_gl_ctx_destroy      = qemu_egl_destroy_context,
    .dpy_gl_ctx_make_current = qemu_egl_make_context_current,
};

static void vfio_encode_error_cb (GstBus *bus, GstMessage *msg, void * data) {
    GError *err;
    gchar *debug_info;

    gst_message_parse_error (msg, &err, &debug_info);
    g_printerr ("Error received from element %s: %s\n", GST_OBJECT_NAME (msg->src), err->message);
    g_printerr ("Debugging information: %s\n", debug_info ? debug_info : "none");
   
    g_clear_error (&err);
    g_free (debug_info);

}

static bool vfio_encode_setup_gstreamer(vfio_encode_gdata *gdata) {
    guint major, minor, micro, nano;
    const gchar *nano_str;
    gst_init (NULL, NULL);
    gst_version (&major, &minor, &micro, &nano);
    if (nano == 1)
        nano_str = "(CVS)";
    else if (nano == 2)
        nano_str = "(Prerelease)";
    else
        nano_str = "";

    printf ("This program is linked against GStreamer %d.%d.%d %s\n",
          major, minor, micro, nano_str);


    gdata->gl_display = gst_gl_display_new_with_type(GST_GL_DISPLAY_TYPE_EGL_DEVICE);
    gdata->gl_context =  gst_gl_context_new( gdata->gl_display);
    GstBus *bus;
    //GstCaps *filtercaps;
    //////////////////////

    
    char launch_stream[]  = 
    
    "appsrc name=source is-live=TRUE do-timestamp=TRUE format=time ! " // leaky-type=2 format=3 ! "
    " video/x-raw(memory:GLMemory), width=1024, height=768, framerate=(fraction)30/1, format=(string)RGBA, texture-target=(string)2D! "//{BGRx,BGRx:0x0100000000000001} ! "

    //" video/x-raw(memory:DMABuf),width=1024,height=768,framerate=(fraction)30/1, drm-format=BGRx:0x0100000000000001 !"
   " glcolorconvert !"
   " gldownload !"
   //" videotestsrc !"
   //" video/x-raw(memory:DMABuf),format=(string)NV12 ! "
   //" video/x-raw(memory:DMABuf),width=1024,height=768,framerate=0/1,format=NV12 !"
   // "  vah264enc ! h264parse ! mp4mux ! filesink location=../test.mp4";
    //"  video/x-raw(memory:DMABuf), width=1024, height=768, interlace-mode=progressive  !"
    //" vaapisink "; 
   // " timeoverlay !"
   " autovideoconvert !"
    "  autovideosink";
    
    //" glupload !"
   // " glcolorconvert ! glimagesinkelement";
    // "autovideoconvert name=conv ! "
    // "videorate ! "
    //"videoscale !" 
  //  "msdkvpp !"
   // "videoconvert name=conv ! "
  //  "autovideosink name=sink ";


    g_print("Using launch string: %s\n", launch_stream);

    GError *error = NULL;
    gdata->pipeline =  gst_parse_launch(launch_stream, &error);
    if (gdata->pipeline == NULL) {
        g_print( "Failed to parse launch: %s\n", error->message);
        return -1;
    }
    
    gdata->source = gst_bin_get_by_name(GST_BIN(gdata->pipeline), "source");
    gst_app_src_set_stream_type(GST_APP_SRC(gdata->source), GST_APP_STREAM_TYPE_STREAM);

    gdata->sink = gst_bin_get_by_name(GST_BIN(gdata->pipeline), "sink");
    gdata->conv = gst_bin_get_by_name(GST_BIN(gdata->pipeline), "conv");
    
    /*/
    gdata->pipeline = gst_pipeline_new ("test-pipeline");

    gdata->source = gst_element_factory_make ("appsrc", "source");
    gdata->filter = gst_element_factory_make ("capsfilter", "filter");
   // GstElement * vappipost = gst_element_factory_make ("vaapipostproc", "postproc");
    gdata->conv = gst_element_factory_make ("videoconvert", "conv");
    gdata->sink = gst_element_factory_make ("autovideosink", "sink");
*/
    //source = gst_element_factory_make ("videotestsrc", "source");
   
   /*
    g_object_set (G_OBJECT (gdata->source), "caps",
        gst_caps_from_string("video/x-raw(memory:DMABuf), width=(int)1024, height=(int)768, drm-format=(string)BGRx"), NULL);
       
       
  		gst_caps_new_simple ("video/x-raw",
				     "format", G_TYPE_STRING, "BGRx",
				     "width", G_TYPE_INT, 1024,
				     "height", G_TYPE_INT, 768,
				     "framerate", GST_TYPE_FRACTION, 0, 1,
				     NULL), NULL);*/
 /*
    if (!gdata->pipeline || !gdata->sink || !gdata->filter ||  !gdata->source || !gdata->conv ) {
        g_printerr ("vfio_encode: Not all Gstreamer Pipeline elements could be created.\n");
        return false;
    }
    gst_bin_add_many (GST_BIN (gdata->pipeline), gdata->source, gdata->filter,  gdata->conv, gdata->sink, NULL);
    if (gst_element_link_many (gdata->source, gdata->filter, gdata->conv, gdata->sink, NULL) != TRUE) {
        g_printerr ("vfio_encode: Not all Gstreamer Pipeline elements could be linked.\n");
        gst_object_unref (gdata->pipeline);
        return false;
    }


    filtercaps = gst_caps_from_string("video/x-raw(memory:DMABuf),width=1024,height=768,framerate=60/1,drm-format=BGRx");
    g_object_set (G_OBJECT (gdata->filter), "caps", filtercaps, NULL);
    gst_caps_unref (filtercaps);
       */

    bus = gst_element_get_bus (gdata->pipeline);
    gst_bus_add_signal_watch (bus);
    g_signal_connect (G_OBJECT (bus), "message::error", (GCallback)vfio_encode_error_cb, NULL);
    gst_object_unref (bus);

    // test
   // g_object_set (gdata->source, "pattern", 0, NULL);
    gdata->allocator = gst_dmabuf_allocator_new();

    //gdata->display = gst_va_display_drm_new_from_path("/sys/bus/mdev/devices/536c0c61-b6ed-4227-9f3a-55737a67779f");
   // gdata->allocator = gst_va_dmabuf_allocator_new (gdata->display);
    GstStateChangeReturn rets = gst_element_set_state (gdata->pipeline, GST_STATE_PLAYING);



    if (rets == GST_STATE_CHANGE_FAILURE) {
        g_printerr ("Unable to set the pipeline to the playing state.\n");
        gst_object_unref (gdata->pipeline);
        return false;
    }
    gdata->buffer = NULL;
    return true;
}


/*TODO Call...
static void vfio_encode_free_gstreamer(vfio_encode_gdata *gdata) {
    gst_element_set_state (gdata->pipeline, GST_STATE_NULL);
    gst_object_unref (gdata->pipeline);
    gst_buffer_unref(gdata->gdata->allocator );
}
*/

static void vfio_encode_early_init(DisplayOptions *opts)
{
    egl_init(opts->u.egl_headless.rendernode, true, &error_fatal);
}




static void vfio_encode_init(DisplayState *ds, DisplayOptions *opts)
{


    
    //DisplayGLMode mode = opts->has_gl ? opts->gl : DISPLAYGL_MODE_ON;
    QemuConsole *con;
    vfio_encode_dpy *vedpy;
    
    int idx;

 

    for (idx = 0;; idx++) {
        DisplayGLCtx *ctx;

        con = qemu_console_lookup_by_index(idx);
        if (!con || !qemu_console_is_graphic(con)) {
            break;
        }

        vedpy = g_new0(vfio_encode_dpy, 1);
        vedpy->dcl.con = con;
        vedpy->dcl.ops = &vegl_ops;
        vedpy->gls = qemu_gl_init_shader();
        ctx = g_new0(DisplayGLCtx, 1);
        ctx->ops = &veglctx_ops;
        qemu_console_set_display_gl_ctx(con, ctx);
        register_displaychangelistener(&vedpy->dcl);
    }
    if(!vfio_encode_setup_gstreamer(&vedpy->gdata)){
        /*TODO ERROR OUT*/
    }

    vedpy->primary_width = 1024;
    vedpy->primary_height = 768; 
     vfio_encode_set_gstreamer_params(vedpy);
}

static QemuDisplay qemu_display_vfio_encode = {
    .type       = DISPLAY_TYPE_VFIO_ENCODE,
    .early_init = vfio_encode_early_init,
    .init       = vfio_encode_init,
};

static void vfio_encode_register(void)
{
    qemu_display_register(&qemu_display_vfio_encode);
}

type_init(vfio_encode_register);

module_dep("gstreamer");