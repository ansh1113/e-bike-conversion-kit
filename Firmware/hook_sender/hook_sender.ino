//esp32 devkit v1
#include <esp_now.h>
#include <WiFi.h>

// Configuration
namespace Config {
    constexpr uint8_t ACCELERATOR_PIN = 19;
    constexpr uint8_t INCREMENT_PIN = 5;
    constexpr uint8_t DECREMENT_PIN = 23;
    constexpr uint8_t MAX_MODE = 3;
    constexpr uint8_t MIN_MODE = 1;
    constexpr unsigned long DEBOUNCE_DELAY = 300;
    constexpr unsigned long RETRY_DELAY = 1000;
    constexpr uint8_t MAX_CONNECTION_ATTEMPTS = 10;
    
    // Replace with actual receiver MAC
    const uint8_t RECEIVER_MAC[] = {0x2C, 0xBC, 0xBB, 0x0D, 0xFF, 0x88};
}

struct __attribute__((packed)) StatusData {
    bool acceleratorPressed;
    uint8_t mode;
};

class ThrottleController {
private:
    StatusData status{false, 1};
    unsigned long lastModeChangeTime = 0;
    bool lastIncrementState = HIGH;
    bool lastDecrementState = HIGH;
    bool isConnected = false;
    unsigned long lastSendTime = 0;
    const unsigned long SEND_INTERVAL = 100;  // Send updates every 100ms

    esp_err_t sendData(const StatusData& data) {
        if (millis() - lastSendTime < SEND_INTERVAL) {
            return ESP_OK;  // Skip sending if too soon
        }
        
        esp_err_t result = esp_now_send(Config::RECEIVER_MAC, 
                                      reinterpret_cast<const uint8_t*>(&data), 
                                      sizeof(StatusData));
        
        if (result == ESP_OK) {
            lastSendTime = millis();
            isConnected = true;
        } else {
            Serial.println("Send failed!");
            isConnected = false;
        }
        return result;
    }

    void updateMode(bool increment) {
        if (millis() - lastModeChangeTime <= Config::DEBOUNCE_DELAY) return;

        uint8_t newMode = status.mode + (increment ? 1 : -1);
        if (newMode >= Config::MIN_MODE && newMode <= Config::MAX_MODE) {
            status.mode = newMode;
            lastModeChangeTime = millis();
            sendData(status);
            Serial.printf("Mode %s to: %d\n", 
                        increment ? "Incremented" : "Decremented", 
                        status.mode);
        } else {
            Serial.printf("Mode already at %s (%d)\n", 
                        increment ? "maximum" : "minimum",
                        status.mode);
        }
    }

public:
    bool initialize() {
        Serial.println("\n\n=== E-Bike Throttle Controller Starting ===");
        Serial.println("Version: 1.0");
        Serial.println("Initializing components...");

        Serial.println("Setting up WiFi in Station mode...");
        WiFi.mode(WIFI_STA);

        Serial.println("Initializing ESP-NOW...");
        if (esp_now_init() != ESP_OK) {
            Serial.println("ESP-NOW initialization failed!");
            return false;
        }

        // Register callback for delivery status
        esp_now_register_send_cb([](const uint8_t* mac, esp_now_send_status_t status) {
            Serial.printf("Last Packet Delivery Status: %s\n", 
                         status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
        });

        Serial.println("Adding peer device...");
        esp_now_peer_info_t peerInfo{};
        memcpy(peerInfo.peer_addr, Config::RECEIVER_MAC, 6);
        
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("Failed to add peer!");
            return false;
        }

        Serial.println("Initialization complete!");
        return waitForConnection();
    }

    bool waitForConnection() {
        Serial.println("Attempting to connect to receiver...");
        
        for (uint8_t attempts = 0; attempts < Config::MAX_CONNECTION_ATTEMPTS; attempts++) {
            Serial.printf("Connection attempt %d/%d...\n", 
                         attempts + 1, Config::MAX_CONNECTION_ATTEMPTS);
                         
            if (esp_now_is_peer_exist(Config::RECEIVER_MAC) && 
                sendData(status) == ESP_OK) {
                Serial.println("Successfully connected to receiver!");
                return true;
            }
            
            delay(Config::RETRY_DELAY);
        }
        
        Serial.println("Failed to connect to receiver after maximum attempts!");
        return false;
    }

    void update() {
        bool currentAccState = !digitalRead(Config::ACCELERATOR_PIN);
        bool incrementPressed = !digitalRead(Config::INCREMENT_PIN);
        bool decrementPressed = !digitalRead(Config::DECREMENT_PIN);

        // Handle accelerator changes
        if (status.acceleratorPressed != currentAccState) {
            status.acceleratorPressed = currentAccState;
            sendData(status);
            Serial.printf("Accelerator: %s\n", 
                        status.acceleratorPressed ? "ON" : "OFF");
        }

        // Handle mode changes
        if (incrementPressed && !decrementPressed && !lastIncrementState) {
            updateMode(true);
        } else if (decrementPressed && !incrementPressed && !lastDecrementState) {
            updateMode(false);
        }

        lastIncrementState = incrementPressed;
        lastDecrementState = decrementPressed;

        // Send periodic updates
        if (millis() - lastSendTime >= SEND_INTERVAL) {
            sendData(status);
        }
    }
};

ThrottleController controller;

void setup() {
    Serial.begin(115200);
    delay(1000);  // Give serial time to initialize
    
    // Configure GPIO pins
    pinMode(Config::ACCELERATOR_PIN, INPUT_PULLUP);
    pinMode(Config::INCREMENT_PIN, INPUT_PULLUP);
    pinMode(Config::DECREMENT_PIN, INPUT_PULLUP);
    
    Serial.println("GPIO pins configured");

    if (!controller.initialize()) {
        Serial.println("Initialization failed! Restarting...");
        delay(1000);
        ESP.restart();
    }
}

void loop() {
    controller.update();
    delay(50);  // Stable sampling rate
}