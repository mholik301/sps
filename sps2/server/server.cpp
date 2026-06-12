
#include <cstdio>
#include "opencv2/highgui/highgui_c.h"
#include "ASICamera2.h"
#include <sys/time.h>
#include <ctime>
#include <random>

#include <iostream>
#include <exception>

#include <chrono>
#include <thread>

#include <vector>
#include <array>
#include <string>

#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/filesystem.hpp>

#include <opencv2/opencv.hpp>
#include "opencv2/core.hpp"
#include <opencv2/core/core.hpp>
#include "opencv2/imgcodecs.hpp"
#include "opencv2/highgui.hpp"
#include "opencv2/imgproc.hpp"

#include <gst/gst.h>


//zwo defines
extern unsigned long GetTickCount();

constexpr const char* bayer[] = { "RG", "BG", "GR", "GB" };
int bDisplay{0};
int bServer{1};
int bMain {1};
ASI_CAMERA_INFO CamInfo;



//my defines
namespace ba = boost::asio;
namespace bs = boost::system;
namespace ip = boost::asio::ip;
using ba::ip::udp;

namespace definitions
{
    struct PacketHeader {
        uint32_t s_sequenceNumber;
        uint32_t s_payloadSize;
        uint32_t s_totalPackets;
        uint32_t s_totalSizeBytes;
        uint32_t s_imageNumber;
        bool s_FIN;
    };

    struct Packet {
        PacketHeader s_header;
        unsigned char s_payload[1];
    };
}
using namespace definitions;

namespace settings_shared
{
    /*
    choose the amount of debug messages printed in the console
    0-2
    */
    constexpr int DEBUG_LEVEL{ 0 };

    constexpr unsigned short PORT = 50013;

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

namespace settings_server
{
    /*
    choose whether the server will send images continually, as fast as it can, or ony by one, waiting for user input to send the next
    */
    #define CONTINUOUS 1

    /*
    choose whether the server will read images from the camera, or generate random ones to send
    1 = camera
    2 = random image
    */
    #define DATA_INPUT 2

    /* choose the desired bin mode (2x bin = /2 resolution and 2x brightness)
    1 = bin1
    2 = bin2
    */
    #define BIN_TYPE 1

    /*
    choose what kind of images gets generated. Random has more complexity, meaning bigger size and processing time
    1 = true random (~29 fps)
    2 = solid grayscale color (~250 fps)
    3 = rectangles of different colors. See the code for changing the number of squares (~245 fps @ 8x8)
    4 = diagonal lines (~130 fps)
    5 = boat: load a fixed number of real images (~52 fps)
    6 = load 50% black images, 50% white images for latency testing
    7 = load 50% 1/4 green bayer, 50% 1/4 blue bayer for latency testing
       - quartGreen.pgm: 97 FPS, doubleQuartGreen 56 FPS, 50blue50green: 31 FPS, green: 23 FPS
       - note: it's recommended to have at least NUMBER_OF_PREGENERATED_RANDOM_IMAGES{ 50 }
    8 = 4k test suite. Set NUMBER_OF_PREGENERATED_RANDOM_IMAGES to 800
    note: all fps values were measured when the server wasn't displaying
    */
    #define RANDOM_IMAGE_TYPE 8

    /*
    choose whether the random gen images will be pre-encoded to maximise server fps
    note: for RANDOM_IMAGE_TYPE 7, quartGreen example we ge from 97 FPS to 145 (and from 31.5 FPS to 46.3 with display)
    0 = don't pre-encode
    1 = pre-encode
    */
    #define PREENCODE_IMAGE 1

    /*
    choose whether the server will display images from the camera
    0 = don't display image
    1 = display image
    */
    #define DISPLAY_IMAGE 0

    /*
    to increase server fps when benchmarking with DATA_INPUT 2, we can display only one part of the image server-side
    note: RANDOM_IMAGE_TYPE 7, quartGreen example we go, with PREENCODE_IMAGE 1, from 46.3 FPS to:
    132 FPS @ 200 x 200
    117 FPS @ 800 x 800
     63 FPS @ 1920 x 1080
    0 = display full image
    1 = display only one part of image
    */
    #define DISPLAY_ROI 1

    // Define the region of interest (ROI)
    cv::Rect roi(0, 0, 1024, 720); // (x, y, width, height)

    /*
    choose whether the server will print the flip message in text when in reaches half of the static image buffer
    note: doesn't work if DATA_INPUT isn't 1
    0 = don't display image
    1 = display image
    */
    #define DISPLAY_FLIP 1

    /*
    choose whether the server will display images from the camera
    0 = don't print dropped frames
    1 = print dropped frames
    */
    #define PRINT_DROPPED_FRAMES 0

    //how many FIN packets will be sent after each image. When a client receives a FIN packets it stops reading packages and proceeds to process the image
    constexpr int FIN_PACKETS_TO_SEND{ 3 };

    //set how many microseconds the server waits before sending the FIN packets
    //larger values reduce packet loss, but increase the latency and reduces server FPS. 1300 Was found to be a good middle-ground
    constexpr int DELAY_BEFORE_FIN_US {1300};

    // if the server is sending pregenerated, random images, choose how many will be in a loop
    constexpr int NUMBER_OF_PREGENERATED_RANDOM_IMAGES{ 800 };

    // this sets how many images will be read from the provided filepath
    constexpr int NUMBER_OF_IMAGES_IN_SAMPLE_IMAGES{ 30 };

    // if the server is JPEG encoding the images before sending, choose the encoding settings
    constexpr int JPEG_QUALITY{ 80 };
    constexpr bool JPEG_PROG_ENC{ false };
    constexpr bool JPEG_HUFF_OPTIMIZE{ false };
    constexpr int JPEG_RST_INTERVAL{ 0 };
}
using namespace settings_server;

namespace constants
{
    constexpr int INPUT_IMAGE_WIDTH{ 3840 };
    constexpr int INPUT_IMAGE_HEIGHT{ 2160 };

    const std::string PORT_STR{ std::to_string(PORT) };

    constexpr int SWITCH_FRAME{NUMBER_OF_PREGENERATED_RANDOM_IMAGES/2};

#if (INPUT_IMAGE_TYPE == 1)
    constexpr auto INPUT_TYPE{ CV_8UC1 };
    constexpr auto IMAGE_TYPE {ASI_IMG_RAW8};
    constexpr int BITS_PER_PIXEL{ 8 };
#elif (INPUT_IMAGE_TYPE == 2)
    constexpr auto INPUT_TYPE{ CV_16UC1 };
    constexpr auto IMAGE_TYPE {ASI_IMG_RAW16};
    constexpr int BITS_PER_PIXEL{ 16 };
#elif (INPUT_IMAGE_TYPE == 3)
    constexpr auto INPUT_TYPE{ CV_8UC3 };
    constexpr auto IMAGE_TYPE {ASI_IMG_RGB24};
    constexpr int BITS_PER_PIXEL{ 8 * 3 };
#endif
    constexpr size_t MAX_IMAGE_SIZE_BYTES{ INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT * BITS_PER_PIXEL / 8 };
    constexpr size_t MAX_PACKET_NUMBER = (MAX_IMAGE_SIZE_BYTES / MAX_DATA_PACKET_SIZE_BYTES);
}
using namespace constants;

namespace globals
{
    GAsyncQueue *queue;

    //reused data structures for the server
    constexpr size_t MAXIMUM_TOTAL_PACKET_SIZE = sizeof(Packet) + (MAX_DATA_PACKET_SIZE_BYTES - sizeof(unsigned char)) * sizeof(unsigned char);
    auto* outboundPacket = reinterpret_cast<Packet*>(new char[MAXIMUM_TOTAL_PACKET_SIZE]);

    int LOOP_COUNTER {0};

    //global array of images
#if (DATA_INPUT == 2)
    cv::Mat randomImages[NUMBER_OF_PREGENERATED_RANDOM_IMAGES];
#if (PREENCODE_IMAGE == 1)
    std::vector<unsigned char> randomImagesEncoded[NUMBER_OF_PREGENERATED_RANDOM_IMAGES];
#endif
#endif

}
using namespace globals;

namespace func_util
{
    cv::Mat generateRandom16bitImage(int p_width, int p_height)
    {
        // Create an image variable
        cv::Mat image(p_height, p_width, CV_16UC1);

        // Fill the image with random data
        cv::randu(image, cv::Scalar::all(0), cv::Scalar::all(65535));

        return image;
    }

    cv::Mat generateRandom8bitImage(int p_width, int p_height)
    {
        // Create an image variable
        cv::Mat image(p_height, p_width, CV_8UC1);

        std::random_device rd; // obtain a random number from hardware
        std::mt19937 gen(rd()); // seed the generator
        std::uniform_int_distribution<> distr(1, 255); // define the range

#if (RANDOM_IMAGE_TYPE==1)
        // Fill the image with random data
        cv::randu(image, cv::Scalar::all(0), cv::Scalar::all(255));
#elif (RANDOM_IMAGE_TYPE==2)
        // Set the entire image to a specific value
        //cv::Scalar color(255, 0, 0); // Blue color (BGR order)
        cv::Scalar color(distr(gen), distr(gen), distr(gen)); // Blue color (BGR order)
        image.setTo(color);
#elif (RANDOM_IMAGE_TYPE==3)
        // Generate random colors for each square
        constexpr int numberOfRows{INPUT_IMAGE_HEIGHT/16};
        constexpr int numberOfColumns{INPUT_IMAGE_WIDTH/16};
        constexpr int rectWidth{INPUT_IMAGE_WIDTH/numberOfColumns};
        constexpr int rectHeight{INPUT_IMAGE_HEIGHT/numberOfRows};

        // offset by 1 to trigger debayering on client
        for (int y = 0; y < numberOfRows-1; y++) {
            for (int x = 0; x < numberOfColumns-1; x++) {
                // Set the color for the current square
                cv::Rect squareRegion(x * rectWidth+1, y * rectHeight+1, rectWidth, rectHeight);
                cv::Scalar color(distr(gen), distr(gen), distr(gen));
                image(squareRegion) = color;
            }
        }
#elif (RANDOM_IMAGE_TYPE==4)
        int N = 5;  // Number of pixels in each color group
        int M = (distr(gen)%5);   // Number of pixels to shift for each row

        // Generate the image with the specified color pattern
        for (int y = 0; y < INPUT_IMAGE_HEIGHT; ++y) {
            for (int x = 0; x < INPUT_IMAGE_WIDTH; ++x) {
                int colorIndex = ((x / N) + (y / N * M) + 32) % 256;
                cv::Vec3b color(colorIndex, colorIndex, colorIndex);
                image.at<uchar>(y, x) = static_cast<uchar>(colorIndex);
            }
        }
#elif (RANDOM_IMAGE_TYPE==5)
        //for this to work add the following to the makefile: BOOST = -L/usr/lib/x86_64-linux-gnu -lboost_filesystem
        /*
        boost::filesystem::path currentPath = boost::filesystem::current_path();
        std::cout << "currentPath=" << currentPath << std::endl;
        for (const auto& entry : boost::filesystem::directory_iterator(currentPath))
        {
            if (boost::filesystem::is_regular_file(entry))
            {
                std::cout << entry.path().filename() << std::endl;
            }
        }
        */

        static int startFrame = 0; // Starting frame number
        boost::filesystem::path currentPath = boost::filesystem::current_path();
        std::string folderPath = "sampleImages/4k";
        std::string filenamePattern = "output%02d.pgm"; // File name pattern with %d as the frame number placeholder

        // Create the file name by replacing the frame number placeholder in the pattern
        char buffer[100];
        snprintf(buffer, sizeof(buffer), filenamePattern.c_str(), startFrame);
        std::string filename = currentPath.string() + "/" + folderPath + "/" + buffer;

        // Load the PGM image into a cv::Mat variable
        image = cv::imread(filename, cv::IMREAD_UNCHANGED);

        // Check if the image was loaded successfully
        if (image.empty())
        {
            std::cout << "Error loading PGM image: " << filename << std::endl;
        }
        startFrame = (startFrame+1)%NUMBER_OF_IMAGES_IN_SAMPLE_IMAGES;
#elif (RANDOM_IMAGE_TYPE==6)
        static int startFrame = 0; // Starting frame number

        if (startFrame < SWITCH_FRAME)
        {
            // first half of the images are solid black
            cv::Scalar color(0, 0, 0); // Black color (BGR order)
            image.setTo(color);
        } else
        {
            // second half of the images are solid white
            cv::Scalar color(255, 255, 255); // White color (BGR order)
            image.setTo(color);
        }
        startFrame = startFrame+1;
#elif (RANDOM_IMAGE_TYPE==7)
        static int startFrame = 0; // Starting frame number

        boost::filesystem::path currentPath = boost::filesystem::current_path();
        std::string folderPath = "sampleImagesLatency/4k";

        // 97 FPS
        std::string greenPattern = "quartGreen.pgm";
        std::string bluePattern = "quartBlue.pgm";

        // 56 FPS
        //std::string greenPattern = "doubleQuartGreen.pgm";
        //std::string bluePattern = "doubleQuartBlue.pgm";

        // 31 FPS no disp, 27 FPS with disp
        //std::string greenPattern = "50red50green.pgm";
        //std::string bluePattern = "50blue50green.pgm";

        // 23 FPS
        //std::string greenPattern = "simpleGreen.pgm";
        //std::string bluePattern = "simpleBlue.pgm";


        std::string filename;
        if (startFrame < SWITCH_FRAME)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern;
        } else
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern;
        }

        // Load the PGM image into a cv::Mat variable
        image = cv::imread(filename, cv::IMREAD_UNCHANGED);

        // Check if the image was loaded successfully
        if (image.empty())
        {
            std::cout << "Error loading PGM image: " << filename << std::endl;
        }
        startFrame = startFrame+1;
#elif (RANDOM_IMAGE_TYPE==8)
        static int startFrame = 0; // Starting frame number

        boost::filesystem::path currentPath = boost::filesystem::current_path();
        std::string folderPath = "sampleImagesLatency/4k";

        std::string greenPattern12 = "12g.pgm";
        std::string bluePattern12 = "12b.pgm";

        std::string greenPattern25 = "25g.pgm";
        std::string bluePattern25 = "25b.pgm";

        std::string greenPattern37 = "37g.pgm";
        std::string bluePattern37 = "37b.pgm";

        std::string greenPattern50 = "50g.pgm";
        std::string bluePattern50 = "50b.pgm";

        std::string greenPattern62 = "62g.pgm";
        std::string bluePattern62 = "62b.pgm";

        std::string greenPattern75 = "75g.pgm";
        std::string bluePattern75 = "75b.pgm";

        std::string greenPattern87 = "87g.pgm";
        std::string bluePattern87 = "87b.pgm";

        std::string greenPattern100 = "100g.pgm";
        std::string bluePattern100 = "100b.pgm";


        std::string filename;
        if (startFrame < 20)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern12;
        }
        else if (startFrame < 40)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern12;
        }
        else if (startFrame < 60)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern12;
        }
        else if (startFrame < 80)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern12;
        }
        else if (startFrame < 100)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern12;
        }

        else if (startFrame < 120)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern25;
        }
        else if (startFrame < 140)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern25;
        }
        else if (startFrame < 160)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern25;
        }
        else if (startFrame < 180)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern25;
        }
        else if (startFrame < 200)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern25;
        }

        else if (startFrame < 220)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern37;
        }
        else if (startFrame < 240)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern37;
        }
        else if (startFrame < 260)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern37;
        }
        else if (startFrame < 280)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern37;
        }
        else if (startFrame < 300)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern37;
        }

        else if (startFrame < 320)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern50;
        }
        else if (startFrame < 340)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern50;
        }
        else if (startFrame < 360)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern50;
        }
        else if (startFrame < 380)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern50;
        }
        else if (startFrame < 400)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern50;
        }

        else if (startFrame < 420)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern62;
        }
        else if (startFrame < 440)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern62;
        }
        else if (startFrame < 460)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern62;
        }
        else if (startFrame < 480)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern62;
        }
        else if (startFrame < 500)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern62;
        }

        else if (startFrame < 520)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern75;
        }
        else if (startFrame < 540)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern75;
        }
        else if (startFrame < 560)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern75;
        }
        else if (startFrame < 580)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern75;
        }
        else if (startFrame < 600)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern75;
        }

        else if (startFrame < 620)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern87;
        }
        else if (startFrame < 640)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern87;
        }
        else if (startFrame < 660)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern87;
        }
        else if (startFrame < 680)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern87;
        }
        else if (startFrame < 700)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern87;
        }

        else if (startFrame < 720)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern100;
        }
        else if (startFrame < 740)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern100;
        }
        else if (startFrame < 760)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern100;
        }
        else if (startFrame < 780)
        {
            filename = currentPath.string() + "/" + folderPath + "/" + bluePattern100;
        }
        else
        {
            filename = currentPath.string() + "/" + folderPath + "/" + greenPattern100;
        }


        // Load the PGM image into a cv::Mat variable
        image = cv::imread(filename, cv::IMREAD_UNCHANGED);

        // Check if the image was loaded successfully
        if (image.empty())
        {
            std::cout << "Error loading PGM image: " << filename << std::endl;
        }
        startFrame = startFrame+1;
#endif
        return image;
    }

    std::string getTimeUs() {
        auto now = std::chrono::system_clock::now();
        auto now_c = std::chrono::system_clock::to_time_t(now);
        auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;
        auto tm = *std::localtime(&now_c);

        std::ostringstream oss;
        oss << std::put_time(&tm, "%H:%M:%S") << '.' << std::setw(6) << std::setfill('0') << us.count() << " (" << std::put_time(&tm, "%S") << ',' << us.count() << ")";

        return oss.str();
    }

    std::vector<unsigned char> packageImage(const cv::Mat& p_image, int quality, bool progressive, bool optimize, int rst_interval)
    {
#if (INPUT_IMAGE_TYPE == 2)
        // Convert the image back to 8-bit before encoding
            cv::Mat image8bit;
            p_image.convertTo(image8bit, CV_8UC1, 1.0 / 256.0);
#endif

        // Encode the image data as JPEG with specified parameters
        std::vector<unsigned char> encoded_image;
        std::vector<int> compression_params = { cv::IMWRITE_JPEG_QUALITY, quality,
                                                cv::IMWRITE_JPEG_PROGRESSIVE, progressive,
                                                cv::IMWRITE_JPEG_OPTIMIZE, optimize,
                                                cv::IMWRITE_JPEG_RST_INTERVAL, rst_interval };
        cv::imencode(".jpg", p_image, encoded_image, compression_params);

        return encoded_image;
    }

    bool isTopSquareBlack(cv::Mat& image)
    {
        int startX = 10;
        int startY = 10;
        int size = 10;
        int halfSize = size / 2;

        // Iterate over the pixels in the square region
        for (int y = startY; y <= startY + size; y++) {
            for (int x = startX; x <= startX + size; x++) {
                cv::Vec3b pixel = image.at<cv::Vec3b>(y, x);

                uchar intensity = (pixel[0] + pixel[1] + pixel[2]) / 3;

                if (intensity > 50) {
                    return false;
                }
            }
        }

        return true;
    }
}
using namespace func_util;

namespace func_networking
{
    udp::endpoint receiveHandshake(udp::socket& socket)
    {
        std::array<char, 100> recvData{};
        udp::endpoint source;
        bs::error_code error;
        socket.receive_from(ba::buffer(recvData), source, 0, error);
        if (error)
        {
            throw bs::system_error(error);
        }
        //std::string recStr {std::begin(recvData), std::end(recvData)};
        //std::cout << "recStr=" << recStr << std::endl;

        std::string timeString {getTimeUs()};
        std::array<unsigned char, 100> sendData{};
        std::copy(timeString.begin(), timeString.end(), sendData.data());
        socket.send_to(ba::buffer(&sendData, 100), source);

        return source;
    }

    void send_packets(unsigned char* a_sendData, size_t length, const udp::endpoint& a_endpoint, udp::socket& a_socket, int a_outgoingIndex) {
        // Calculate the total number of packets needed to send the a_sendData
        size_t totalNumberOfPackets = ((length + 1) / MAX_DATA_PACKET_SIZE_BYTES) + 1;  // '* sizeof(char)' (8) unnecessary because we would do /8 to get bytes
        if (DEBUG_LEVEL >= 3)
        {
            std::cout << "Attempting to send " << totalNumberOfPackets << " packets containing a " << length << " Byte payload\n";
        }

        // Create and send each packet
        bs::error_code ec;
        for (size_t packetIdx{ 0 }; packetIdx < totalNumberOfPackets; ++packetIdx) {
            // Calculate the size of the payload for this outboundPacket
            size_t payloadSize = std::min(MAX_DATA_PACKET_SIZE_BYTES, static_cast<size_t>(length) - (packetIdx * MAX_DATA_PACKET_SIZE_BYTES));

            // Calculate the size of the Data structure needed for the given payload size
            size_t outboundPacketSize = sizeof(Packet) + (payloadSize - sizeof(unsigned char)) * sizeof(unsigned char);

            // Allocate memory for the Data structure
            // auto* outboundPacket = reinterpret_cast<Packet*>(new char[outboundPacketSize]);

            outboundPacket->s_header.s_sequenceNumber = packetIdx;
            outboundPacket->s_header.s_totalPackets = totalNumberOfPackets;
            outboundPacket->s_header.s_totalSizeBytes = length;
            outboundPacket->s_header.s_payloadSize = payloadSize;
            outboundPacket->s_header.s_imageNumber = a_outgoingIndex;
            outboundPacket->s_header.s_FIN = false;

            // Copy the a_sendData for this outboundPacket into the outboundPacket's payload
            std::copy(a_sendData + (packetIdx * MAX_DATA_PACKET_SIZE_BYTES), a_sendData + (packetIdx * MAX_DATA_PACKET_SIZE_BYTES) + payloadSize, outboundPacket->s_payload);
            //std::copy(a_sendData.begin() + (packetIdx * MAX_PACKET_SIZE_BYTES), a_sendData.begin() + (packetIdx * MAX_PACKET_SIZE_BYTES) + payloadSize, outboundPacket->s_payload);

            // Send the outboundPacket
            a_socket.send_to(ba::buffer(outboundPacket, outboundPacketSize), a_endpoint, 0, ec);
            if (ec.failed())
            {
                std::cout << "Sending data failed with the following message: " << ec.message() << "\n";
            }
            else {
                if (DEBUG_LEVEL >= 3)
                {
                    std::cout << "Sent " << packetIdx + 1 << "/" << totalNumberOfPackets << " packets totaling " << outboundPacketSize << " Bytes\n";
                }
            }
        }

        // Send FIN_PACKETS_TO_SEND packets to indicate data sending has finished
        Packet finOutboundPacket{};
        finOutboundPacket.s_header.s_sequenceNumber = 0;
        finOutboundPacket.s_header.s_totalPackets = totalNumberOfPackets;
        finOutboundPacket.s_header.s_totalSizeBytes = length;
        finOutboundPacket.s_header.s_payloadSize = 0;
        finOutboundPacket.s_header.s_imageNumber = a_outgoingIndex;
        finOutboundPacket.s_header.s_FIN = true;

        // wait a bit for data packets to arrive, because as soon as a FIN packet arrives listening for data stops
        // 10ms of waiting = 45 fps serverside max;
        // 5ms = 80-100 FPS;
        // 1.3ms = 174 FPS solid image example, 130 FPS or diagonal lines example; both with ~0 packet loss f
        // 1ms = 190-200 FPS
        // 600us = 210-220 FPS, 0% packet loss for the solid image example
        // no delay = 250 FPS 0% packet loss for the solid image example
        // in the true random example the image quality is still quite bad at 1300, but better than with no delay (and costs <1fps)
        std::this_thread::sleep_for (std::chrono::microseconds (DELAY_BEFORE_FIN_US));

        for (int i{ 0 }; i < FIN_PACKETS_TO_SEND; i++)
        {
            a_socket.send_to(ba::buffer(&finOutboundPacket, sizeof(Packet)), a_endpoint, 0, ec);
            if (ec.failed())
            {
                std::cout << "Sending fin packet no. " << i << " failed with the following message: " << ec.message() << "\n";
            }
        }
    }
}
using namespace func_networking;


void server(ba::io_context& io)
{
    try
    {
        // open server
        udp::socket socket{ io, udp::endpoint{udp::v4(), PORT} };
        std::cout << "Server ready for 0.0.0.0:" << PORT << "\n";

        // receive client's endpoint through a handshake
        udp::endpoint endpoint = receiveHandshake(socket);
        std::cout << "Client connect from: " << endpoint << "\n";

        //print current time to synchronise the clocks
        std::cout << "Initial time S: " << getTimeUs() << "\n";

        int loopingRandomImageIndex{ 0 };
        int outgoingMessageCounter{ 0 };

        cv::Mat inputImage;
        std::vector<unsigned char> encodedImage;
        bool flip{false};
        bool topLeftSquareWasBlack{ false };

#if (CONTINUOUS != 1)
        std::cout << "Press enter to generate and send an image, or 'x' to exit.\n";
        char key{ 'a' };
#endif

        // prepare images to be sent if camera isn't used
#if (DATA_INPUT == 2)
        for (int i = 0; i < NUMBER_OF_PREGENERATED_RANDOM_IMAGES; i++) {
            // Fill the global array of images with random un-encoded images
            randomImages[i] = generateRandom8bitImage(INPUT_IMAGE_WIDTH, INPUT_IMAGE_HEIGHT);
#if (PREENCODE_IMAGE == 1)
            // Fill the global array of images with random JPEG images
            randomImagesEncoded[i] = packageImage(generateRandom8bitImage(INPUT_IMAGE_WIDTH, INPUT_IMAGE_HEIGHT), JPEG_QUALITY, JPEG_PROG_ENC, JPEG_HUFF_OPTIMIZE, JPEG_RST_INTERVAL);
#endif
        }
#endif

        // main loop
        do {
#if (CONTINUOUS != 1)
            key = std::cin.get();
                if (key != 'x') {
#endif
#if (DATA_INPUT == 1)
            inputImage = cv::cvarrToMat((IplImage*)g_async_queue_pop(queue));
#elif (DATA_INPUT == 2)
            inputImage = randomImages[loopingRandomImageIndex++];
#if (PREENCODE_IMAGE == 1)
            encodedImage = randomImagesEncoded[loopingRandomImageIndex++];
#endif
            // reset the random image loop if we reached the end of the array
            if (loopingRandomImageIndex > (NUMBER_OF_PREGENERATED_RANDOM_IMAGES - 1))
            {
                loopingRandomImageIndex = 0;
                LOOP_COUNTER++;
            }
#endif

#if (RANDOM_IMAGE_TYPE==8)
            if (loopingRandomImageIndex == 1)
            {
                std::cout << "12%\n";
            } else if (loopingRandomImageIndex == 100)
            {
                std::cout << "25%\n";
            } else if (loopingRandomImageIndex == 200)
            {
                std::cout << "37%\n";
            } else if (loopingRandomImageIndex == 300)
            {
                std::cout << "50%\n";
            } else if (loopingRandomImageIndex == 400)
            {
                std::cout << "62%\n";
            } else if (loopingRandomImageIndex == 500)
            {
                std::cout << "75%\n";
            } else if (loopingRandomImageIndex == 600)
            {
                std::cout << "87%\n";
            } else if (loopingRandomImageIndex == 700)
            {
                std::cout << "100%\n";
            }
#endif


#if (ENCODE_AS_JPEG == 0)
            // send raw
            send_packets(inputImage.data, (inputImage.total() * inputImage.elemSize()), endpoint, socket, outgoingMessageCounter);
#elif (ENCODE_AS_JPEG == 1)
            // send jpeg
#if (PREENCODE_IMAGE == 0)
            encodedImage = std::move(packageImage(inputImage, JPEG_QUALITY, JPEG_PROG_ENC, JPEG_HUFF_OPTIMIZE, JPEG_RST_INTERVAL));
#endif
            send_packets(encodedImage.data(), encodedImage.size(), endpoint, socket, outgoingMessageCounter);
#endif
#if(DISPLAY_FLIP == 1)
            if (isTopSquareBlack(inputImage) && !topLeftSquareWasBlack)
            {
                std::cout << "swap to black S: " << getTimeUs() << " on image #" << outgoingMessageCounter << "\n";
                topLeftSquareWasBlack = true;
            }
            else if (!isTopSquareBlack(inputImage) && topLeftSquareWasBlack)
            {
                std::cout << "swap to white S: " << getTimeUs() << " on image #" << outgoingMessageCounter << "\n";
                topLeftSquareWasBlack = false;
            }

            /*
            if (loopingRandomImageIndex == SWITCH_FRAME)
            {
                if (flip)
                {
                    std::cout << "#### SWITCH #### " << outgoingMessageCounter << " #### SWITCH ####\n";
                } else
                {
                    std::cout << outgoingMessageCounter << " #### SWITCH ####\n";
                }
                flip = (!flip);
            }
             */
#endif
#if (DISPLAY_IMAGE == 1 && DATA_INPUT == 2)
            // show image on screen
            try {
#if (DISPLAY_ROI == 0)
                cv::imshow("video", inputImage);
#else
                cv::Mat roiImage = inputImage(roi);
                cv::imshow("ROI Image", roiImage);
#endif
            } catch (cv::Exception &e) {
                std::cerr << "OpenCV Exception for cv::imshow: " << e.what() << std::endl;
            }
            char c = cvWaitKey(1);
#endif
            if (DEBUG_LEVEL >= 1)
            {
                std::cout << "Image " << outgoingMessageCounter << " sent successfully at " << getTimeUs() << "\n";
            }
            outgoingMessageCounter++;
#if (CONTINUOUS != 1)
            }
                else
                {
                    break;
                }
#endif
            //std::this_thread::sleep_for(std::chrono::seconds(1));
        } while (bServer);
    }
    catch (std::exception& e)
    {
        std::cerr << "Exception: " << e.what() << "\n";
    }
}


namespace func_zwo
{
    int listAndChooseCamera()
    {
        int numDevices = ASIGetNumOfConnectedCameras();
        if(numDevices <= 0)
        {
            printf("no camera connected, press any key to exit\n");
            getchar();
            exit(-1);
        }
        else
            printf("attached cameras:\n");

        for(int i {0}; i < numDevices; i++)
        {
            ASIGetCameraProperty(&CamInfo, i);
            printf("%d %s\n",i, CamInfo.Name);
        }

        printf("\nselect one to preview\n");
        int CamIndex {0};
        //scanf("%d", &CamIndex);
        return CamIndex;
    }

    void printCameraInfo(int CamIndex)
    {
        bool bresult;

        ASIGetCameraProperty(&CamInfo, CamIndex);
        bresult = ASIOpenCamera(CamInfo.CameraID);
        bresult += ASIInitCamera(CamInfo.CameraID);
        if(bresult)
        {
            printf("OpenCamera error,are you root?,press any key to exit\n");
            getchar();
            exit(-1);
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
        for(int i {0}; i < ctrlnum; i++)
        {
            ASIGetControlCaps(CamInfo.CameraID, i,&ctrlcap);
            printf("%s\n", ctrlcap.Name);
        }
    }

    void configureCamera(int exposureTime, ASI_CAMERA_MODE &mode)
    {
        int piNumberOfControls {0};
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

        ASISetControlValue(CamInfo.CameraID,ASI_EXPOSURE, exposureTime*1000, ASI_FALSE);
        ASISetControlValue(CamInfo.CameraID,ASI_GAIN,0, ASI_FALSE);
        ASISetControlValue(CamInfo.CameraID,ASI_BANDWIDTHOVERLOAD, transferSpeed, ASI_FALSE);
        ASISetControlValue(CamInfo.CameraID,ASI_HIGH_SPEED_MODE, 1, ASI_FALSE); //high speed mode=10bit ADC instead of 12
        ASISetControlValue(CamInfo.CameraID,ASI_WB_B, 90, ASI_FALSE);
        ASISetControlValue(CamInfo.CameraID,ASI_WB_R, 48, ASI_TRUE);

        if(CamInfo.IsTriggerCam)
        {
            printf("This is multi mode camera, you need to select the camera mode:\n");
            ASI_SUPPORTED_MODE cammode;
            ASIGetCameraSupportMode(CamInfo.CameraID, &cammode);
            int i{0};
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
                    printf("%d:Trigger Low  Level Mode\n", i);
                i++;
            }

            int modeIndex{0};
            scanf("%d", &modeIndex);
            ASISetCameraMode(CamInfo.CameraID, cammode.SupportedCameraMode[modeIndex]);
            ASIGetCameraMode(CamInfo.CameraID, &mode);
            if(mode != cammode.SupportedCameraMode[modeIndex])
                printf("Set mode failed!\n");
        }
    }

    void* Display(void* params)
    {
        IplImage *pImg = (IplImage *)params;
        cvNamedWindow("video", 1);

        while(bDisplay)
        {
            // push image to buffer
            g_async_queue_push(queue, pImg);

#if (DISPLAY_IMAGE == 1 && DATA_INPUT == 1)
            // show image on screen
            try {
                cvShowImage("video", pImg);
            } catch (cv::Exception &e) {
                std::cerr << "OpenCV Exception for cvShowImage: " << e.what() << std::endl;
            }
#endif
            char c = cvWaitKey(1);
            if(c == 27) //esc
            {
                bDisplay = 0;
                bServer = 0;
                bMain = 0;
                goto END_DISPLAY;
            }

        }

        END_DISPLAY:
#if (DISPLAY_IMAGE == 1)
        cvDestroyWindow("video");
#endif
        printf("Display thread over\n");
        ASIStopVideoCapture(CamInfo.CameraID);
        return nullptr;
    }
}
using namespace func_zwo;


int  main()
{
#if (DATA_INPUT != 2)
    // choosing the desired camera
    int camIndex {listAndChooseCamera()};
    printCameraInfo(camIndex);


    // set camera format according to chosen settings
    ASISetROIFormat(CamInfo.CameraID, INPUT_IMAGE_WIDTH, INPUT_IMAGE_HEIGHT, BIN_TYPE, IMAGE_TYPE);

    IplImage *pRgb;
    if(IMAGE_TYPE == ASI_IMG_RAW16)
        pRgb=cvCreateImage(cvSize(INPUT_IMAGE_WIDTH,INPUT_IMAGE_HEIGHT), IPL_DEPTH_16U, 1);
    else if(IMAGE_TYPE == ASI_IMG_RGB24)
        pRgb=cvCreateImage(cvSize(INPUT_IMAGE_WIDTH,INPUT_IMAGE_HEIGHT), IPL_DEPTH_8U, 3);
    else
        pRgb=cvCreateImage(cvSize(INPUT_IMAGE_WIDTH,INPUT_IMAGE_HEIGHT), IPL_DEPTH_8U, 1);

    int exposureTime {0};
    printf("Please input exposure time(ms):\n"
           "1 ms - sunny windows\n"
           "10 ms - indoor lighting (recommended)\n"
           "50 ms - big pinhole\n"
           "300 ms - small pinhole\n");
    //scanf("%d", &exposureTime);
    exposureTime = 5;

    ASI_CAMERA_MODE mode;
    configureCamera(exposureTime, mode);


    // print of final settings
    printf("\nStarting recording with following settings:\n");
    printf("Resolution: %dx%d\n", INPUT_IMAGE_WIDTH, INPUT_IMAGE_HEIGHT);
    printf("Bin mode: %d (2x bin = /2 resolution and 2x brightness)\n", BIN_TYPE);
    printf("Image type: %d (0=ASI_IMG_RAW8, 1=ASI_IMG_RGB24 and 2=ASI_IMG_RAW16)\n", IMAGE_TYPE);
    printf("Exposition time: %d (max FPS =1/%d=%d)\n", exposureTime, exposureTime, int(1000/exposureTime));

    printf("\nEncode as JPEG: %d\n", ENCODE_AS_JPEG);
#endif

    // starting server thread
    ba::io_context io;

    queue = g_async_queue_new();

    std::thread threadServer(server, std::ref(io));

#if (DATA_INPUT != 2)
    // start video capture
    ASIStartVideoCapture(CamInfo.CameraID);

    printf("\ncontrols:\n"
           "         esc - stop\n");


    // print sensor temperature
    long temperature;
    ASI_BOOL bAuto;
    ASIGetControlValue(CamInfo.CameraID, ASI_TEMPERATURE, &temperature, &bAuto);
    printf("\nSensor temperature:%.1f\n", temperature/10.0);


    // start the display thread
    bDisplay = 1;
    std::thread threadDisplay(Display, static_cast<void*>(pRgb));


    // read images from camera and print lost frames
#if (PRINT_DROPPED_FRAMES == 1)
    int time1,time2;
    time1 = GetTickCount();
    int iDropFrame;
#endif

    int count {0};
    while(bMain)
    {
        if(mode == ASI_MODE_NORMAL)
        {
            if(ASIGetVideoData(CamInfo.CameraID, (unsigned char*)pRgb->imageData, pRgb->imageSize, 500) == ASI_SUCCESS)
            {
                count++;
            }

        }
        else
        {
            if(ASIGetVideoData(CamInfo.CameraID, (unsigned char*)pRgb->imageData, pRgb->imageSize, 1000) == ASI_SUCCESS)
            {
                count++;
            }
        }

#if (PRINT_DROPPED_FRAMES == 1)
        time2 = GetTickCount();
        if(time2-time1 > 1000 )
        {
            ASIGetDroppedFrames(CamInfo.CameraID, &iDropFrame);
            printf("fps:%d dropped frames:%d ImageType:%d\n",count, iDropFrame, IMAGE_TYPE);

            count = 0;
            time1=GetTickCount();
        }
#endif
    }


    // zwo cleanup
    if(bDisplay)
    {
        bDisplay = 0;
        threadDisplay.join();
    }

    ASIStopVideoCapture(CamInfo.CameraID);
    ASICloseCamera(CamInfo.CameraID);
    cvReleaseImage(&pRgb);
#endif

#if (DATA_INPUT == 2)
    char stopServerThread {'a'};
    printf("Input anything besides 'a' to stop the server thread\n");
    while(stopServerThread == 'a')
    {
        scanf("%c", &stopServerThread);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif

    // server cleanup
    if(bServer)
    {
        bServer = 0;
        threadServer.join();
    }
    printf("main function over\n");

    return 1;
}
