#include "qemu/osdep.h"
#include "qemu/module.h"
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
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

#define VFIO_ENCODE_USE_CPU  TRUE

typedef struct vfio_encode_gdata{
    GstElement *pipeline;
    GstAllocator *allocator;
    GstElement *source;
    GstElement *sink;
    GstElement *conv;
    GstElement *filter;
    GstBuffer* buffer;
 //   GstVaDisplay * display;
} vfio_encode_gdata;

typedef struct vfio_encode_dpy {
    DisplayChangeListener dcl;
    DisplaySurface *ds;
    egl_fb guest_fb;
    egl_fb cursor_fb;
    egl_fb blit_fb;
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

static void vfio_encode_refresh(DisplayChangeListener *dcl)
{
    graphic_hw_update(dcl->con);

}

static void vfio_encode_release_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{

}


static char * vfio_encode_convert_format_cpu(QemuDmaBuf *dmabuf) {

    struct dma_buf_sync sync = { 0 };
    sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START;
    int ret;
	ret = ioctl(dmabuf->fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (ret!= 0) {
        printf("DMA_BUF_SYNC_START failed: %s\n", strerror(errno));
		return 0;
    }

    char *mmap_adr = mmap(NULL, dmabuf->stride * dmabuf->height, PROT_READ, MAP_SHARED, dmabuf->fd, 0);
	if (mmap_adr == MAP_FAILED) {
		printf("mmap dmabuf failed: %s\n", strerror(errno));
		return 0;
	}
    //gstreamer will free it
    char* buffer = malloc(dmabuf->stride* dmabuf->height);
    // linear image
    if(dmabuf->modifier == 0){
        memcpy(buffer,mmap_adr, dmabuf->stride* dmabuf->height);
    //tiled image    
    } else {
        int num_tiles = (dmabuf->stride * dmabuf->height)/(512*8) ;
        for (int i = 0; i < num_tiles; i++) {
        // read Tile
            ssize_t tile_start = (i * 512) % dmabuf->stride;
            for(int j = 0; j < 7; j++ ) {
            
                memcpy(buffer+((i*8*512)+j*512), mmap_adr+(tile_start+(j*dmabuf->stride )), 512);
            }
        }
    }
    sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END;
	ret = ioctl( dmabuf->fd, DMA_BUF_IOCTL_SYNC, &sync);
    if (ret!= 0) {
        printf("DMA_BUF_SYNC_END failed: %s\n", strerror(errno));
		return 0;
    }

    munmap(mmap_adr,dmabuf->stride* dmabuf->height);
    return buffer;
}


/*
static char * vfio_encode_convert_format_cpu(QemuDmaBuf *dmabuf) {

    struct dma_buf_sync sync = { 0 };
    sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_START;
	ioctl(dmabuf->fd, DMA_BUF_IOCTL_SYNC, &sync);

    char *mmap_adr = mmap(NULL, dmabuf->stride* dmabuf->height, PROT_READ, MAP_SHARED, dmabuf->fd, 0);

    //struct dma_buf *dma_buf;
    //dma_buf = dma_buf_get(dmabuf-> fd)
   // dma_buf_mmap(dma_buf,, 0);
	if (mmap_adr == MAP_FAILED) {
		printf("mmap dmabuf failed: %s\n", strerror(errno));
		return 0;
	}

    //gstreamer will free it
    char* buffer = malloc(dmabuf->stride* dmabuf->height);

    // linear image
    if(dmabuf->modifier == 0){

        memcpy(buffer,mmap_adr, dmabuf->stride* dmabuf->height);
        
    //tiled image    
    } else {
        int num_tiles = (dmabuf->stride * dmabuf->height)/(512*8) ;
        for (int i = 0; i < num_tiles; i++) {
        // read Tile
            ssize_t tile_start = (i * 512) % dmabuf->stride;
            for(int j = 0; j < 7; j++ ) {
            
                memcpy(buffer+((i*8*512)+j*512), mmap_adr+(tile_start+(j*dmabuf->stride )), 512);
            }
        }
    }
    sync.flags = DMA_BUF_SYNC_READ | DMA_BUF_SYNC_END;
	ioctl( dmabuf->fd, DMA_BUF_IOCTL_SYNC, &sync);
    munmap(mmap_adr,dmabuf->stride* dmabuf->height);


    return buffer;
}*/











/*

static ssize_t read_till_end(int fd, char * buf, ssize_t count) {

    ssize_t bytes_read = 0;
    while ( bytes_read < count){
        ssize_t b = read (fd, buf+bytes_read, count-bytes_read);
        if(b < 1){
            return bytes_read;
        }
        bytes_read+=b;
    }
    return bytes_read;
}




static char * vfio_encode_convert_format_cpu(QemuDmaBuf *dmabuf) {

    
    //gstreamer fill free it
    char* buffer = malloc(dmabuf->stride* dmabuf->height);
    lseek(dmabuf->fd, 0, SEEK_SET);
    // linear image
    if(dmabuf->modifier == 0){
        ssize_t bytes_read = 0;

        while(bytes_read < dmabuf->stride* dmabuf->height){
            ssize_t bytes_to_read = (ssize_t) MIN(dmabuf->stride* dmabuf->height - bytes_read, 512);

            ssize_t tmp = read_till_end(dmabuf->fd, buffer+bytes_read,bytes_to_read);
            
            if(tmp < bytes_to_read) {
                free(buffer);
                return 0;
            }
            bytes_read += tmp;

        }
    //tiled image    
    } else {
        int num_tiles = (dmabuf->stride * dmabuf->height)/(512*8) ;
        for (int i = 0; i < num_tiles; i++) {
        // read Tile

            ssize_t tile_start = (i * 512) % dmabuf->stride;
            for(int j = 0; j < 7; j++ ) {
            
                ssize_t bytes_read = 0;
                bytes_read = read_till_end(dmabuf->fd, buffer+(tile_start+(j*dmabuf->stride )),512);
                if(bytes_read < 512){
                    printf("%zd\n", bytes_read);
                    free(buffer);
                    return 0;
                }
            }

        }
    }
    return buffer;
}
*/
static void vfio_encode_import_dma_buf_cpu(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
 {   
    char * data;
    GstFlowReturn ret;
    if ((data = vfio_encode_convert_format_cpu(dmabuf))==0){
        return;
    } 
    gsize data_size = dmabuf->stride * dmabuf->height;
    GstBuffer * buf = gst_buffer_new_wrapped_full (GST_MEMORY_FLAG_READONLY, data, data_size, 0,data_size,NULL, NULL);

    vfio_encode_dpy *vedpy = container_of(dcl, vfio_encode_dpy, dcl);
    g_signal_emit_by_name (vedpy->gdata.source, "push-buffer", buf, &ret);
    if (ret != GST_FLOW_OK) {
        GST_DEBUG ("some error");
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



static void vfio_encode_import_dma_buf_gstreamer(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{

    vfio_encode_dpy *vedpy = container_of(dcl, vfio_encode_dpy, dcl);
    if(vedpy->gdata.buffer!=NULL){
        gst_buffer_unref(vedpy->gdata.buffer);
        
    } else {
        ///memcpy(&vedpy->primary_width, &dmabuf->width, 5*sizeof(uint32_t));
        vedpy->primary_modifier = dmabuf->modifier;
        

    } 
   // if(memcmp(&vedpy->primary_width, &dmabuf->width, 5*sizeof(uint32_t))){
    if (vedpy->primary_modifier != dmabuf->modifier){
        printf("dma buf info changed \n");
        vedpy->primary_modifier = dmabuf->modifier;
        //TODO CHANGE PIPELINE
        //memcpy(&vedpy->primary_width, &dmabuf->width, 5*sizeof(uint32_t));

        //TODO CHECK DRM_I915_GEM_SET_TILING
    }
   // if( dmabuf->modifier == 0)  {
     
    //    return;
   // }


    /*

    GstMemory * mem = gst_va_allocator_alloc (GstAllocator * allocator)
    vedpy->gdata.buffer = gst_buffer_new();
    gsize offset[GST_VIDEO_MAX_PLANES] = {0, 0, 0, 0};
    uintptr_t fds[GST_VIDEO_MAX_PLANES] = {dmabuf->fd, 0, 0, 0};
    GstMemory * mems [GST_VIDEO_MAX_PLANES] = {mem,NULL,NULL,NULL};
    GstVideoInfo * info = gst_video_info_new ();
    gst_video_info_init (info);
    GST_VIDEO_INFO_HEIGHT(info) = dmabuf->height;
    GST_VIDEO_INFO_WIDTH(info) = dmabuf->width;
    GST_VIDEO_INFO_SIZE(info) = dmabuf->width * dmabuf->height * (dmabuf->stride/1024);
    GST_VIDEO_INFO_FORMAT(info) =GST_VIDEO_FORMAT_ENCODED;
    GST_VIDEO_INFO_PLANE_STRIDE(info,0)= dmabuf->stride;
    gboolean ok = gst_va_dmabuf_memories_setup (GstVaDisplay * display,
                              info,
                              1,
                              mems,
                              fds,
                              offset,
                              0);

    gst_buffer_append_memory(vedpy->gdata.buffer, mem );
    */
    
    GstMemory* mem = gst_dmabuf_allocator_alloc(vedpy->gdata.allocator, dmabuf->fd,  dmabuf->height * dmabuf->stride);
    vedpy->gdata.buffer = gst_buffer_new();
    gst_buffer_append_memory(vedpy->gdata.buffer, mem );


    gsize offset[GST_VIDEO_MAX_PLANES] = {0, 0, 0, 0};
    gint stride[GST_VIDEO_MAX_PLANES] = {dmabuf->stride, 0, 0, 0};
    gst_buffer_add_video_meta_full( vedpy->gdata.buffer, GST_VIDEO_FRAME_FLAG_NONE,
                                    //gst_video_format_from_fourcc(dmabuf->fourcc),
                                    //GST_VIDEO_FORMAT_BGRx,
                                    GST_VIDEO_FORMAT_ENCODED,
                                    dmabuf->width, dmabuf->height, 1, offset, stride);
    
    GST_BUFFER_FLAG_SET(vedpy->gdata.buffer,GST_BUFFER_FLAG_LIVE) ;
  


                       
}

static void vfio_encode_scanout_dmabuf(DisplayChangeListener *dcl,
                               QemuDmaBuf *dmabuf)
{
    if(VFIO_ENCODE_USE_CPU){
        vfio_encode_import_dma_buf_cpu(dcl, dmabuf);
    }
    else{
        vfio_encode_import_dma_buf_gstreamer(dcl, dmabuf);
    }
}

static void vfio_encode_cursor_dmabuf(DisplayChangeListener *dcl,
                              QemuDmaBuf *dmabuf, bool have_hot,
                              uint32_t hot_x, uint32_t hot_y)
{
    
}

static void vfio_encode_cursor_position(DisplayChangeListener *dcl,
                                uint32_t pos_x, uint32_t pos_y)
{
    vfio_encode_dpy *vedpy = container_of(dcl, vfio_encode_dpy, dcl);

    vedpy->pos_x = pos_x;
    vedpy->pos_y = pos_y;
}

static void vfio_encode_update(DisplayChangeListener *dcl,
                   uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    if(VFIO_ENCODE_USE_CPU){
        return;
    }
    GstFlowReturn ret;
    vfio_encode_dpy *vedpy = container_of(dcl, vfio_encode_dpy, dcl);
    if(vedpy->gdata.buffer==NULL){
        return;
    }
   

    //////////////////
   // vedpy->gdata.buffer->pts = timestamp;
    //timestamp += 33333333; // 16666667
   // GST_BUFFER_TIMESTAMP(vedpy->gdata.buffer) = GST_CLOCK_TIME_NONE;

/*/
    GstClock *clock = gst_element_get_clock (GST_ELEMENT ( vedpy->gdata.source));
    if(clock != NULL) {
        GST_BUFFER_PTS (vedpy->gdata.buffer) =
            GST_CLOCK_DIFF (gst_element_get_base_time (GST_ELEMENT ( vedpy->gdata.source)), gst_clock_get_time (clock));
        GST_BUFFER_DTS(vedpy->gdata.buffer) = 16666667;
        gst_object_unref (clock);
    } else{
        GST_BUFFER_PTS (vedpy->gdata.buffer) = 0;
    }
*/

    
    g_signal_emit_by_name (vedpy->gdata.source, "push-buffer", vedpy->gdata.buffer, &ret);
    if (ret != GST_FLOW_OK) {
        GST_DEBUG ("some error");
    }
    
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



    GstBus *bus;
    //GstCaps *filtercaps;
    //////////////////////

    
    char launch_stream[]  = 
    
    "appsrc name=source is-live=TRUE do-timestamp=TRUE format=time ! " // leaky-type=2 format=3 ! "
    "video/x-raw,width=1024,height=768,framerate=(fraction)30/1,format=BGRx! "//{BGRx,BGRx:0x0100000000000001} ! "

    //" video/x-raw(memory:DMABuf),width=1024,height=768,framerate=(fraction)30/1, drm-format=BGRx:0x0100000000000001 !"
    //" glupload ! glcolorconvert !  gldownload ! video/x-raw,format=NV12 !"
   //" videotestsrc !"
   //" video/x-raw(memory:DMABuf),format=(string)NV12 ! "
   //" video/x-raw(memory:DMABuf),width=1024,height=768,framerate=0/1,format=NV12 !"
   // "  vah264enc ! h264parse ! mp4mux ! filesink location=../test.mp4";
    //"  video/x-raw(memory:DMABuf), width=1024, height=768, interlace-mode=progressive  !"
    //" vaapisink "; 
   // " timeoverlay !"
    " vapostproc !"
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
    display_opengl = 1;
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
        //vedpy->gls = qemu_gl_init_shader();
        ctx = g_new0(DisplayGLCtx, 1);
        ctx->ops = &veglctx_ops;
        qemu_console_set_display_gl_ctx(con, ctx);
        register_displaychangelistener(&vedpy->dcl);
    }
    if(!vfio_encode_setup_gstreamer(&vedpy->gdata)){
        /*TODO ERROR OUT*/
    }
}

static QemuDisplay qemu_display_vfio_encode = {
    .type       = DISPLAY_TYPE_VFIO_ENCODE,
    .early_init = vfio_encode_early_init,
    .init       = vfio_encode_init,
};

static void register_vfio_encode(void)
{
    qemu_display_register(&qemu_display_vfio_encode);
}

type_init(register_vfio_encode);

module_dep("gstreamer");