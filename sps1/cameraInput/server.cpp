
//zwo includes
#include "stdio.h"
#include "opencv2/highgui/highgui_c.h"
#include "pthread.h"
#include "ASICamera2.h"
#include <sys/time.h>
#include <time.h>


//my includes

#include <iostream>
#include <gst/gst.h>

#include <gst/rtsp-server/rtsp-server.h>
#include <gst/app/gstappsrc.h>

#include <opencv2/opencv.hpp>
#include <opencv2/core/core.hpp>

#include <unistd.h>
#include <thread>
#include <mutex>



//zwo defines
#define  MAX_CONTROL 7


//my defines


#define STREAM_DONTDRAW 1


/// set basic params
//note: lower values can behave weirdly depending on the version; 100 works nice and smooth
#define FPS 20

//max: 5496 X 3672
//works1: 1024 X 720 and 2560 X 1440 also work, while 1920 x 1080 doesn't

//4K: 3840 X 2160
#define HEIGHT 2160
#define WIDTH 3840

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


//my GLOBALS
GstRTSPServer *server;
GstElement *appsrc;
GAsyncQueue *queue;

int incr = 0;
int sharedImgType;

extern unsigned long GetTickCount();


//zwo GLOBALS
int bDisplay = 0;
int bMain = 1;
int bChangeFormat = 0;
int bSendTiggerSignal = 0;
ASI_CAMERA_INFO CamInfo;

enum CHANGE{
    change_imagetype = 0,
    change_bin,
    change_size_bigger,
    change_size_smaller
};

CHANGE change;


//zwo FUNCTIONS
void cvText(IplImage* img, const char* text, int x, int y)
{
    CvFont font;

    double hscale = 0.6;
    double vscale = 0.6;
    int linewidth = 2;
    cvInitFont(&font,CV_FONT_HERSHEY_SIMPLEX | CV_FONT_ITALIC,hscale,vscale,0,linewidth);

    CvScalar textColor =cvScalar(255,255,255);
    CvPoint textPos =cvPoint(x, y);

    cvPutText(img, text, textPos, &font,textColor);
}

void* Display(void* params)
{
    IplImage *pImg = (IplImage *)params;
    cvNamedWindow("video", 1);
    while(bDisplay)
    {
        // show image on screen
        cvShowImage("video", pImg);

        // push image to buffer
        //g_async_queue_push(queue, pImg);

        /*
         * Commands:
         * esc - stop
         * i - change image type
         * b - change bin
         * w/s - make image smaller or bigger
         * t - trigger
         * */

        char c=cvWaitKey(1);
        switch(c)
        {
            case 27://esc
                bDisplay = false;
                bMain = false;
                goto END;

            case 'i'://space
                bChangeFormat = true;
                change = change_imagetype;
                break;

            case 'b'://space
                bChangeFormat = true;
                change = change_bin;
                break;

            case 'w'://space
                bChangeFormat = true;
                change = change_size_smaller;
                break;

            case 's'://space
                bChangeFormat = true;
                change = change_size_bigger;
                break;

            case 't'://triiger
                bSendTiggerSignal = true;
                break;
        }
    }
    END:
    cvDestroyWindow("video");
    printf("Display thread over\n");
    ASIStopVideoCapture(CamInfo.CameraID);
    return (void*)0;
}


//my FUNCTIONS

// Pushes IplImage* data stored in the global GAsyncQueue *queue to a global GstElement *appsrc;
static gboolean timeout (MyContext * ctx)
{
    GstBuffer *buffer;
    GstFlowReturn ret;
    GstMapInfo map;
    GstClock *clock;
    GstClockTime running_time;
    gint64 timestamp;

    // fetch data
    IplImage* iplImg = (IplImage*)g_async_queue_pop(queue);
    cv::Mat img = cv::cvarrToMat(iplImg);  // default additional arguments: don't copy data.
    //cv::cvtColor(img, img, CV_BGR2RGB);   //note: gstreamer knows how to deal with rgb24, raw8 and raw16. This just messes it up
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

    switch (sharedImgType) {
        case 0:
            g_object_set (G_OBJECT (appsrc), "caps",
                          gst_caps_new_simple ("video/x-raw",
                                               "format", G_TYPE_STRING, "GRAY8",
                                               "width", G_TYPE_INT, WIDTH,
                                               "height", G_TYPE_INT, HEIGHT,
                                               "framerate", GST_TYPE_FRACTION, FPS, 1, NULL), NULL);
            break;
        case 1:
            g_object_set (G_OBJECT (appsrc), "caps",
                          gst_caps_new_simple ("video/x-raw",
                                               "format", G_TYPE_STRING, "RGB",
                                               "width", G_TYPE_INT, WIDTH,
                                               "height", G_TYPE_INT, HEIGHT,
                                               "framerate", GST_TYPE_FRACTION, FPS, 1, NULL), NULL);
            break;
        case 2:
            g_object_set (G_OBJECT (appsrc), "caps",
                          gst_caps_new_simple ("video/x-raw",
                                               "format", G_TYPE_STRING, "GRAY16_LE",
                                               "width", G_TYPE_INT, WIDTH,
                                               "height", G_TYPE_INT, HEIGHT,
                                               "framerate", GST_TYPE_FRACTION, FPS, 1, NULL), NULL);
            break;
        default:
            g_object_set (G_OBJECT (appsrc), "caps",
                          gst_caps_new_simple ("video/x-raw",
                                               "format", G_TYPE_STRING, "RGB",
                                               "width", G_TYPE_INT, WIDTH,
                                               "height", G_TYPE_INT, HEIGHT,
                                               "framerate", GST_TYPE_FRACTION, FPS, 1, NULL), NULL);
            break;
    }

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
    g_timeout_add (CALLBACK_PERIOD, (GSourceFunc) timeout, ctx);


    /* set the pipeline to playing */
    //gst_element_set_state (element, GST_STATE_PLAYING);
    //note: this line causes the "gst_element_set_state: assertion 'GST_IS_ELEMENT (element)' failed" error when
    // a client connects. The stream still works so it's more like a warning

    g_print ("media_configure done\n");

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
    gst_rtsp_server_set_address (server, (const gchar *)"192.168.1.105");

    /* get the mount points for this server, every server has a default object
     * that be used to map uri mount points to media factories */
    mounts = gst_rtsp_server_get_mount_points (server);

    /* make a media factory for a test stream. The default media factory can use
     * gst-launch syntax to create pipelines.
     * any launch line works as long as it contains elements named pay%d. Each
     * element with pay%d names will be a stream */
    factory = gst_rtsp_media_factory_new ();
    //gst_rtsp_media_factory_set_transport_mode(factory, GST_RTSP_TRANSPORT_MODE_UDP);
    gst_rtsp_media_factory_set_launch (factory,
                                       "( appsrc name=mysrc is-live=true ! videoconvert ! video/x-raw,format=(string)I420 ! "
                                       "timeoverlay valignment=4 halignment=1 ! "
                                       "x264enc speed-preset=ultrafast tune=zerolatency ! "
                                       "rtph264pay config-interval=1 name=pay0 pt=96 )");

	//bframes=0 key-int-max=1  potgraju gledanje (slika postane siva nakon jedne sekunde)

    /*
     * time-mode=
     * buffer-time
     * time-code
     * reference-timestamp
     * stream-time
     * running-time
     * elapse-running-time
     *
     * maybe I misunderstood something?
     * */

    /*
     * "clockoverlay valignment=4 halignment=1 ! "
     * great and all but it can't show bellow one second
     * */

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
    g_print ("stream ready at rtsp://192.168.1.105:8554/test\n");
    g_main_loop_run (loop);

    return NULL;
}



int  main(int argc, char *argv[])
{
    int width;
    char* bayer[] = {"RG","BG","GR","GB"};
    char* controls[MAX_CONTROL] = {"Exposure", "Gain", "Gamma", "WB_R", "WB_B", "Brightness", "USB Traffic"};

    int height;
    int i;
    char c;
    bool bresult;
    int modeIndex;

    int time1,time2;
    int count=0;

    char buf[128]={0};

    int CamIndex=0;
    int inputformat;
    int definedformat;


    IplImage *pRgb;


    int numDevices = ASIGetNumOfConnectedCameras();
    if(numDevices <= 0)
    {
        printf("no camera connected, press any key to exit\n");
        getchar();
        return -1;
    }
    else
        printf("attached cameras:\n");

    for(i = 0; i < numDevices; i++)
    {
        ASIGetCameraProperty(&CamInfo, i);
        printf("%d %s\n",i, CamInfo.Name);
    }

    printf("\nselect one to privew\n");
    scanf("%d", &CamIndex);


    ASIGetCameraProperty(&CamInfo, CamIndex);
    bresult = ASIOpenCamera(CamInfo.CameraID);
    bresult += ASIInitCamera(CamInfo.CameraID);
    if(bresult)
    {
        printf("OpenCamera error,are you root?,press any key to exit\n");
        getchar();
        return -1;
    }

    printf("%s information\n",CamInfo.Name);
    int iMaxWidth, iMaxHeight;
    iMaxWidth = CamInfo.MaxWidth;
    iMaxHeight =  CamInfo.MaxHeight;
    printf("resolution:%dX%d\n", iMaxWidth, iMaxHeight);
    if(CamInfo.IsColorCam)
        printf("Color Camera: bayer pattern:%s\n",bayer[CamInfo.BayerPattern]);
    else
        printf("Mono camera\n");

    int ctrlnum;
    ASIGetNumOfControls(CamInfo.CameraID, &ctrlnum);
    ASI_CONTROL_CAPS ctrlcap;
    for( i = 0; i < ctrlnum; i++)
    {
        ASIGetControlCaps(CamInfo.CameraID, i,&ctrlcap);

        printf("%s\n", ctrlcap.Name);

    }

    int bin = 1, Image_type;
    printf("Use customer format or predefined format resolution?\n 0:customer format \n 1:predefined format\n");
    scanf("%d", &inputformat);
    if(inputformat)
    {
        printf("0:Size %d X %d, BIN 1, ImgType raw8 (note: crashes)\n", iMaxWidth/2, iMaxHeight/2);
        printf("1:Size %d X %d, BIN 1, ImgType raw16 (note: drops all frames)\n", iMaxWidth/2, iMaxHeight/2);
        printf("2:Size 3840 X 2160, BIN 1, ImgType RGB24 (42 fps @10ms)\n");
        printf("3:Size 2560 X 1440, BIN 1, ImgType RGB24 (61 fps @10ms)\n");
        printf("4:Size 1920 X 1080, BIN 1, ImgType RGB24 (71 fps @10ms)\n");
        printf("5:Size 1024 X 720, BIN 1, ImgType raw16 (100 fps @10ms)\n");
        printf("6:Size 1024 X 720, BIN 2, ImgType RGB24 (50 fps @10ms)\n");
        scanf("%d", &definedformat);
        if(definedformat == 0)
        {
            ASISetROIFormat(CamInfo.CameraID, iMaxWidth/2, iMaxHeight/2, 1, ASI_IMG_RAW8);
            width = iMaxWidth/2;
            height = iMaxHeight/2;
            bin = 1;
            Image_type = ASI_IMG_RAW8;
        }
        else if(definedformat == 1)
        {
            ASISetROIFormat(CamInfo.CameraID, iMaxWidth/2, iMaxHeight/2, 1, ASI_IMG_RAW16);
            width = iMaxWidth/2;
            height = iMaxHeight/2;
            bin = 1;
            Image_type = ASI_IMG_RAW16;
        }
        else if(definedformat == 2)
        {
            ASISetROIFormat(CamInfo.CameraID, 3840, 2160, 1, ASI_IMG_RGB24);
            width = 3840;
            height = 2160;
            bin = 1;
            Image_type = ASI_IMG_RGB24;
        }
        else if(definedformat == 3)
        {
            ASISetROIFormat(CamInfo.CameraID, 2560, 1440, 1, ASI_IMG_RGB24);
            width = 2560;
            height = 1440;
            bin = 1;
            Image_type = ASI_IMG_RGB24;
        }
        else if(definedformat == 4)
        {
            ASISetROIFormat(CamInfo.CameraID, 1920, 1080, 1, ASI_IMG_RGB24);
            width = 1920;
            height = 1080;
            bin = 1;
            Image_type = ASI_IMG_RGB24;
        }
        else if(definedformat == 5)
        {
            ASISetROIFormat(CamInfo.CameraID, 1024, 720, 1, ASI_IMG_RAW16);
            width = 1024;
            height = 720;
            bin = 1;
            Image_type = ASI_IMG_RAW16;
        }
        else if(definedformat == 6)
        {
            ASISetROIFormat(CamInfo.CameraID, 1024, 720, 2, ASI_IMG_RGB24);
            width = 1024;
            height = 720;
            bin = 2;
            Image_type = ASI_IMG_RGB24;

        }
        else
        {
            printf("Wrong input! Will use the resolution0 as default.\n");
            ASISetROIFormat(CamInfo.CameraID, iMaxWidth, iMaxHeight, 1, ASI_IMG_RAW8);
            width = iMaxWidth;
            height = iMaxHeight;
            bin = 1;
            Image_type = ASI_IMG_RAW8;
        }

    }
    else
    {
        printf("\nPlease input the <width height bin image_type> with one space\n"
               "supported bin modes are '1' (bin1) and '2' (bin2)\n"
               "supported image types are %d (ASI_IMG_RAW8), %d (ASI_IMG_RGB24) and %d (ASI_IMG_RAW16)\n"
               "ie. 1024 720 1 1 10ms (100 fps), 1024 720 2 1 10ms (50 fps), 1920 1080 1 1 10 ms - 71 fps\n"
               "use max resolution (5496 X 3672) if input is 0\n"
               "Press ESC when video window is focused to quit capture\n", ASI_IMG_RAW8, ASI_IMG_RGB24, ASI_IMG_RAW16);
        scanf("%d %d %d %d", &width, &height, &bin, &Image_type);
        if(width == 0 || height == 0)
        {
            width = iMaxWidth;
            height = iMaxHeight;
        }


        while(ASISetROIFormat(CamInfo.CameraID, width, height, bin, (ASI_IMG_TYPE)Image_type))//IMG_RAW8
        {
            printf("Set format error, please check the width and height\n ASI120's data size(width*height) must be integer multiple of 1024\n");
            printf("Please input the width and height again, ie. 640 480\n");
            scanf("%d %d %d %d", &width, &height, &bin, &Image_type);
        }
        printf("\nset image format %d %d %d %d success, start privew, press ESC to stop \n", width, height, bin, Image_type);
    }

    if(Image_type == ASI_IMG_RAW16)
        pRgb=cvCreateImage(cvSize(width,height), IPL_DEPTH_16U, 1);
    else if(Image_type == ASI_IMG_RGB24)
        pRgb=cvCreateImage(cvSize(width,height), IPL_DEPTH_8U, 3);
    else
        pRgb=cvCreateImage(cvSize(width,height), IPL_DEPTH_8U, 1);

    sharedImgType = Image_type;

    int exp_ms;
    printf("Please input exposure time(ms):\n"
           "1 ms - sunny windows\n"
           "10 ms - indoor lighting (recommended)\n"
           "50 ms - big pinhole\n"
           "300 ms - small pinhole\n");
    scanf("%d", &exp_ms);


    int piNumberOfControls = 0;
    ASI_ERROR_CODE errCode = ASIGetNumOfControls(CamInfo.CameraID, &piNumberOfControls);

    ASI_CONTROL_CAPS pCtrlCaps;
    int transferSpeed = -1;
    for(int j = 0; j < piNumberOfControls; j++)
    {
        ASIGetControlCaps(CamInfo.CameraID, j, &pCtrlCaps);
        if (pCtrlCaps.ControlType == ASI_BANDWIDTHOVERLOAD)
        {
            printf("setting ASI_BANDWIDTHOVERLOAD to pCtrlCaps.MaxValue=%ld\n", pCtrlCaps.MaxValue);
            transferSpeed = pCtrlCaps.MaxValue;
            break;
        }
    }

    if (transferSpeed == -1)
    {
        printf("Couldn't find ASI_BANDWIDTHOVERLOAD MaxValue. Setting to 40\n");
        transferSpeed = 40; //default
    }

    ASISetControlValue(CamInfo.CameraID,ASI_EXPOSURE, exp_ms*1000, ASI_FALSE);
    ASISetControlValue(CamInfo.CameraID,ASI_GAIN,0, ASI_FALSE);
    ASISetControlValue(CamInfo.CameraID,ASI_BANDWIDTHOVERLOAD, transferSpeed, ASI_FALSE); //low transfer speed
    ASISetControlValue(CamInfo.CameraID,ASI_HIGH_SPEED_MODE, 1, ASI_FALSE);
    ASISetControlValue(CamInfo.CameraID,ASI_WB_B, 90, ASI_FALSE);
    ASISetControlValue(CamInfo.CameraID,ASI_WB_R, 48, ASI_TRUE);

    ASI_SUPPORTED_MODE cammode;
    ASI_CAMERA_MODE mode;
    if(CamInfo.IsTriggerCam)
    {
        i = 0;
        printf("This is multi mode camera, you need to select the camera mode:\n");
        ASIGetCameraSupportMode(CamInfo.CameraID, &cammode);
        while(cammode.SupportedCameraMode[i]!= ASI_MODE_END)
        {
            if(cammode.SupportedCameraMode[i]==ASI_MODE_NORMAL)
                printf("%d:Normal Mode\n", i);
            if(cammode.SupportedCameraMode[i]==ASI_MODE_TRIG_SOFT_EDGE)
                printf("%d:Trigger Soft Edge Mode\n", i);
            if(cammode.SupportedCameraMode[i]==ASI_MODE_TRIG_RISE_EDGE)
                printf("%d:Trigger Rise Edge Mode\n", i);
            if(cammode.SupportedCameraMode[i]==ASI_MODE_TRIG_FALL_EDGE)
                printf("%d:Trigger Fall Edge Mode\n", i);
            if(cammode.SupportedCameraMode[i]==ASI_MODE_TRIG_SOFT_LEVEL)
                printf("%d:Trigger Soft Level Mode\n", i);
            if(cammode.SupportedCameraMode[i]==ASI_MODE_TRIG_HIGH_LEVEL)
                printf("%d:Trigger High Level Mode\n", i);
            if(cammode.SupportedCameraMode[i]==ASI_MODE_TRIG_LOW_LEVEL)
                printf("%d:Trigger Low  Lovel Mode\n", i);

            i++;
        }

        scanf("%d", &modeIndex);
        ASISetCameraMode(CamInfo.CameraID, cammode.SupportedCameraMode[modeIndex]);
        ASIGetCameraMode(CamInfo.CameraID, &mode);
        if(mode != cammode.SupportedCameraMode[modeIndex])
            printf("Set mode failed!\n");

    }




    // ///////////////// gstreamer stuff start /////////////////

#ifdef STREAM_DONTDRAW
    gst_init (&argc, &argv);

    // init thread safe message queue
    queue = g_async_queue_new();

    // start server
    GThread* server_thread = g_thread_new("server", gstreamer_server, NULL);

    //pthread_t thread_server;
    //pthread_create(&thread_server, NULL, gstreamer_server, (void*)pRgb);
#endif

    // ///////////////// gstreamer stuff end   /////////////////




    ASIStartVideoCapture(CamInfo.CameraID); //start privew

    printf("controls:\n"
           "         esc - stop\n"
           "         i - change image type\n"
           "         b - change bin\n"
           "         w/s - make image smaller or bigger\n"
           "         t - trigger\n");

    long lVal;
    ASI_BOOL bAuto;
    ASIGetControlValue(CamInfo.CameraID, ASI_TEMPERATURE, &lVal, &bAuto);
    printf("sensor temperature:%.1f\n", lVal/10.0);

#ifndef STREAM_DONTDRAW
    bDisplay = 1;
#ifdef _LIN
    pthread_t thread_display;
	pthread_create(&thread_display, NULL, Display, (void*)pRgb);
#elif defined _WINDOWS
    HANDLE thread_setgainexp;
	thread_setgainexp = (HANDLE)_beginthread(Display,  NULL, (void*)pRgb);
#endif
#endif

    time1 = GetTickCount();
    int iStrLen = 0, iTextX = 40, iTextY = 60;
    void* retval;


    int iDropFrmae;
    while(bMain)
    {

        if(mode == ASI_MODE_NORMAL)
        {
            if(ASIGetVideoData(CamInfo.CameraID, (unsigned char*)pRgb->imageData, pRgb->imageSize, 500) == ASI_SUCCESS)
                count++;
        }
        else
        {
            if(ASIGetVideoData(CamInfo.CameraID, (unsigned char*)pRgb->imageData, pRgb->imageSize, 1000) == ASI_SUCCESS)
                count++;
        }

#ifdef STREAM_DONTDRAW
        // push image to buffer
        g_async_queue_push(queue, pRgb);
#endif



        time2 = GetTickCount();



        if(time2-time1 > 1000 )
        {
            ASIGetDroppedFrames(CamInfo.CameraID, &iDropFrmae);
            sprintf(buf, "fps:%d dropped frames:%d ImageType:%d",count, iDropFrmae, (int)Image_type);

            count = 0;
            time1=GetTickCount();
            printf(buf);
            printf("\n");

        }
        if(Image_type != ASI_IMG_RGB24 && Image_type != ASI_IMG_RAW16)
        {
            iStrLen = strlen(buf);
            CvRect rect = cvRect(iTextX, iTextY - 15, iStrLen* 11, 20);
            cvSetImageROI(pRgb , rect);
            cvSet(pRgb, cvScalar(180, 180, 180));
            cvResetImageROI(pRgb);
        }
        cvText(pRgb, buf, iTextX,iTextY );

        if(bSendTiggerSignal)
        {
            ASISendSoftTrigger(CamInfo.CameraID, ASI_TRUE);
            bSendTiggerSignal = 0;
        }

        if(bChangeFormat)
        {
            bChangeFormat = 0;
#ifndef STREAM_DONTDRAW
            bDisplay = false;
            pthread_join(thread_display, &retval);
#endif
            cvReleaseImage(&pRgb);
            ASIStopVideoCapture(CamInfo.CameraID);

            switch(change)
            {
                case change_imagetype:
                    Image_type++;
                    if(Image_type > 3)
                        Image_type = 0;

                    break;
                case change_bin:
                    if(bin == 1)
                    {
                        bin = 2;
                        width/=2;
                        height/=2;
                    }
                    else
                    {
                        bin = 1;
                        width*=2;
                        height*=2;
                    }
                    break;
                case change_size_smaller:
                    if(width > 320 && height > 240)
                    {
                        width/= 2;
                        height/= 2;
                    }
                    break;

                case change_size_bigger:

                    if(width*2*bin <= iMaxWidth && height*2*bin <= iMaxHeight)
                    {
                        width*= 2;
                        height*= 2;
                    }
                    break;
            }
            ASISetROIFormat(CamInfo.CameraID, width, height, bin, (ASI_IMG_TYPE)Image_type);
            if(Image_type == ASI_IMG_RAW16)
                pRgb=cvCreateImage(cvSize(width,height), IPL_DEPTH_16U, 1);
            else if(Image_type == ASI_IMG_RGB24)
                pRgb=cvCreateImage(cvSize(width,height), IPL_DEPTH_8U, 3);
            else
                pRgb=cvCreateImage(cvSize(width,height), IPL_DEPTH_8U, 1);
#ifndef STREAM_DONTDRAW
            bDisplay = 1;
            pthread_create(&thread_display, NULL, Display, (void*)pRgb);
#endif
            ASIStartVideoCapture(CamInfo.CameraID); //start privew
        }
    }
    END:

#ifdef STREAM_DONTDRAW
    // gstreamer
    g_thread_join(server_thread);
    //pthread_join(thread_server, &retval);
#endif


#ifndef STREAM_DONTDRAW
    if(bDisplay)
    {
        bDisplay = 0;
#ifdef _LIN
        pthread_join(thread_display, &retval);


#elif defined _WINDOWS
        Sleep(50);
#endif
    }
#endif

    ASIStopVideoCapture(CamInfo.CameraID);
    ASICloseCamera(CamInfo.CameraID);
    cvReleaseImage(&pRgb);
    printf("main function over\n");
    return 1;
}
