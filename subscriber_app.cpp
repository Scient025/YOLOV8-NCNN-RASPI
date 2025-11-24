#include <zmq.hpp>
#include <iostream>
#include <cstring>
#include <vector>
#include <sstream>

// --- Data Structures ---
#pragma pack(push, 1)

struct Person {
    float cx, cy;      // 8 bytes: 2 floats
};

struct FramePacketHeader {
    uint8_t cam_id;    // 1 byte
    uint8_t count;     // 1 byte
    uint64_t ts_ms;    // 8 bytes
};                     // TOTAL = 10 bytes

#pragma pack(pop)

// --- Configuration (6 Total ZeroMQ Endpoints) ---
// Each element is a pair of {IP_ADDRESS, PORT}
const std::vector<std::pair<std::string, int>> ZMQ_ENDPOINTS = {
    {"192.168.0.186", 5554},
    {"192.168.0.186", 5555},
    {"192.168.0.16", 5556},
    {"192.168.0.16", 5557},
    {"192.168.0.158", 5558},
    {"192.168.0.158", 5559}
};

const size_t NUM_ENDPOINTS = ZMQ_ENDPOINTS.size();

// --- Main Function ---
int main() {
    zmq::context_t context(1);
    
    // We will use a vector to hold the socket objects
    std::vector<zmq::socket_t> subscribers;
    // We will use an array of poll items for efficient multiplexing
    zmq::pollitem_t items[NUM_ENDPOINTS];
    
    // 1. Setup Subscribers
    std::cout << "👂 Aggregating ZeroMQ Subscribers across " << NUM_ENDPOINTS << " endpoints...\n";
    for (size_t i = 0; i < NUM_ENDPOINTS; ++i) {
        const auto& endpoint = ZMQ_ENDPOINTS[i];
        std::string url = "tcp://" + endpoint.first + ":" + std::to_string(endpoint.second);
        std::cout << "   -> Connecting to " << url << "\n";
        
        // Create and configure the subscriber socket
        subscribers.emplace_back(context, zmq::socket_type::sub);
        zmq::socket_t& subscriber = subscribers.back();

        subscriber.set(zmq::sockopt::linger, 0);       // Do not block on close
        subscriber.connect(url);
        subscriber.set(zmq::sockopt::subscribe, "");   // Subscribe to all messages

        // Configure the poll item
        items[i].socket = (void*)subscriber; // Cast to void* for the pollitem
        items[i].fd = 0;
        items[i].events = ZMQ_POLLIN;
        items[i].revents = 0;
    }
    
    std::cout << "\n--- Subscriber Output (Combined from 6 Streams) ---\n";

    const size_t HEADER_SIZE = sizeof(FramePacketHeader);
    const size_t PERSON_SIZE = sizeof(Person);

    try {
        while (true) {
            // Poll all sockets with a timeout of 100 milliseconds
            // rc is the number of sockets that have data
            int rc = zmq::poll(items, NUM_ENDPOINTS, 100); 

            if (rc > 0) { 
                // Check each socket to see which one has data
                for (size_t i = 0; i < NUM_ENDPOINTS; ++i) {
                    if (items[i].revents & ZMQ_POLLIN) {
                        zmq::message_t message;
                        
                        // Receive message without blocking (since poll confirmed data is there)
                        if (!subscribers[i].recv(message, zmq::recv_flags::dontwait)) {
                            continue; 
                        }
                        
                        const uint8_t* data = static_cast<const uint8_t*>(message.data());
                        size_t msg_size = message.size();

                        if (msg_size < HEADER_SIZE) {
                            std::cerr << "WARN: Message too small on endpoint " << i << std::endl;
                            continue;
                        }

                        // Unpack Header
                        FramePacketHeader header;
                        memcpy(&header, data, HEADER_SIZE);

                        size_t expected_size = HEADER_SIZE + header.count * PERSON_SIZE;
                        if (msg_size != expected_size) {
                            std::cerr << "WARN: Size mismatch on Cam " << (int)header.cam_id 
                                      << "! Expected " << expected_size << " bytes, but got " 
                                      << msg_size << std::endl;
                            continue;
                        }

                        // Process Detections
                        std::cout << "CAM " << (int)header.cam_id 
                                  << ": " << (int)header.count 
                                  << " Detections at T=" << header.ts_ms << "ms: ";

                        if (header.count > 0) {
                            const Person* persons = reinterpret_cast<const Person*>(data + HEADER_SIZE);
                            for (uint8_t j = 0; j < header.count; j++) {
                                // Print coordinates with reduced precision for clarity
                                std::cout << "(" << std::fixed << std::setprecision(3) << persons[j].cx 
                                          << ", " << persons[j].cy << ")";
                                if (j < header.count - 1) {
                                    std::cout << ", ";
                                }
                            }
                        } else {
                            std::cout << "None";
                        }
                        std::cout << "\n";
                    }
                }
            }
        }
    } catch (const zmq::error_t& e) {
        if (e.num() != ETERM) {
            std::cerr << "ZeroMQ Error: " << e.what() << std::endl;
        }
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
    }

    std::cout << "\nZeroMQ Subscriber terminated.\n";
    return 0;
}