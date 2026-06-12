#include "wslWin.h"
#include "settings.h"

namespace sps
{
    namespace constants
    {
        constexpr int INPUT_IMAGE_WIDTH{ 3840 };
        constexpr int INPUT_IMAGE_HEIGHT{ 2160 };

#if (INPUT_IMAGE_TYPE == 1)
        constexpr auto INPUT_TYPE{ CV_8UC1 };
        constexpr int BITS_PER_PIXEL{ 8 };
#elif (INPUT_IMAGE_TYPE == 2)
        constexpr auto INPUT_TYPE{ CV_16UC1 };
        constexpr int BITS_PER_PIXEL{ 16 };
#elif (INPUT_IMAGE_TYPE == 3)
        constexpr auto INPUT_TYPE{ CV_8UC3 };
        constexpr int BITS_PER_PIXEL{ 8 * 3 };
#endif

        constexpr int DEBAYERING_BLOCK_NUMBER{ 2 * 2 };     // for DEBAYER_METHOD 2 and 3; best results: 4 or 16
        constexpr int NUMBER_OF_DEBAYERING_THREADS{ DEBAYERING_BLOCK_NUMBER };
        constexpr int TASK_ARENA_NUMBER{ 1 };               // for DEBAYER_METHOD 4; best results: 1

        constexpr int BLOCKS_PER_ROW{ DEBAYERING_BLOCK_NUMBER / 2 };
        constexpr int BLOCKS_PER_COL{ 2 };

        constexpr int BLOCK_WIDTH{ INPUT_IMAGE_WIDTH / BLOCKS_PER_ROW };
        constexpr int BLOCK_HEIGHT{ INPUT_IMAGE_HEIGHT / BLOCKS_PER_COL };

        constexpr size_t MAX_IMAGE_SIZE_BYTES{ INPUT_IMAGE_WIDTH * INPUT_IMAGE_HEIGHT * BITS_PER_PIXEL / 8 };
        constexpr size_t MAX_PACKET_NUMBER = (MAX_IMAGE_SIZE_BYTES / MAX_DATA_PACKET_SIZE_BYTES);
    }
    using namespace constants;

    namespace globals
    {
        // reused data structures for the client
        std::vector<unsigned char> receivedImage1;
        std::vector<unsigned char> receivedImage2;
        std::vector<unsigned char> receivedImage3;

        int receivedImage1index;
        int receivedImage2index;
        int receivedImage3index;

        std::vector<std::vector<unsigned char>> receivedPayloads(MAX_PACKET_NUMBER);
        std::vector<bool> receivedIndexes(MAX_PACKET_NUMBER, false);

        BufferManager bufferManager;

        std::array<unsigned char, (sizeof(Packet) + MAX_DATA_PACKET_SIZE_BYTES)> recv_buf{};

        boost::asio::thread_pool pool(DEBAYERING_BLOCK_NUMBER);
        boost::asio::strand<boost::asio::thread_pool::executor_type> strand(pool.get_executor());   // ensure that the tasks are executed sequentially within the thread pool

        std::atomic<int> taskCounter(0); // keep track of the completed tasks.

        int numberOfDecodedImages{ 0 };

        bool runThreads{ true };
    }
    using namespace globals;

    namespace func_util
    {
        // Prints the raw contents of the entire packet
        void printPacketRaw(const size_t packetSize, const Packet* packet)
        {
            std::ostringstream outputStream;

            outputStream << "Packet raw:\n";
            outputStream << "------------\n";
            auto* sent_data = reinterpret_cast<const unsigned char*>(packet);
            for (size_t i = 0; i < packetSize; i++) {
                outputStream << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(sent_data[i]) << " ";
            }
            outputStream << "\n";

            std::string outputString = outputStream.str();
            std::cout << outputString;
        }

        // Prints the raw contents of the packet header
        void printHeader(const Packet* packet)
        {
            std::ostringstream outputStream;

            outputStream << "Header:\n";
            outputStream << "------------\n";
            outputStream << "Current sequence number:   " << packet->s_header.s_sequenceNumber << "\n";
            outputStream << "Current payload size:      " << packet->s_header.s_payloadSize << "\n";
            outputStream << "Total number of packets:   " << packet->s_header.s_totalPackets << "\n";
            outputStream << "Total payload size:        " << packet->s_header.s_totalSizeBytes << "\n";

            // Get the contents of the stream as a string and print
            std::string outputString = outputStream.str();
            std::cout << outputString;
        }

        // Prints the raw contents of the packet payload
        void printPayloadRaw(const Packet* packet) {
            std::ostringstream outputStream;

            outputStream << "Payload raw:\n";
            outputStream << "------------\n";
            for (unsigned int i = 0; i < packet->s_header.s_payloadSize; i += 20) {
                for (unsigned int j = i; j < i + 20 && j < packet->s_header.s_payloadSize; j++) {
                    outputStream << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(packet->s_payload[j]) << " ";
                }
                outputStream << "\n";
            }
            outputStream << "\n";

            std::string outputString = outputStream.str();
            std::cout << outputString;
        }

        // Prints the raw contents of the packet payload which was reinterpreted to a std::vector<unsigned char>
        void printPayload(const Packet* inboundPacket, const std::vector<unsigned char>& inboundPayload) {
            std::ostringstream outputStream;

            outputStream << "Payload:\n";
            outputStream << "------------\n";
            for (unsigned int i = 0; i < inboundPacket->s_header.s_payloadSize; i++)
            {
                std::cout << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(inboundPayload[i]) << " ";
            }
            outputStream << "\n";

            std::string outputString = outputStream.str();
            std::cout << outputString;
        }

        std::string getTimeUs()
        {
            auto now = std::chrono::system_clock::now();
            auto now_c = std::chrono::system_clock::to_time_t(now);
            auto us = std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()) % 1000000;
            auto tm = *std::localtime(&now_c);

            std::ostringstream oss;
            oss << std::put_time(&tm, "%H:%M:%S") << '.' << std::setw(6) << std::setfill('0') << us.count() << " (" << std::put_time(&tm, "%S") << "," << us.count() << ")";

            return oss.str();
        }

        /*
        A function which returns the amount of milliseconds since it was last called
        Returns 0 on the first call
        */
        long long getMillisecondsSinceLastCall() {
            static auto lastTime = std::chrono::high_resolution_clock::now();
            auto currentTime = std::chrono::high_resolution_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(currentTime - lastTime);
            lastTime = currentTime;
            return duration.count();
        }

        /*
        This function takes a vector of received payloads, a vector containing flags indicating whether a particular packet was received, any packet header, maximum size of the data payload and a fill value.
        It then iteraters through the packets, finds the missing ones and it's expected size (which is the maximum for N-1 packets and smaller for the N-th packet) and generates a filler packet consisting of the fill value
        */
        void fillMissingPackets(std::vector<std::vector<unsigned char>>& a_receivedPayloads, const std::vector<bool>& a_receivedIndexes, const PacketHeader& header, size_t maxDataPacketSizeBytes, int missingFillValue = 0x00)
        {
            size_t originalSize{};
            size_t addedSize{};

            //assert(a_receivedIndexes.size() == header.s_totalPackets);
            //assert((((header.s_totalSizeBytes + 1) / maxDataPacketSizeBytes) + 1) == header.s_totalPackets);
            size_t finalPacketSize = header.s_totalSizeBytes % maxDataPacketSizeBytes;
            //assert(((header.s_totalPackets-1)*maxDataPacketSizeBytes + finalPacketSize) == header.s_totalSizeBytes);

            for (size_t packetIdx{ 0 }; packetIdx < header.s_totalPackets; ++packetIdx) {
                size_t expectedSizeOfCurrentPacket = ((packetIdx + 1) == header.s_totalPackets) ? finalPacketSize : maxDataPacketSizeBytes;

                if (packetIdx > a_receivedIndexes.size())
                {
                    continue;
                }

                if (a_receivedIndexes[packetIdx])
                {
                    // If the packet was received we just count the received size
                    originalSize += expectedSizeOfCurrentPacket;
                }
                else {
                    // If the packet wasn't received fill it with placeholder data
                    a_receivedPayloads[packetIdx] = std::vector<unsigned char>(expectedSizeOfCurrentPacket, missingFillValue);
                    addedSize += expectedSizeOfCurrentPacket;
                }
            }

            //assert((originalSize + addedSize) == header.s_totalSizeBytes);
            //std::cout << "Added " << addedSize << " bytes to a " << originalSize << " byte packet to meet the expected " << header.s_totalSizeBytes << " byte size\n";
        }

        /*
        Concatenates packets in receivedPayloads into single vector
        */
        void concatenateFragments(std::vector<std::vector<unsigned char>>& receivedPayloads, int imageIndex) {
            std::vector<unsigned char>* concatenatedPayloads{ nullptr };  //receivedImage
            switch (bufferManager.getNextFreeBuffer())
            {
            case 1:
                concatenatedPayloads = &receivedImage1;
                receivedImage1index = imageIndex;
                break;
            case 2:
                concatenatedPayloads = &receivedImage2;
                receivedImage2index = imageIndex;
                break;
            case 3:
                concatenatedPayloads = &receivedImage3;
                receivedImage3index = imageIndex;
                break;
            default:
                assert(false && "BufferManager didn't return a buffer from {1, 2, 3} when asked for getNextFreeBuffer()");
                break;
            }

            if (concatenatedPayloads == nullptr)
            {
                std::cout << "concatenatedPayloads received a nullptr - receivedImage1, 2 or 3 wasn't initialized. Exiting\n";
                exit(1);
            }
            concatenatedPayloads->clear(); // Remove old contents, keeping the allocated memory

            size_t sumOfFragmentSizes{ 0 };
            for (const auto& payload : receivedPayloads) {
                sumOfFragmentSizes += payload.size();
                concatenatedPayloads->insert(concatenatedPayloads->end(), payload.begin(), payload.end());
                //std::copy(payload.begin(), payload.end(), std::back_inserter(*concatenatedPayloads)); // this is extremly slow
            }

            // Size of concatenated payloads doesn't match the total size received in header
            //assert(sumOfFragmentSizes == totalReceivedSize);

            bufferManager.setLastFreeBufferToReady();

            return;
        }

        void printPerfStats(int& droppedFrameCounter, int& incomingMessageCounter, long long& msCounter)
        {
            double percentage;
            double receiveFps;
            double decodeFps;
            double serverFps;

            std::ostringstream oss;
            percentage = (double)droppedFrameCounter / incomingMessageCounter * 100;
            oss << "Loss rate=" << std::fixed << std::setprecision(2) << percentage << "% (" << droppedFrameCounter << "/" << incomingMessageCounter << ")\n";

            receiveFps = (static_cast<double>(incomingMessageCounter - droppedFrameCounter) / (static_cast<double>(msCounter) / 1000));
            oss << "Receive FPS=" << receiveFps << " (" << (incomingMessageCounter - droppedFrameCounter) << "/" << static_cast<double>(msCounter) / 1000 << ")\n";

            decodeFps = (static_cast<double>(numberOfDecodedImages) / (static_cast<double>(msCounter) / 1000));
            serverFps = (static_cast<double>(incomingMessageCounter) / (static_cast<double>(msCounter) / 1000));
            oss << "Decode FPS=" << decodeFps << " (" << numberOfDecodedImages << "/" << static_cast<double>(msCounter) / 1000 << ")";
            oss << " (" << (decodeFps / receiveFps) * 100 << "% of received FPS, " << (decodeFps / serverFps) * 100 << "% of server FPS)\n";

            oss << "Server fps=" << serverFps << " (" << incomingMessageCounter << "/" << static_cast<double>(msCounter) / 1000 << ")\n";
            std::cout << oss.str();
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

        int calculateRtt(std::string& start, std::string& end)
        {
            // Create string stream from the time string
            std::istringstream issStart(start);
            std::istringstream issEnd(end);
            std::string segment;

            std::getline(issStart, segment, ':'); // Extract hours segment
            int hoursStart = std::stoi(segment);

            std::getline(issStart, segment, ':'); // Extract minutes segment
            int minutesStart = std::stoi(segment);

            std::getline(issStart, segment, '.'); // Extract seconds segment
            int secondsStart = std::stoi(segment);

            std::getline(issStart, segment); // Extract microseconds segment
            int microsecondsStart = std::stoi(segment);


            std::getline(issEnd, segment, ':'); // Extract hours segment
            int hoursEnd = std::stoi(segment);

            std::getline(issEnd, segment, ':'); // Extract minutes segment
            int minutesEnd = std::stoi(segment);

            std::getline(issEnd, segment, '.'); // Extract seconds segment
            int secondsEnd = std::stoi(segment);

            std::getline(issEnd, segment); // Extract microseconds segment
            int microsecondsEnd = std::stoi(segment);

            return ((secondsEnd * 1000000 + microsecondsEnd) - (secondsStart * 1000000 + microsecondsStart)) / 2;
        }

        int calculateTimeDiff(std::string& serverTime, std::string& timeStringEnd, int rttUs)
        {
            // Create string stream from the time string
            std::istringstream issServer(serverTime);
            std::istringstream issEnd(timeStringEnd);
            std::string segment;

            std::getline(issServer, segment, ':'); // Extract hours segment
            int hoursServer = std::stoi(segment);

            std::getline(issServer, segment, ':'); // Extract minutes segment
            int minutesServer = std::stoi(segment);

            std::getline(issServer, segment, '.'); // Extract seconds segment
            int secondsServer = std::stoi(segment);

            std::getline(issServer, segment); // Extract microseconds segment
            int microsecondsServer = std::stoi(segment);


            std::getline(issEnd, segment, ':'); // Extract hours segment
            int hoursEnd = std::stoi(segment);

            std::getline(issEnd, segment, ':'); // Extract minutes segment
            int minutesEnd = std::stoi(segment);

            std::getline(issEnd, segment, '.'); // Extract seconds segment
            int secondsEnd = std::stoi(segment);

            std::getline(issEnd, segment); // Extract microseconds segment
            int microsecondsEnd = std::stoi(segment);

            return (((secondsServer * 1000000 + microsecondsServer) + rttUs) - (secondsEnd * 1000000 + microsecondsEnd));
        }
    }
    using namespace func_util;

    namespace func_debayering
    {
        cv::Mat debayerImageTbbArena(cv::Mat& p_decodedImage)
        {
            // Split the image into blocks and debayer them in parallel
            std::map<int, cv::Mat> blocks;
            for (int i = 0; i < INPUT_IMAGE_HEIGHT; i += BLOCK_HEIGHT) {
                for (int j = 0; j < INPUT_IMAGE_WIDTH; j += BLOCK_WIDTH) {
                    cv::Rect roi(j, i, BLOCK_WIDTH, BLOCK_HEIGHT);
                    int block_idx = (i / BLOCK_HEIGHT) * BLOCKS_PER_ROW + j / BLOCK_WIDTH;
                    blocks[block_idx] = std::move(p_decodedImage(roi));
                }
            }
            if (DEBUG_LEVEL >= 2)
            {
                std::cout << "Split ok\n";
            }

            std::vector<cv::Mat> debayered_blocks(DEBAYERING_BLOCK_NUMBER);

            tbb::task_arena arena(TASK_ARENA_NUMBER);
            arena.execute([&] {
                tbb::parallel_for(tbb::blocked_range<size_t>(0, debayered_blocks.size()), [&](const tbb::blocked_range<size_t>& range) {
                    for (size_t i = range.begin(); i != range.end(); ++i) {
                        int block_idx = i;
                        auto block_iter = blocks.find(block_idx);
                        if (block_iter != blocks.end()) {
                            cv::cvtColor(block_iter->second, debayered_blocks[block_idx], cv::COLOR_BayerBG2BGR);
                        }
                    }
                    });
                });
            if (DEBUG_LEVEL >= 2)
            {
                std::cout << "debayer ok\n";
            }

            // Combine the debayered blocks into a single image
            cv::Mat debayered(INPUT_IMAGE_HEIGHT, INPUT_IMAGE_WIDTH, CV_8UC3);
            for (int i = 0; i < INPUT_IMAGE_HEIGHT; i += BLOCK_HEIGHT) {
                for (int j = 0; j < INPUT_IMAGE_WIDTH; j += BLOCK_WIDTH) {
                    cv::Rect roi(j, i, BLOCK_WIDTH, BLOCK_HEIGHT);
                    int block_idx = (i / BLOCK_HEIGHT) * BLOCKS_PER_ROW + j / BLOCK_WIDTH;
                    debayered_blocks[block_idx].copyTo(debayered(roi));
                }
            }
            if (DEBUG_LEVEL >= 2)
            {
                std::cout << "combine ok\n";
            }

#if (DO_COLOR_CORRECTION==1)
            // Decrease the amount of green
            debayered -= cv::Scalar(0, 50, 0);  // Adjust the green channel values

            // Increase the brightness
            cv::convertScaleAbs(debayered, debayered, 1.2, 10);  // Adjust the brightness and contrast
#endif

            return debayered;
        }

        void debayerHelper(cv::Mat& bayer_image, cv::Mat& color_image)
        {
            color_image.create(bayer_image.size(), CV_8UC3);
            try
            {
                cv::cvtColor(bayer_image, color_image, cv::COLOR_BayerBG2BGR);  //good for camera; too much green and purple for my images

#if (DO_COLOR_CORRECTION==1)
                // Decrease the amount of green
                color_image -= cv::Scalar(0, 50, 0);  // Adjust the green channel values

                // Increase the brightness
                cv::convertScaleAbs(color_image, color_image, 1.2, 10);  // Adjust the brightness and contrast
#endif

            }
            catch (const std::exception& e)
            {
                std::cerr << "ln290 debayerHelper cv::cvtColor failed with: " << e.what() << "\n";
            }

            if (DEBUG_LEVEL >= 2)
            {
                /*
                std::cerr << "bayer_image.channels()=" << bayer_image.channels() << "\n";
                std::cerr << "bayer_image.depth()=" << bayer_image.depth() << ", bayer_image.size()=" << bayer_image.size() << "\n";
                std::cerr << "bayer_image.empty()=" << bayer_image.empty() << ", bayer_image.type()=" << bayer_image.type() << "\n";

                std::cerr << "color_image.channels()=" << color_image.channels() << "\n";
                std::cerr << "color_image.depth()=" << color_image.depth() << ", color_image.size()=" << color_image.size() << "\n";
                std::cerr << "color_image.empty()=" << color_image.empty() << ", color_image.type()=" << color_image.type() << "\n";
                */
            }

            taskCounter.fetch_add(1);
        }

        cv::Mat debayerImageBoost(cv::Mat& p_decodedImage)
        {
            // Split the image into blocks and debayer them in parallel
            std::map<int, cv::Mat> blocks;
            for (int i = 0; i < INPUT_IMAGE_HEIGHT; i += BLOCK_HEIGHT) {
                for (int j = 0; j < INPUT_IMAGE_WIDTH; j += BLOCK_WIDTH) {
                    cv::Rect roi(j, i, BLOCK_WIDTH, BLOCK_HEIGHT);
                    int block_idx = (i / BLOCK_HEIGHT) * BLOCKS_PER_ROW + j / BLOCK_WIDTH;
                    blocks[block_idx] = std::move(p_decodedImage(roi));
                }
            }
            if (DEBUG_LEVEL >= 2)
            {
                std::cout << "Split ok\n";
            }

            std::vector<cv::Mat> debayered_blocks(DEBAYERING_BLOCK_NUMBER);
            taskCounter = 0;

            // Submit tasks to the thread pool to debayer the image in parallel
            for (int i = 0; i < DEBAYERING_BLOCK_NUMBER; ++i) {
                boost::asio::post(strand, std::bind(debayerHelper, std::ref(blocks[i]), std::ref(debayered_blocks[i])));
            }

            // Wait for all tasks to finish
            while (taskCounter.load() != (DEBAYERING_BLOCK_NUMBER))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }

            if (DEBUG_LEVEL >= 2)
            {
                std::cout << "debayer ok\n";
            }

            // Combine the debayered blocks into a single image
            cv::Mat debayered(INPUT_IMAGE_HEIGHT, INPUT_IMAGE_WIDTH, CV_8UC3);
            for (int i = 0; i < INPUT_IMAGE_HEIGHT; i += BLOCK_HEIGHT) {
                for (int j = 0; j < INPUT_IMAGE_WIDTH; j += BLOCK_WIDTH) {
                    cv::Rect roi(j, i, BLOCK_WIDTH, BLOCK_HEIGHT);
                    int block_idx = (i / BLOCK_HEIGHT) * BLOCKS_PER_ROW + j / BLOCK_WIDTH;
                    try
                    {
                        debayered_blocks[block_idx].copyTo(debayered(roi));
                    }
                    catch (const std::exception& e)
                    {
                        debayered_blocks[block_idx].channels();
                        debayered.channels();
                        std::cerr << "ln260 Boost copyTo failed with: " << e.what() << "\n";
                    }

                    if (DEBUG_LEVEL >= 3)
                    {
                        std::cerr << "debayered_blocks[block_idx].channels()=" << debayered_blocks[block_idx].channels() << ", debayered.channels()=" << debayered.channels() << "\n";
                        std::cerr << "debayered_blocks[block_idx].depth()=" << debayered_blocks[block_idx].depth() << ", debayered_blocks[block_idx].size()=" << debayered_blocks[block_idx].size() << "\n";
                        std::cerr << "debayered_blocks[block_idx].empty()=" << debayered_blocks[block_idx].empty() << ", debayered_blocks[block_idx].type()=" << debayered_blocks[block_idx].type() << "\n";
                        std::cerr << "debayered_blocks[block_idx].rows=" << debayered_blocks[block_idx].rows << ", debayered_blocks[block_idx].cols=" << debayered_blocks[block_idx].cols << "\n";
                    }
                }
            }
            if (DEBUG_LEVEL >= 2)
            {
                std::cout << "combine ok\n";
            }

#if (DO_COLOR_CORRECTION==1)
            // Decrease the amount of green
            debayered -= cv::Scalar(0, 50, 0);  // Adjust the green channel values

            // Increase the brightness
            cv::convertScaleAbs(debayered, debayered, 1.2, 10);  // Adjust the brightness and contrast
#endif

            return debayered;
        }

        cv::Mat debayerImageGeneric(cv::Mat& p_decodedImage)
        {
            // Split the image into blocks and debayer them in parallel
            std::map<int, cv::Mat> blocks;
            for (int i = 0; i < INPUT_IMAGE_HEIGHT; i += BLOCK_HEIGHT) {
                for (int j = 0; j < INPUT_IMAGE_WIDTH; j += BLOCK_WIDTH) {
                    cv::Rect roi(j, i, BLOCK_WIDTH, BLOCK_HEIGHT);
                    int block_idx = (i / BLOCK_HEIGHT) * BLOCKS_PER_ROW + j / BLOCK_WIDTH;
                    blocks[block_idx] = std::move(p_decodedImage(roi));
                }
            }
            if (DEBUG_LEVEL >= 2)
            {
                std::cout << "Split ok\n";
            }

            std::vector<cv::Mat> debayered_blocks(DEBAYERING_BLOCK_NUMBER);
            std::vector<std::thread> threads(DEBAYERING_BLOCK_NUMBER);

            // Submit tasks to the thread pool to debayer the image in parallel
            for (int i = 0; i < DEBAYERING_BLOCK_NUMBER; ++i) {
                threads[i] = std::thread(debayerHelper, std::ref(blocks[i]), std::ref(debayered_blocks[i]));
            }

            // Wait for all tasks to finish
            for (auto& thread : threads) {
                thread.join();
            }

            if (DEBUG_LEVEL >= 2)
            {
                std::cout << "debayer ok\n";
            }

            // Combine the debayered blocks into a single image
            cv::Mat debayered(INPUT_IMAGE_HEIGHT, INPUT_IMAGE_WIDTH, CV_8UC3);
            for (int i = 0; i < INPUT_IMAGE_HEIGHT; i += BLOCK_HEIGHT) {
                for (int j = 0; j < INPUT_IMAGE_WIDTH; j += BLOCK_WIDTH) {
                    cv::Rect roi(j, i, BLOCK_WIDTH, BLOCK_HEIGHT);
                    int block_idx = (i / BLOCK_HEIGHT) * BLOCKS_PER_ROW + j / BLOCK_WIDTH;
                    debayered_blocks[block_idx].copyTo(debayered(roi));
                }
            }
            if (DEBUG_LEVEL >= 2)
            {
                std::cout << "combine ok\n";
            }

#if (DO_COLOR_CORRECTION==1)
            // Decrease the amount of green
            debayered -= cv::Scalar(0, 50, 0);  // Adjust the green channel values

            // Increase the brightness
            cv::convertScaleAbs(debayered, debayered, 1.2, 10);  // Adjust the brightness and contrast
#endif

            return debayered;
        }
    }
    using namespace func_debayering;

    namespace func_networking
    {
        udp::endpoint sendHandshake(ba::io_context& io, const std::string& host, udp::socket& socket)
        {
            udp::resolver resolver{ io };
            udp::endpoint destination = *resolver.resolve(udp::v4(), host, PORT_STR).begin();

            // print current time to synchronise the clocks
            //std::cout << "Initial time C: " << sps::getTimeUs() << "\n";

            // get time of REQ
            std::string timeStringStart{ sps::getTimeUs() };

            // send REQ
            std::array<unsigned char, 100> sendData{};
            std::copy(timeStringStart.begin(), timeStringStart.end(), sendData.data());
            socket.send_to(ba::buffer(&sendData, 100), destination);

            // receive ACK
            std::array<char, 100> recvData{};
            bs::error_code error;
            socket.receive_from(ba::buffer(recvData), destination, 0, error);
            if (error)
            {
                throw bs::system_error(error);
            }

            // get time of ACK
            std::string timeStringEnd{ sps::getTimeUs() };

            // calculate time difference

            std::string serverTime(std::begin(recvData), std::end(recvData));
            int rttUs{ calculateRtt(timeStringStart, timeStringEnd) };
            int timeDiffUs{ calculateTimeDiff(serverTime, timeStringEnd, rttUs) };

            std::cout << "Sent handshake at " << timeStringStart << ", received response at " << timeStringEnd << ", diff==RTT: " << rttUs << " us\n";
            std::cout << "Server sent timestamp " << serverTime << ", adjusted for RTT that gives a time difference (server+rtt-client) of: " << timeDiffUs << " us\n";

            return destination;
        }

        void receive_packets(udp::endpoint& a_endpoint, udp::socket& a_socket, int& a_incomingIndex) {
            size_t totalPacketsReceived{ 0 };
            size_t totalPacketsExpected{ 0 };
            size_t totalExpectedSize{ 0 };
            size_t totalReceivedSize{ 0 };
            bool areReceivingVarsInitialized{ false };

            while (true)
            {
                // Receive inboundPacket
                bs::error_code ec;
                size_t inboundPacketSize = a_socket.receive_from(ba::buffer(recv_buf), a_endpoint, 0, ec);
                if (ec.failed())
                {
                    std::cout << "Receiving failed with the following message: " << ec.message();
                }
                if (DEBUG_LEVEL >= 3)
                {
                    std::cout << "Received a packet of len=" << inboundPacketSize << "\n";
                }

                // Deserialize the inboundPayload
                Packet inboundPacket{};

                // Copy the header from the receive buffer into the inboundPacket object
                std::memcpy(&inboundPacket.s_header, recv_buf.data(), sizeof(PacketHeader));

                // Copy the payload from the receive buffer into the inboundPacket object
                std::vector<unsigned char> inboundPayload(recv_buf.data() + sizeof(PacketHeader), recv_buf.data() + sizeof(PacketHeader) + inboundPacket.s_header.s_payloadSize);
                inboundPayload.resize(inboundPacket.s_header.s_payloadSize);

                if (DEBUG_LEVEL >= 3)
                {
                    printPacketRaw(inboundPacketSize, &inboundPacket);
                    printPayloadRaw(&inboundPacket);
                    printHeader(&inboundPacket);
                    printPayload(&inboundPacket, inboundPayload);
                }

                // if we received a FIN packet for the current image we stop listening for new data packets
                if (inboundPacket.s_header.s_FIN && (inboundPacket.s_header.s_imageNumber == a_incomingIndex))
                {
                    if (DEBUG_LEVEL >= 2)
                    {
                        std::cout << "Only received " << totalPacketsReceived << "/" << totalPacketsExpected << " packets of image " << inboundPacket.s_header.s_imageNumber << " totaling " << totalReceivedSize << "/" << totalExpectedSize << " Bytes of payload, but a FIN packet came in - Break\n";
                    }
                    fillMissingPackets(receivedPayloads, receivedIndexes, inboundPacket.s_header, MAX_DATA_PACKET_SIZE_BYTES);
                    break;
                }

                // if we received a FIN packet for the previous image we ignore it
                if (inboundPacket.s_header.s_FIN)
                {
                    continue;
                }

                unsigned int received_data_size = inboundPacketSize - (sizeof(Packet) - sizeof(unsigned char));
                if (received_data_size != inboundPacket.s_header.s_payloadSize)
                {
                    if (DEBUG_LEVEL >= 2)
                    {
                        std::cout << "Received a packet of size=" << inboundPacketSize << " meaning packet size - (sizeof(Packet) + sizeof(const char))=" << inboundPacketSize << "-" << (sizeof(Packet) - sizeof(unsigned char)) << "=" << received_data_size << ", but the header.s_payloadSize=" << inboundPacket.s_header.s_payloadSize << "\n";
                    }
                }

                // if we received a data packet for the previous image we ignore it
                if (inboundPacket.s_header.s_imageNumber < static_cast<uint32_t>(a_incomingIndex))
                {
                    continue;
                }

                // if we received a data packet for the next image it means we probably missed all 3 FIN packets and we need to reset the receiving loop
                if (inboundPacket.s_header.s_imageNumber > static_cast<uint32_t>(a_incomingIndex))
                {
                    if (DEBUG_LEVEL >= 2)
                    {
                        std::cout << "Expected an image number=" << a_incomingIndex << " but received=" << inboundPacket.s_header.s_imageNumber << ". Receive loop is out of sync? - Resetting loop\n";
                    }
                    a_incomingIndex = inboundPacket.s_header.s_imageNumber;
                    areReceivingVarsInitialized = false;
                }

                // If this is our first inboundPacket also extract the extra header information
                if (!areReceivingVarsInitialized)
                {
                    receivedPayloads.resize(inboundPacket.s_header.s_totalPackets);
                    receivedIndexes.resize(inboundPacket.s_header.s_totalPackets);

                    receivedIndexes.assign(receivedIndexes.size(), false);

                    totalExpectedSize = inboundPacket.s_header.s_totalSizeBytes;
                    totalPacketsExpected = inboundPacket.s_header.s_totalPackets;

                    totalReceivedSize = 0;
                    totalPacketsReceived = 0;

                    areReceivingVarsInitialized = true;
                }

                // Ignore inboundPacket if it's out of range or already receivedIndexes
                if (inboundPacket.s_header.s_sequenceNumber >= totalPacketsExpected || receivedIndexes[inboundPacket.s_header.s_sequenceNumber]) {
                    if (DEBUG_LEVEL >= 2)
                    {
                        std::cout << "Received a inboundPacket with inboundPacket number=" << inboundPacket.s_header.s_sequenceNumber << "/" << totalPacketsExpected << " (duplicate=" << receivedIndexes[inboundPacket.s_header.s_sequenceNumber] << ")\n";
                    }
                    continue;
                }

                // Store the payload at the index in the header and mark it as received
                receivedPayloads[inboundPacket.s_header.s_sequenceNumber] = inboundPayload;
                receivedIndexes[inboundPacket.s_header.s_sequenceNumber] = true;

                // Increment the received byte counter as a second check
                totalReceivedSize += inboundPayload.size();
                totalPacketsReceived++;

                // Check if all packets receivedIndexes
                bool all_received = std::all_of(receivedIndexes.begin(), receivedIndexes.end(), [](bool b) { return b; });
                if (all_received) {
                    if (DEBUG_LEVEL >= 1)
                    {
                        std::cout << "Received " << totalPacketsReceived << "/" << totalPacketsExpected << " packets of image " << inboundPacket.s_header.s_imageNumber << " totaling " << totalReceivedSize << "/" << totalExpectedSize << " Bytes of payload\n";
                    }
                    break;
                }
            }
            concatenateFragments(receivedPayloads, a_incomingIndex);

            return;
        }
    }
    using namespace func_networking;


    void threadFuncProcesAndDisplayImage()
    {
        std::vector<unsigned char>* data{ nullptr };  //receivedImage
        int imageNumber{ 0 };   //absolute index of the received image
        bool topLeftSquareWasBlack{ false };

        while (runThreads)
        {
            int readyBufferIndex{ bufferManager.getAndLockLastFilledBuffer() };
            while (readyBufferIndex == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                readyBufferIndex = bufferManager.getAndLockLastFilledBuffer();
            }
            switch (readyBufferIndex)
            {
            case 1:
                data = &receivedImage1;
                imageNumber = receivedImage1index;
                break;
            case 2:
                data = &receivedImage2;
                imageNumber = receivedImage2index;
                break;
            case 3:
                data = &receivedImage3;
                imageNumber = receivedImage3index;
                break;
            default:
                assert(false && "We shouldn't be able to get here");
                break;
            }

            if (data == nullptr)
            {
                std::cout << "data received a nullptr - receivedImage1, 2 or 3 wasn't initialized. Exiting\n";
                exit(1);
            }

            if (data->empty())
            {
                std::cout << "displayImage called with empty image\n";
            }

#if (ENCODE_AS_JPEG == 0)
            // when there is no encoding at all
            cv::Mat decodedImage(INPUT_IMAGE_HEIGHT, INPUT_IMAGE_WIDTH, CV_8UC1, data->data());
#else
            // when JPEG decoding is done
            cv::Mat decodedImage;
            try
            {
#if (INPUT_IMAGE_TYPE != 3)
                // decode the image using imdecode
                decodedImage = std::move(cv::imdecode(*data, cv::IMREAD_UNCHANGED));
#else
                decodedImage = std::move(cv::imdecode(data, cv::IMREAD_GRAYSCALE));   //for when I receive rgb24, I can convert to grayscale and redo debayering if I want
#endif
            }
            catch (const std::exception& e)
            {
                std::cerr << "ln321 cv::imdecode failed with: " << e.what() << "\n";
            }
#endif

            if (decodedImage.empty())
            {
                if (DEBUG_LEVEL >= 2)
                {
                    std::cout << "cv::imdecode returned an empty image\n";
                }
                return;
            }



#if (DEBAYER_RECEIVED_IMAGE == 0)
            // don't debayer, just show
#if (DISPLAY_IMAGE == 1)
            cv::imshow("Received Image", decodedImage);
            char c = cv::waitKey(1);
            if (c == 27) //esc
            {
                runThreads = false;
            }

#if (PRINT_SWAP)
            if (isTopSquareBlack(decodedImage) && !topLeftSquareWasBlack)
            {
                std::cout << "swap to black C: " << getTimeUs() << " on image #" << imageNumber << " (decoded =" << numberOfDecodedImages + 1 << ")\n";
                topLeftSquareWasBlack = true;
            }
            else if (!isTopSquareBlack(decodedImage) && topLeftSquareWasBlack)
            {
                std::cout << "swap to white C: " << getTimeUs() << " on image #" << imageNumber << " (decoded =" << numberOfDecodedImages + 1 << ")\n";
                topLeftSquareWasBlack = false;
            }
#endif
#endif
#else
            // debayer the image - this can throw "Error: Assertion failed (0 <= roi.x && 0 <= roi.width..." occasionaly
            cv::Mat color_image;

#if (DEBAYER_METHOD == 12)
            // Convert cv::Mat to cv::UMat
            cv::UMat inputUMat;
            decodedImage.copyTo(inputUMat);

            // Debayer the image
            cv::UMat debayeredUMat;
#endif

            try
            {
#if (DEBAYER_METHOD == 1)
                // all at once
                cv::cvtColor(decodedImage, color_image, cv::COLOR_BayerBG2BGR);
#elif (DEBAYER_METHOD == 12)
                // all at once
                cv::cvtColor(inputUMat, debayeredUMat, cv::COLOR_BayerBG2BGR);

                if (debayeredUMat.empty())
                {
                    if (DEBUG_LEVEL >= 2)
                    {
                        std::cout << "cv::cvtColor returned an empty image debayeredUMat\n";
                    }
                    return;
                }
#elif (DEBAYER_METHOD == 2)
                // generic
                //cv::Mat color_image{ std::move(debayerImageGeneric(decodedImage)) };
                color_image = std::move(debayerImageGeneric(decodedImage));
#elif (DEBAYER_METHOD == 3)
                // boost pool post
                //cv::Mat color_image{ std::move(debayerImageBoost(decodedImage)) };
                color_image = std::move(debayerImageBoost(decodedImage));
#elif (DEBAYER_METHOD == 4)
                // tbb parallel for with arena
                // create a color image with the same size and type as the bayer image
                //cv::Mat color_image{ std::move(debayerImageTbbArena(decodedImage)) };
                color_image = std::move(debayerImageTbbArena(decodedImage));
#endif
            }
            catch (const std::exception& e)
            {
                std::cerr << "ln341 cv::debayer failed with: " << e.what() << "\n";
            }

            try
            {
#if (DEBAYER_METHOD == 12)
                if (debayeredUMat.empty())
#else
                if (color_image.empty())
#endif
                {
                    if (DEBUG_LEVEL >= 2)
                    {
                        std::cout << "cv::cvtColor returned an empty image\n";
                    }
                    return;
                }
                else
                {
#if (DISPLAY_IMAGE == 1)
                    // display the image
#if (DEBAYER_METHOD == 12)
                    cv::imshow("Received Image", debayeredUMat);
#else
                    cv::imshow("Received Image", color_image);
#endif
                    char c = cv::waitKey(1);
                    if (c == 27) //esc
                    {
                        runThreads = false;
                    }
#endif
                }

            }
            catch (const std::exception& e)
            {
                std::cerr << "ln782 cv::imshow failed with: " << e.what() << "\n";
            }

#if (PRINT_SWAP)
            if (isTopSquareBlack(color_image) && !topLeftSquareWasBlack)
            {
                std::cout << "swap to black C: " << getTimeUs() << " on image #" << imageNumber << " (decoded =" << numberOfDecodedImages + 1 << ")\n";
                topLeftSquareWasBlack = true;
            }
            else if (!isTopSquareBlack(color_image) && topLeftSquareWasBlack)
            {
                std::cout << "swap to white C: " << getTimeUs() << " on image #" << imageNumber << " (decoded =" << numberOfDecodedImages + 1 << ")\n";
                topLeftSquareWasBlack = false;
            }
#endif
#endif
            numberOfDecodedImages++;
            bufferManager.unlockLastLockedBuffer();
        }
    }


    void threadFuncReceive(udp::socket& socket, udp::endpoint& sender)
    {
        int droppedFrameCounter{ 0 };
        long long msCounter{ getMillisecondsSinceLastCall() };
        int incomingMessageCounter{ 0 };

        while (runThreads)
        {
            int expectedIncomingMessageCounter{ incomingMessageCounter };
            // receive image
            receive_packets(sender, socket, incomingMessageCounter);

            int receivedIndexDifference = incomingMessageCounter - expectedIncomingMessageCounter;
            if (receivedIndexDifference > 0)
            {
                if (DEBUG_LEVEL >= 1)
                {
                    std::cout << "Expected image " << expectedIncomingMessageCounter << " but received " << incomingMessageCounter << ": lost " << receivedIndexDifference << " frames\n";
                }
                droppedFrameCounter += receivedIndexDifference;
            }

            if (DEBUG_LEVEL >= 1)
            {
                std::cout << "Received image no. " << incomingMessageCounter << " at " << getTimeUs() << "\n\n";
            }
            incomingMessageCounter++;
            expectedIncomingMessageCounter = incomingMessageCounter;

#if (ACTIVE_DISPLAY_PERF_STATS == 1)
            msCounter += getMillisecondsSinceLastCall();
            printPerfStats(droppedFrameCounter, incomingMessageCounter, msCounter);
#endif
        }
        msCounter += getMillisecondsSinceLastCall();
        printPerfStats(droppedFrameCounter, incomingMessageCounter, msCounter);

        return;
    }


    void client(ba::io_context& io, const std::string& host)
    {
        // open client socket
#if (USE_FIXED_PORT == 0)
        udp::socket socket{ io };
        socket.open(udp::v4());
#else
        udp::socket socket{ io, udp::endpoint{udp::v4(), PORT + 1} };
#endif

        std::cout << "Client connecting to " << HOST_IP << ":" << PORT << "\n\n";

        // send a handshake
        udp::endpoint destination = sendHandshake(io, host, socket);
        std::cout << "Server found at: " << destination << "\n";
        udp::endpoint sender;   // info about the server

        // start the receiving and processing thread
        receivedImage1.reserve(MAX_IMAGE_SIZE_BYTES); // Reserve memory to avoid reallocations
        receivedImage2.reserve(MAX_IMAGE_SIZE_BYTES);
        receivedImage3.reserve(MAX_IMAGE_SIZE_BYTES);


        std::thread threadReceive(
            [&socket, &sender]()
            {
                threadFuncReceive(std::ref(socket), std::ref(sender));
            });
        std::thread threadProcesAndDisplayImage(threadFuncProcesAndDisplayImage);

        std::cout << "Receiving, decoding, debayering and displaying images from the UDP server\n";

        // run until user input
        char stopClientThreads{ 'a' };
        std::cout << "Click on the image window and pres 'ESC' or type something besides 'a' to stop the receive and processDisplay threads\n";
        while (runThreads == true && stopClientThreads == 'a')
        {
            std::cin >> stopClientThreads;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        // cleanup
        runThreads = false;
        threadReceive.join();
        threadProcesAndDisplayImage.join();

        return;
    }
}

int main()
{
    ba::io_context io;

    std::cout << "Choose the type: 1 - client: ";
    int input{ -1 };
    std::cin >> input;

    if (input == 1)
    {
        sps::client(io, HOST_IP);
    }

}
