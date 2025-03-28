#include <esp_now.h>
#include <WiFi.h>

// Configuration
namespace Config {
    constexpr uint8_t ACCELERATOR_PIN = D2;
    constexpr uint8_t INCREMENT_PIN = D3;
    constexpr uint8_t DECREMENT_PIN = D1;
    constexpr uint8_t LED_PIN = 43;
    constexpr uint8_t MAX_MODE = 3;
    constexpr uint8_t MIN_MODE = 1;
    constexpr unsigned long DEBOUNCE_DELAY = 300;
    constexpr unsigned long RETRY_DELAY = 1000;
    constexpr uint8_t MAX_CONNECTION_ATTEMPTS = 10;
    constexpr unsigned long ACCELERATION_HOLD_TIME = 1500;
    
    // Replace with actual receiver MAC
    const uint8_t RECEIVER_MAC[] = {0x2C, 0xBC, 0xBB, 0x0D, 0xFF, 0x88};
}

struct _attribute_((packed)) StatusData {
    bool acceleratorPressed;
    uint8_t mode;
};

class ThrottleController {
private:
    StatusData status{false, Config::MIN_MODE};
    unsigned long lastModeChangeTime = 0;
    unsigned long acceleratorReleaseTime = 0;
    bool lastAcceleratorState = HIGH;
    bool lastIncrementState = HIGH;
    bool lastDecrementState = HIGH;
    bool isConnected = false;
    unsigned long lastSendTime = 0;
    const unsigned long SEND_INTERVAL = 100;

    esp_err_t sendData(const StatusData& data) {
        if (millis() - lastSendTime < SEND_INTERVAL) {
            return ESP_OK;
        }
        
        esp_err_t result = esp_now_send(Config::RECEIVER_MAC, 
                                      reinterpret_cast<const uint8_t*>(&data), 
                                      sizeof(StatusData));
        
        if (result == ESP_OK) {
            lastSendTime = millis();
            isConnected = true;
        } else {
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
        }
    }

public:
    bool initialize() {
        WiFi.mode(WIFI_STA);

        if (esp_now_init() != ESP_OK) {
            return false;
        }

        esp_now_register_send_cb([](const uint8_t* mac, esp_now_send_status_t status) {});

        esp_now_peer_info_t peerInfo{};
        memcpy(peerInfo.peer_addr, Config::RECEIVER_MAC, 6);
        
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            return false;
        }

        return waitForConnection();
    }

    bool waitForConnection() {
        for (uint8_t attempts = 0; attempts < Config::MAX_CONNECTION_ATTEMPTS; attempts++) {
            if (esp_now_is_peer_exist(Config::RECEIVER_MAC) && 
                sendData(status) == ESP_OK) {
                return true;
            }
            
            delay(Config::RETRY_DELAY);
        }
        
        return false;
    }

    void update() {
        bool acceleratorPressed = !digitalRead(Config::ACCELERATOR_PIN);
        bool incrementPressed = !digitalRead(Config::INCREMENT_PIN);
        bool decrementPressed = !digitalRead(Config::DECREMENT_PIN);

        // Handle acceleration logic with 2-second hold
        if (acceleratorPressed && !lastAcceleratorState) {
            // Pressed first time or re-pressed within 2 seconds
            status.acceleratorPressed = true;
            acceleratorReleaseTime = 0;
            sendData(status);
        }
        else if (!acceleratorPressed && lastAcceleratorState) {
            // Just released
            acceleratorReleaseTime = millis();
        }
        else if (acceleratorReleaseTime > 0) {
            // Check if 2 seconds have passed since release
            if (millis() - acceleratorReleaseTime >= Config::ACCELERATION_HOLD_TIME) {
                // Check if no buttons are pressed
                if (!incrementPressed && !decrementPressed) {
                    status.acceleratorPressed = false;
                    sendData(status);
                    acceleratorReleaseTime = 0;
                }
            }
        }

        // Handle mode changes
        if (incrementPressed && !decrementPressed && !lastIncrementState) {
            updateMode(true);
        } else if (decrementPressed && !incrementPressed && !lastDecrementState) {
            updateMode(false);
        }

        lastAcceleratorState = acceleratorPressed;
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
    pinMode(Config::ACCELERATOR_PIN, INPUT_PULLUP);
    pinMode(Config::INCREMENT_PIN, INPUT_PULLUP);
    pinMode(Config::DECREMENT_PIN, INPUT_PULLUP);
    pinMode(43, OUTPUT);
    
    digitalWrite(43, HIGH);

    if (!controller.initialize()) {
        delay(1000);
        ESP.restart();
    }
}

void loop() {
    controller.update();
    delay(50);
}