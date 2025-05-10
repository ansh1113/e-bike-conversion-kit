#include <esp_now.h>
#include <WiFi.h>
#include <VescUart.h>

namespace Config {
    constexpr uint8_t PPM_PIN = 22;
    constexpr uint8_t MODE_LEDS[] = {5, 18, 19};
    constexpr unsigned long TIMEOUT = 1000; // ms
    constexpr uint16_t PPM_IDLE = 600;
    constexpr uint16_t PPM_DEFAULT = 600;
    constexpr uint16_t PPM_MODE_MAX[] = {1100, 1200, 1260};
    constexpr uint8_t WIFI_CHANNEL = 1;
    constexpr uint64_t SLEEP_DURATION_US = 5e6; // 5 seconds
}

struct __attribute__((packed)) StatusData {
    uint16_t throttleValue;
    uint8_t mode;
};

class EBikeController {
private:
    VescUart vescUart;
    uint16_t lastPPM = Config::PPM_DEFAULT;

public:
    StatusData status{0, 1};
    bool isConnected = false;
    unsigned long lastDataTime = 0;

    void sendPPMSignal(uint16_t ppmValue) {
        if (ppmValue < 500 || ppmValue > 2000) {
            ppmValue = Config::PPM_DEFAULT;
        }
        digitalWrite(Config::PPM_PIN, HIGH);
        delayMicroseconds(ppmValue);
        digitalWrite(Config::PPM_PIN, LOW);
        delayMicroseconds(20000 - ppmValue);
    }

    uint16_t calculatePPM() {
        if (!isConnected || status.throttleValue == 0)
            return Config::PPM_DEFAULT;

        uint8_t modeIndex = constrain(status.mode - 1, 0, 2);
        uint16_t maxPPM = Config::PPM_MODE_MAX[modeIndex];
        return map(status.throttleValue, 0, 1000, Config::PPM_DEFAULT, maxPPM);
    }

    bool initializePinsAndLEDs() {
        pinMode(Config::PPM_PIN, OUTPUT);
        for (auto pin : Config::MODE_LEDS) pinMode(pin, OUTPUT);

        for (auto pin : Config::MODE_LEDS) digitalWrite(pin, HIGH);
        delay(500);
        for (auto pin : Config::MODE_LEDS) digitalWrite(pin, LOW);
        return true;
    }

    bool initializeVESC() {
        vescUart.setSerialPort(&Serial);
        return true;
    }

    void update() {
        unsigned long now = millis();

        if (now - lastDataTime > Config::TIMEOUT) {
            if (isConnected) {
                isConnected = false;
                delay(1000); // Wait 1 second before sleeping
                esp_sleep_enable_timer_wakeup(Config::SLEEP_DURATION_US);
                esp_deep_sleep_start();
            }
            return;
        }

        for (uint8_t i = 0; i < 3; i++)
            digitalWrite(Config::MODE_LEDS[i], status.mode == (i + 1));

        uint16_t targetPPM = calculatePPM();
        lastPPM = (lastPPM * 9 + targetPPM) / 10;
        sendPPMSignal(lastPPM);

        vescUart.getVescValues(); // Optional monitoring
    }

    static EBikeController controller;
};

EBikeController EBikeController::controller;
bool receivedInitialData = false;

void onDataReceive(const esp_now_recv_info* recvInfo, const uint8_t* data, int len) {
    if (len == sizeof(StatusData)) {
        memcpy(&EBikeController::controller.status, data, sizeof(StatusData));
        EBikeController::controller.lastDataTime = millis();
        EBikeController::controller.isConnected = true;
        receivedInitialData = true;
    }
}

void setup() {
    Serial.begin(115200);
    delay(500);

    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_now_init() != ESP_OK) {
        delay(2000);
        ESP.restart();
    }

    esp_now_register_recv_cb(onDataReceive);

    unsigned long waitStart = millis();
    while (!receivedInitialData && millis() - waitStart < 5000) {
        delay(10);  // wait for ESP-NOW data
    }

    if (!receivedInitialData) {
        esp_sleep_enable_timer_wakeup(Config::SLEEP_DURATION_US);
        esp_deep_sleep_start();
    }

    EBikeController::controller.initializePinsAndLEDs();
    EBikeController::controller.initializeVESC();
}

void loop() {
    EBikeController::controller.update();
}
