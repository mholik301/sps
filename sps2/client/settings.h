#pragma once

namespace settings_shared
{
    /*
    choose the amount of debug messages printed in the console
    0-2
    */
    constexpr int DEBUG_LEVEL{ 0 };

    constexpr unsigned short PORT = 50013;
    const std::string PORT_STR{ std::to_string(PORT) };

    //1<<15 is maximum, 16+ returns 'Sending data failed with the following message: Message too long' on server
    constexpr size_t MAX_DATA_PACKET_SIZE_BYTES = ((1 << 14) - 1);

    /*
     choose whether we're expecting a JPEG encoded image or a raw one
     0 - raw
     1 - JPEG encoded
     */
     #define ENCODE_AS_JPEG 1

     /* choose the format we expect to receive from the server
     1 = raw8
     2 = raw16
     3 = rgb
     */
     #define INPUT_IMAGE_TYPE 1
}
using namespace settings_shared;



namespace settings_client
{
     /*
     choose whether the client opens a fixed, hardcoded port or any available port instead
     0 - any available port
     1 - fixed port
     */
    #define USE_FIXED_PORT 1

     /*
     choose whether you wish to debayer the image (resulting in a color image) or display as-is (grayscale)
     0 - don't debayer, display grayscale
     1 - debayer, display in color
     */
    #define DEBAYER_RECEIVED_IMAGE 1

     /*
     choose whether you wish to display when an image with a watermark in the top left apperas and disapperas
     0 - don't display
     1 - display
     */
     #define PRINT_SWAP 0

     /*
      choose whether you wish to display the performance stats in the following format:
        Loss rate=0.00% (0/997)
        Receive FPS=60.84 (997/16.39)
        Decode FPS=0.00 (0/16.39) (0.00% of received FPS, 0.00% of server FPS)
        Server fps=60.84 (997/16.39)
      0 - don't display
      1 - display
      */
    #define ACTIVE_DISPLAY_PERF_STATS 1

     /*
     choose whether you wish to display the image
     0 - don't display
     1 - display
     */
    #define DISPLAY_IMAGE 1

     /* choose the desired debayering paralelisation technique
     1 = no paralelisation, debayer the entire image all at once
     12 = no paralelisation, debayer the entire image all at once, but try to use the GPU
     2 = generic threading
     3 = BoostPool
     4 = TbbArena
     */
    #define DEBAYER_METHOD 1

     /* choose whether the debayer image should have the green channel reduced and brightness increased (useful when sending custom images)
      0 = don't do color correction
      1 = do color correction
      */
      #define DO_COLOR_CORRECTION 0

    const std::string HOST_IP{ "192.168.1.105" };   // edit to match the server's IP address
}
using namespace settings_client;
