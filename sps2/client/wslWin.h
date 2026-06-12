#pragma once

// uncomment in VS19
//#pragma warning(disable:4996)

#include <iostream>
#include <exception>

#include <boost/array.hpp>
#include <boost/asio.hpp>
#include <boost/asio/ip/udp.hpp>
#include <boost/asio/io_context.hpp>

#include <chrono>
#include <iomanip>
#include <thread>
#include <atomic>

#include <vector>
#include <map>
#include <array>
#include <string>

#include <random>

#include <tbb/tbb.h>
#include <tbb/concurrent_queue.h>

#include <opencv2/opencv.hpp>
#include "opencv2/core.hpp"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/highgui.hpp"
#include "opencv2/highgui/highgui_c.h"
#include "opencv2/imgproc.hpp"

#include <boost/asio/thread_pool.hpp>
#include <boost/asio/post.hpp>

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

    class BufferManager
    {
    private:
        std::mutex mutex_;

        int bufferA;    // buffer where we activly receive an image
        int bufferB;    // buffer where the last full iamge got received
        int bufferC;    // buffer which is currently being processed, or was last processed

        bool bufferBisReady;    // set to true when B has a full image ready for processing
        bool bufferCisBusy;     // set to true while C is being used

    public:
        BufferManager() : bufferA{ 1 }, bufferB{ 2 }, bufferC{ 3 }, bufferCisBusy{ 0 } {}

        int getNextFreeBuffer()
        {
            std::lock_guard<std::mutex> lock(mutex_);

            return bufferA;
        }
        void setLastFreeBufferToReady()
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // the just updated bufferA becomes bufferB -> bufferB is the latest and ready to return, bufferA is again ready to receive a new image
            std::swap(bufferA, bufferB);
            bufferBisReady = true;
        }
        int getAndLockLastFilledBuffer()
        {
            std::lock_guard<std::mutex> lock(mutex_);

            assert((bufferCisBusy == false) && "bufferCisBusy is true, meaning the processing isn't finished or the processing function didn't call unlockLastLockedBuffer()");
            //assert((bufferBisReady == true) && "bufferBisReady isn't true, meaning there isn't an image ready for processing in B or the loading function didn't call setLastFreeBufferToReady()");
            if (bufferBisReady != true)
            {
                //std::cout << "BufferManager error: no ready image or the loading function didn't call setLastFreeBufferToReady()" << std::endl;
                return 0;
            }
            bufferBisReady = false;

            // we save which buffer is currently being processed in bufferC and mark it as busy in latestBusyBuffer
            std::swap(bufferB, bufferC);
            bufferCisBusy = true;
            return bufferC;
        }
        void unlockLastLockedBuffer()
        {
            std::lock_guard<std::mutex> lock(mutex_);

            // we reset the latestBusyBuffer variables, meaning no buffer is being processed
            bufferCisBusy = false;
        }
    };
}
using namespace definitions;
