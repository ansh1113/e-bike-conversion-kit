#include <esp_now.h>
#include <WiFi.h>

// Configuration
namespace Config {
    constexpr uint8_t THROTTLE_PIN = A0;
    constexpr uint8_t INCREMENT_PIN = D3;
    constexpr uint8_t DECREMENT_PIN = D1;
    constexpr uint8_t LED_PIN = 43;
    constexpr uint8_t MAX_MODE = 3;
    constexpr uint8_t MIN_MODE = 1;
    constexpr unsigned long DEBOUNCE_DELAY = 200;
    constexpr unsigned long SEND_INTERVAL = 20; // ms, increased frequency
    constexpr uint16_t THROTTLE_MIN = 1000;     // Adjusted after calibration
    constexpr uint16_t THROTTLE_MAX = 3200;    // Adjusted after calibration
    constexpr uint16_t THROTTLE_DEADBAND = 50; // Deadband for idle
    const uint8_t RECEIVER_MAC[] = {0x2C, 0xBC, 0xBB, 0x0D, 0xFF, 0x88};
}

struct _attribute_((packed)) StatusData {
    uint16_t throttleValue; // Scaled 0-1000 for simplicity
    uint8_t mode;
};

class ThrottleController {
private:
    StatusData status{0, Config::MIN_MODE};
    unsigned long lastModeChangeTime = 0;
    unsigned long lastSendTime = 0;
    bool lastIncrementState = HIGH;
    bool lastDecrementState = HIGH;
    bool isConnected = false;

    esp_err_t sendData() {
        esp_err_t result = esp_now_send(Config::RECEIVER_MAC, 
                                       reinterpret_cast<const uint8_t*>(&status), 
                                       sizeof(StatusData));
        if (result == ESP_OK) {
            isConnected = true;
            lastSendTime = millis();
            Serial.println("Data sent successfully");
        } else {
            isConnected = false;
            Serial.println("Send failed");
        }
        return result;
    }

    void updateMode(bool increment) {
        if (millis() - lastModeChangeTime < Config::DEBOUNCE_DELAY) return;
        uint8_t newMode = status.mode + (increment ? 1 : -1);
        if (newMode >= Config::MIN_MODE && newMode <= Config::MAX_MODE) {
            status.mode = newMode;
            lastModeChangeTime = millis();
            Serial.printf("Mode changed to %d\n", status.mode);
        }
    }

    uint16_t readThrottle() {
        uint16_t rawValue = analogRead(Config::THROTTLE_PIN);
        // Apply deadband and map to 0-1000
        if (rawValue < Config::THROTTLE_MIN + Config::THROTTLE_DEADBAND) {
            return 0;
        }
        rawValue = constrain(rawValue, Config::THROTTLE_MIN, Config::THROTTLE_MAX);
        return map(rawValue, Config::THROTTLE_MIN, Config::THROTTLE_MAX, 0, 1000);
    }

public:
    bool initialize() {
        Serial.begin(115200);
        WiFi.mode(WIFI_STA);
        if (esp_now_init() != ESP_OK) {
            Serial.println("ESP-NOW init failed");
            return false;
        }

        esp_now_peer_info_t peerInfo{};
        memcpy(peerInfo.peer_addr, Config::RECEIVER_MAC, 6);
        peerInfo.channel = 1;
        peerInfo.encrypt = false;
        if (esp_now_add_peer(&peerInfo) != ESP_OK) {
            Serial.println("Peer add failed");
            return false;
        }

        // Register send callback for debugging
        esp_now_register_send_cb([](const uint8_t* mac, esp_now_send_status_t status) {
            Serial.printf("Send CB: %s\n", status == ESP_NOW_SEND_SUCCESS ? "Success" : "Fail");
        });

        return true;
    }

    void update() {
        // Read buttons
        bool incrementPressed = !digitalRead(Config::INCREMENT_PIN);
        bool decrementPressed = !digitalRead(Config::DECREMENT_PIN);

        if (incrementPressed && !lastIncrementState) {
            updateMode(true);
        }
        if (decrementPressed && !lastDecrementState) {
            updateMode(false);
        }
        lastIncrementState = incrementPressed;
        lastDecrementState = decrementPressed;

        // Read and smooth throttle
        uint16_t newThrottle = readThrottle();
        // Simple exponential moving average to prevent sudden jumps
        status.throttleValue = (status.throttleValue * 7 + newThrottle * 3) / 10;

        // Send data regularly
        if (millis() - lastSendTime >= Config::SEND_INTERVAL) {
            sendData();
        }

        // Debug output
        static unsigned long lastDebug = 0;
        if (millis() - lastDebug >= 500) {
            Serial.printf("Throttle: %d, Mode: %d, Connected: %d\n",
                         status.throttleValue, status.mode, isConnected);
            lastDebug = millis();
        }
    }
};

ThrottleController controller;

void setup() {
    pinMode(Config::INCREMENT_PIN, INPUT_PULLUP);
    pinMode(Config::DECREMENT_PIN, INPUT_PULLUP);
    pinMode(Config::LED_PIN, OUTPUT);
    digitalWrite(Config::LED_PIN, HIGH);

    if (!controller.initialize()) {
        Serial.println("Init failed, restarting...");
        delay(2000);
        ESP.restart();
    }
}

void loop() {
    controller.update();
    delay(50);
}