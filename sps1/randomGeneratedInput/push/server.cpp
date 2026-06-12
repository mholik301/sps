

/*
 * g++ -o test-launch  testB8.cpp `pkg-config --cflags --libs opencv4 gstreamer-1.0 gstreamer-app-1.0 gstreamer-rtsp-server-1.0 gstreamer-plugins-base-1.0 gstreamer-rtp-1.0 gstreamer-video-1.0`
 * GST_DEBUG=4 ./test-launch
 * ./test-launch
 * */

#include <iostream>
#include <gst/gst.h>

#include <gst/rtsp-server/rtsp-server.h>
#include <gst/app/gstappsrc.h>

#include <opencv2/opencv.hpp>
#include <opencv2/core/core.hpp>
#include "opencv2/highgui/highgui_c.h"


#include <unistd.h>
#include <thread>
#include <mutex>


/// set basic params
//note: lower values can behave weirdly depending on the version; 100 works nice and smooth
#define FPS 50
#define HEIGHT 1440
#define WIDTH 2560

#define MAIN_ITERATION_LEN 20   //s


/// set advanced params
// comment to load gstreamer buffer with 'memcpy', uncomment to load with gst_buffer_new_memdup
#define read_V2 1

// generate random colors or white noise (uncomment only one)
#define GEN_COLOR
//#define GEN_RAND


/// don't touch
#define SIZE (HEIGHT*WIDTH*3+HEIGHT)
#define MS_TO_US 1000
constexpr int CALLBACK_PERIOD = 1000/FPS;   //ms

// struct used for setting image timings
typedef struct
{
    gboolean white;
    GstClockTime timestamp;
} MyContext;


/// GLOBALS
GstRTSPServer *server;
GstElement *appsrc;

bool lastCtx = FALSE;

int incr = 0;

static gboolean timeout (MyContext * ctx, IplImage* pushImg)
{
    GstBuffer *buffer;
    GstFlowReturn ret;
    GstMapInfo map;
    GstClock *clock;
    GstClockTime running_time;
    gint64 timestamp;

    // fetch data
    IplImage* iplImg = pushImg;
    cv::Mat img = cv::cvarrToMat(iplImg);  // default additional arguments: don't copy data.
    cv::cvtColor(img, img, CV_BGR2RGB);
    guint8* data = img.data;

    /* create a new buffer */
#ifndef read_V2
    buffer = gst_buffer_new_allocate (NULL, SIZE, NULL);

    /* fill the buffer with some image data */
    gst_buffer_map (buffer, &map, GST_MAP_WRITE);
    memcpy(map.data, data, SIZE);
    gst_buffer_unmap (buffer, &map);
#else
    buffer = gst_buffer_new_memdup(data, SIZE);
#endif

    /* increment the timestamp */
    GST_BUFFER_PTS (buffer) = ctx->timestamp;
    GST_BUFFER_DURATION (buffer) = gst_util_uint64_scale_int (1, GST_SECOND, FPS);
    ctx->timestamp += GST_BUFFER_DURATION (buffer);


    /* push the buffer into the pipeline */
    ret = gst_app_src_push_buffer (GST_APP_SRC (appsrc), buffer);
    if (ret != GST_FLOW_OK) {
        /* something wrong, stop sending data */
        return FALSE;
    }

    return TRUE;
}


/* called when a new media pipeline is constructed. We can query the pipeline and configure our appsrc */
static void media_configure (GstRTSPMediaFactory * factory, GstRTSPMedia * media, gpointer user_data)
{
    GstElement *element;
    MyContext *ctx;

    /* get the element used for providing the streams of the media */
    element = gst_rtsp_media_get_element (media);

    /* get our appsrc, we named it 'mysrc' with the name property */
    appsrc = gst_bin_get_by_name_recurse_up (GST_BIN (element), "mysrc");

    /* this instructs appsrc that we will be dealing with timed buffer */
    gst_util_set_object_arg (G_OBJECT (appsrc), "format", "time");
    /* configure the caps of the video */
    g_object_set (G_OBJECT (appsrc), "caps",
                  gst_caps_new_simple ("video/x-raw",
                                       "format", G_TYPE_STRING, "RGB16",
                                       "width", G_TYPE_INT, WIDTH,
                                       "height", G_TYPE_INT, HEIGHT,
                                       "framerate", GST_TYPE_FRACTION, FPS, NULL), NULL);

    ctx = g_new0 (MyContext, 1);
    ctx->white = FALSE;
    ctx->timestamp = 0;
    /* make sure ther datais freed when the media is gone */
    g_object_set_data_full (G_OBJECT (media), "my-extra-data", ctx,
                            (GDestroyNotify) g_free);


    /// new
    /* set the "is-live" property on the "appsrc" element */
    g_object_set(G_OBJECT(appsrc), "is-live", TRUE, NULL);

    /* set the min-latency and max-latency to minimize the delay */
    g_object_set(G_OBJECT(appsrc), "min-latency", GST_SECOND/120, NULL);
    g_object_set(G_OBJECT(appsrc), "max-latency", GST_SECOND/120, NULL);

    /* add a timeout for the appsrc */
    //g_timeout_add (CALLBACK_PERIOD, (GSourceFunc) timeout, ctx);


    /* set the pipeline to playing */
    //gst_element_set_state (element, GST_STATE_PLAYING);
    //note: this line causes the "gst_element_set_state: assertion 'GST_IS_ELEMENT (element)' failed" error when
    // a client connects. The stream still works however

    std::cout << "media_configure done" << std::endl;

    gst_object_unref (appsrc);
    gst_object_unref (element);
}

static void* gstreamer_server(void* notused)
{
    GMainLoop *loop;
    GstRTSPMountPoints *mounts;
    GstRTSPMediaFactory *factory;

    loop = g_main_loop_new (NULL, FALSE);

    /* create a server instance */
    server = gst_rtsp_server_new ();
    gst_rtsp_server_set_address (server, (const gchar *)"161.53.67.188");

    /* get the mount points for this server, every server has a default object
     * that be used to map uri mount points to media factories */
    mounts = gst_rtsp_server_get_mount_points (server);

    /* make a media factory for a test stream. The default media factory can use
     * gst-launch syntax to create pipelines.
     * any launch line works as long as it contains elements named pay%d. Each
     * element with pay%d names will be a stream */
    factory = gst_rtsp_media_factory_new ();
    gst_rtsp_media_factory_set_launch (factory,
                                       "( appsrc name=mysrc is-live=true ! videoconvert ! video/x-raw,format=(string)I420 ! "
                                       "timeoverlay valignment=4 halignment=1 ! "
                                       "x264enc speed-preset=ultrafast tune=zerolatency ! "
                                       "rtph264pay config-interval=1 name=pay0 pt=96 )");

    /*gst_rtsp_media_factory_set_launch (factory,
                                       "( appsrc name=mysrc is-live=true ! videoconvert ! video/x-raw,format=(string)I420 ! "
                                       "timeoverlay valignment=4 halignment=1 ! "
                                       "rtpvrawpay name=pay0 pt=96 )");*/

    /* notify when our media is ready, This is called whenever someone asks for
     * the media and a new pipeline with our appsrc is created */
    g_signal_connect (factory, "media-configure", (GCallback) media_configure,
                      NULL);

    /* attach the test factory to the /test url */
    gst_rtsp_mount_points_add_factory (mounts, "/test", factory);

    /* don't need the ref to the mounts anymore */
    g_object_unref (mounts);


    /* attach the server to the default maincontext */
    gst_rtsp_server_attach (server, NULL);

    /* start serving */
    g_print ("stream ready at rtsp://161.53.67.188:8554/test\n");
    g_main_loop_run (loop);

    return NULL;
}

IplImage* random_image(int width, int height, int type)
{
    IplImage* img = cvCreateImage(cvSize(width, height), IPL_DEPTH_8U, 3);
    for (int y = 0; y < img->height; y++)
        for (int x = 0; x < img->width; x++)
            for (int c = 0; c < 3; c++)
                if (type==-1)
                {
                    //GEN_RAND
                    img->imageData[y * img->widthStep + x * 3 + c] = rand() % 255;
                }
                else
                {
                    img->imageData[y * img->widthStep + x * 3 + c] = type;
                }

    return img;
}

int
main (int argc, char *argv[])
{
    gst_init (&argc, &argv);

    constexpr int len = 50;

    // start server
    GThread* server_thread = g_thread_new("server", gstreamer_server, NULL);

    MyContext *ctx;
    ctx = g_new0 (MyContext, 1);
    ctx->white = FALSE;
    ctx->timestamp = 0;

    while (1)
    {
        for(int i=0; i<len;i++)
        {
            // push image to buffer
            timeout(ctx, random_image(WIDTH, HEIGHT, i));   //rainbow
            //timeout(ctx, random_image(WIDTH, HEIGHT, -1));   //random

            // sleep for 1000/FPS ms
            usleep(CALLBACK_PERIOD*MS_TO_US);
        }
    }

    // cleanup
    g_thread_join(server_thread);

    return 0;
}
