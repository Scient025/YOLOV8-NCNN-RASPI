#include <zmq.hpp>
#include <iostream>
#include <cstring>
#include <vector>
#include <sstream>
#include <iomanip> // Necessary for std::fixed and std::setprecision

// --- Data Structures ---
// Ensure the struct is tightly packed for network transmission
#pragma pack(push, 1)

struct Person {
    float cx, cy;      // 8 bytes: Center X, Center Y (Normalized coordinates)
};

struct FramePacketHeader {
    uint8_t cam_id;    // 1 byte: Identifier for the camera
    uint8_t count;     // 1 byte: Number of detections (People) in the packet
    uint64_t ts_ms;    // 8 bytes: Timestamp in milliseconds
};                     // TOTAL = 10 bytes

#pragma pack(pop)

// --- Configuration (6 Total ZeroMQ Endpoints) ---
// Define all six ZMQ endpoints based on your latest port configuration.
const std::vector<std::pair<std::string, int>> ZMQ_ENDPOINTS = {
    {"192.168.0.186", 5554}, // Pi 1, Cam 1 Detections
    {"192.168.0.186", 5555}, // Pi 1, Cam 2 Detections
    {"192.168.0.16", 5556},  // Pi 2, Cam 3 Detections
    {"192.168.0.16", 5557},  // Pi 2, Cam 4 Detections
    {"192.168.0.158", 5558}, // Pi 3, Cam 5 Detections
    {"192.168.0.158", 5559}  // Pi 3, Cam 6 Detections
};

const size_t NUM_ENDPOINTS = ZMQ_ENDPOINTS.size();

// --- Main Function ---
int main() {
    zmq::context_t context(1);
    
    // Vector to hold the six subscriber sockets
    std::vector<zmq::socket_t> subscribers;
    // Array of poll items for efficient, non-blocking multiplexing
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
        items[i].events = ZMQ_POLLIN; // Listen for incoming data
        items[i].revents = 0;
    }
    
    std::cout << "\n--- Live Aggregated Subscriber Output (Ctrl+C to stop) ---\n";

    const size_t HEADER_SIZE = sizeof(FramePacketHeader);
    const size_t PERSON_SIZE = sizeof(Person);

    try {
        while (true) {
            // Poll all sockets with a timeout of 100 milliseconds
            // This prevents blocking indefinitely on a single socket.
            int rc = zmq::poll(items, NUM_ENDPOINTS, 100); 

            if (rc > 0) { // If there is incoming data on one or more sockets
                // Check each socket to see which one has data
                for (size_t i = 0; i < NUM_ENDPOINTS; ++i) {
                    if (items[i].revents & ZMQ_POLLIN) {
                        zmq::message_t message;
                        
                        // Receive message without blocking
                        if (!subscribers[i].recv(message, zmq::recv_flags::dontwait)) {
                            continue; 
                        }
                        
                        // Correctly cast the message data to a pointer
                        const uint8_t* data = static_cast<const uint8_t*>(message.data());
                        size_t msg_size = message.size();

                        if (msg_size < HEADER_SIZE) {
                            std::cerr << "WARN: Message too small on endpoint " << i << std::endl;
                            continue;
                        }

                        // Unpack Header
                        FramePacketHeader header;
                        memcpy(&header, data, HEADER_SIZE);

                        // Validate packet size
                        size_t expected_size = HEADER_SIZE + header.count * PERSON_SIZE;
                        if (msg_size != expected_size) {
                            std::cerr << "WARN: Size mismatch on Cam " << (int)header.cam_id 
                                      << "! Expected " << expected_size << " bytes, but got " 
                                      << msg_size << std::endl;
                            continue;
                        }

                        // --- Print Aggregated Detections ---
                        // Use std::cout for the aggregated output, matching the single-camera style
                        std::cout << "CAM " << (int)header.cam_id 
                                  << " (Port " << ZMQ_ENDPOINTS[i].second 
                                  << ", T=" << header.ts_ms << "ms): " 
                                  << (int)header.count << " Detections: ";

                        if (header.count > 0) {
                            // Correctly cast the remaining data to a Person pointer array
                            const Person* persons = reinterpret_cast<const Person*>(data + HEADER_SIZE);
                            for (uint8_t j = 0; j < header.count; j++) {
                                // Print coordinates with 3 decimal places for clarity
                                std::cout << "Center=(" << std::fixed << std::setprecision(3) 
                                          << persons[j].cx << ", " << persons[j].cy << ")";
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
        std::cerr << "Standard Error: " << e.what() << std::endl;
    }

    std::cout << "\nZeroMQ Subscriber terminated.\n";
    return 0;
}