#include <esp_now.h>
#include <WiFi.h>
#include <VescUart.h>

// Configuration
namespace Config {
    constexpr uint8_t PPM_PIN = 22;
    constexpr uint8_t CONNECTION_LED = 2;
    constexpr uint8_t MODE_LEDS[] = {5, 18, 19};
    constexpr unsigned long TIMEOUT = 1000; // ms
    constexpr uint16_t PPM_IDLE = 600;      // Idle PPM value
    constexpr uint16_t PPM_DEFAULT = 600;   // Default PPM when throttle is low
    constexpr uint16_t PPM_MODE_MAX[] = {1100, 1200, 1260}; // Max PPM per mode
    constexpr uint8_t WIFI_CHANNEL = 1;
}

struct _attribute_((packed)) StatusData {
    uint16_t throttleValue; // 0-1000
    uint8_t mode;
};

class EBikeController {
private:
    StatusData status{0, 1};
    bool isConnected = false;
    unsigned long lastDataTime = 0;
    VescUart vescUart;
    uint16_t lastPPM = Config::PPM_DEFAULT;

    void sendPPMSignal(uint16_t ppmValue) {
        // Ensure PPM is within valid range
        if (ppmValue < 500 || ppmValue > 2000) {
            ppmValue = Config::PPM_DEFAULT;
            Serial.printf("Invalid PPM %d, using default\n", ppmValue);
        }
        // Generate PPM signal
        digitalWrite(Config::PPM_PIN, HIGH);
        delayMicroseconds(ppmValue);
        digitalWrite(Config::PPM_PIN, LOW);
        // Maintain consistent frame timing (20ms typical)
        delayMicroseconds(20000 - ppmValue);
    }

    uint16_t calculatePPM() {
        if (!isConnected || status.throttleValue == 0) {
            return Config::PPM_DEFAULT;
        }
        uint8_t modeIndex = constrain(status.mode - 1, 0, 2);
        uint16_t maxPPM = Config::PPM_MODE_MAX[modeIndex];
        // Map throttle (0-1000) to PPM range (600 to maxPPM)
        return map(status.throttleValue, 0, 1000, Config::PPM_DEFAULT, maxPPM);
    }

public:
    bool initialize() {
        Serial.begin(115200);
        delay(1000);

        // Initialize pins
        pinMode(Config::PPM_PIN, OUTPUT);
        pinMode(Config::CONNECTION_LED, OUTPUT);
        for (auto pin : Config::MODE_LEDS) pinMode(pin, OUTPUT);

        // LED startup sequence
        digitalWrite(Config::CONNECTION_LED, HIGH);
        for (auto pin : Config::MODE_LEDS) digitalWrite(pin, HIGH);
        delay(500);
        digitalWrite(Config::CONNECTION_LED, LOW);
        for (auto pin : Config::MODE_LEDS) digitalWrite(pin, LOW);

        // Initialize WiFi and ESP-NOW
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        if (esp_now_init() != ESP_OK) {
            Serial.println("ESP-NOW init failed");
            return false;
        }

        // Register receive callback
        esp_now_register_recv_cb([](const esp_now_recv_info* recv_info, const uint8_t* data, int len) {
            if (len == sizeof(StatusData)) {
                memcpy(&controller.status, data, sizeof(StatusData));
                controller.lastDataTime = millis();
                controller.isConnected = true;
                Serial.printf("Received - Throttle: %d, Mode: %d\n",
                             controller.status.throttleValue, controller.status.mode);
            }
        });

        // Initialize VESC
        vescUart.setSerialPort(&Serial);
        return true;
    }

    void update() {
        unsigned long now = millis();

        // Check connection status
        if (now - lastDataTime > Config::TIMEOUT) {
            if (isConnected) {
                Serial.println("Connection lost");
                isConnected = false;
                digitalWrite(Config::CONNECTION_LED, LOW);
                status.throttleValue = 0;
                status.mode = 1;
            }
            // Blink LED when disconnected
            digitalWrite(Config::CONNECTION_LED, (now / 300) % 2);
        } else {
            digitalWrite(Config::CONNECTION_LED, HIGH);
        }

        // Update mode LEDs
        for (uint8_t i = 0; i < 3; i++) {
            digitalWrite(Config::MODE_LEDS[i], status.mode == (i + 1));
        }

        // Calculate and smooth PPM
        uint16_t targetPPM = calculatePPM();
        // Smooth transitions to prevent ESC cutoff
        lastPPM = (lastPPM * 9 + targetPPM) / 10;
        sendPPMSignal(lastPPM);

        // Debug output
        static unsigned long lastDebug = 0;
        if (now - lastDebug > 500) {
            Serial.printf("PPM: %d, Mode: %d, Connected: %d\n",
                         lastPPM, status.mode, isConnected);
            lastDebug = now;
        }

        // Debug VESC data
        if (vescUart.getVescValues()) {
            static unsigned long lastVescDebug = 0;
            if (now - lastVescDebug > 1000) {
                Serial.printf("VESC - Voltage: %.1fV, RPM: %ld\n",
                             vescUart.data.inpVoltage, vescUart.data.rpm);
                lastVescDebug = now;
            }
        }
    }

    static EBikeController controller;
};

EBikeController EBikeController::controller;

void setup() {
    if (!EBikeController::controller.initialize()) {
        Serial.println("Init failed, restarting...");
        delay(2000);
        ESP.restart();
    }
}

void loop() {
    EBikeController::controller.update();
}