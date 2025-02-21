#include <NimBLEDevice.h>
#include <VescUart.h>
#include <esp_now.h>
#include <WiFi.h>

namespace Config {
    // Previous configuration remains the same...
    constexpr uint8_t PPM_PIN = 22;
    constexpr uint8_t BATTERY_LEDS[] = {32, 33, 25, 26};
    constexpr uint8_t MODE_LEDS[] = {5, 18, 19};
    
    // Increased timeout to prevent premature disconnection
    constexpr unsigned long CONNECTION_TIMEOUT = 5000;
    constexpr unsigned long WIRELESS_TIMEOUT = 3000;  // Increased from 2000
    
    // Other configs remain the same
    constexpr float MIN_VOLTAGE = 30.0f;
    constexpr float VOLTAGE_RANGE = 12.0f;
    constexpr uint16_t PPM_IDLE = 600;
    constexpr uint16_t PPM_MODE_VALUES[] = {1030, 1150, 1270};
    constexpr char DEVICE_NAME[] = "esp32";
    constexpr char SERVICE_UUID[] = "12345678-1234-1234-1234-123456789012";
    constexpr char CHAR_UUID[] = "87654321-4321-4321-4321-210987654321";
}

struct __attribute__((packed)) StatusData {
    bool acceleratorPressed;
    uint8_t mode;
};

class EBikeController;
EBikeController* g_controller = nullptr;

class EBikeController {
private:
    struct {
        bool deviceConnected = false;
        bool acceleratorStatus = false;
        bool newDataReceived = false;
        bool wirelessConnected = false;
        unsigned long lastDataTime = 0;
        uint8_t currentMode = 1;
    } state;

    VescUart vescUart;
    NimBLEServer* bleServer = nullptr;
    NimBLECharacteristic* bleCharacteristic = nullptr;

    class ServerCallbacks: public NimBLEServerCallbacks {
    private:
        EBikeController& controller;
    public:
        ServerCallbacks(EBikeController& ctrl) : controller(ctrl) {}
        
        void onConnect(NimBLEServer*) {
            controller.state.deviceConnected = true;
            Serial.println("BLE Client connected!");
        }
        
        void onDisconnect(NimBLEServer*) {
            controller.state.deviceConnected = false;
            Serial.println("BLE Client disconnected!");
        }
    };

    void initializeLEDs() {
        Serial.println("Initializing LEDs...");
        
        pinMode(Config::PPM_PIN, OUTPUT);
        
        for (auto pin : Config::MODE_LEDS) pinMode(pin, OUTPUT);
        for (auto pin : Config::BATTERY_LEDS) pinMode(pin, OUTPUT);
        
        // Test all LEDs
        Serial.println("Testing LEDs...");
        for (auto pin : Config::MODE_LEDS) digitalWrite(pin, HIGH);
        for (auto pin : Config::BATTERY_LEDS) digitalWrite(pin, HIGH);
        delay(500);
        for (auto pin : Config::MODE_LEDS) digitalWrite(pin, LOW);
        for (auto pin : Config::BATTERY_LEDS) digitalWrite(pin, LOW);
        
        Serial.println("LED initialization complete");
    }

    bool initializeWireless() {
        Serial.println("Initializing ESP-NOW...");
        WiFi.mode(WIFI_STA);
        
        if (esp_now_init() != ESP_OK) {
            Serial.println("ESP-NOW initialization failed!");
            return false;
        }
        
        esp_now_register_recv_cb([](const esp_now_recv_info*, const uint8_t* data, int len) {
            if (g_controller) {
                g_controller->handleWirelessData(data, len);
            }
        });
        
        Serial.println("ESP-NOW initialized successfully");
        return true;
    }

    void initializeBLE() {
        Serial.println("Initializing BLE...");
        
        NimBLEDevice::init(Config::DEVICE_NAME);
        bleServer = NimBLEDevice::createServer();
        bleServer->setCallbacks(new ServerCallbacks(*this));

        auto* service = bleServer->createService(Config::SERVICE_UUID);
        bleCharacteristic = service->createCharacteristic(
            Config::CHAR_UUID,
            NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
        );

        service->start();
        auto* advertising = NimBLEDevice::getAdvertising();
        advertising->addServiceUUID(Config::SERVICE_UUID);
        advertising->start();
        
        Serial.println("BLE initialization complete");
        Serial.printf("Device name: %s\n", Config::DEVICE_NAME);
        Serial.printf("Service UUID: %s\n", Config::SERVICE_UUID);
    }

public:
    void handleWirelessData(const uint8_t* data, int len) {
        if (len == sizeof(StatusData)) {
            const StatusData& status = *reinterpret_cast<const StatusData*>(data);
            
            // Print state changes
            if (state.acceleratorStatus != status.acceleratorPressed || 
                state.currentMode != status.mode) {
                Serial.printf("Received - Accelerator: %s, Mode: %d\n",
                            status.acceleratorPressed ? "ON" : "OFF",
                            status.mode);
            }
            
            state.acceleratorStatus = status.acceleratorPressed;
            state.currentMode = status.mode;
            state.newDataReceived = true;
            state.lastDataTime = millis();
            
            if (!state.wirelessConnected) {
                Serial.println("Wireless connection established");
                state.wirelessConnected = true;
            }
        }
    }

private:
    void updateBatteryLEDs(float voltage) {
        static float lastVoltage = 0;
        if (abs(voltage - lastVoltage) > 0.1) {  // Only update if voltage changed by >0.1V
            int percentage = constrain(
                static_cast<int>((voltage - Config::MIN_VOLTAGE) * 100 / Config::VOLTAGE_RANGE),
                0, 100
            );
            
            for (size_t i = 0; i < 4; ++i) {
                digitalWrite(Config::BATTERY_LEDS[i], percentage >= (75 - i * 25));
            }
            
            Serial.printf("Battery voltage: %.2fV (%d%%)\n", voltage, percentage);
            lastVoltage = voltage;
        }
    }

    void updateModeLEDs() {
        for (uint8_t i = 0; i < 3; ++i) {
            digitalWrite(Config::MODE_LEDS[i], i + 1 == state.currentMode);
        }
        Serial.printf("Mode LEDs updated for mode: %d\n", state.currentMode);
    }

    void sendPPMSignal(uint16_t value) {
        digitalWrite(Config::PPM_PIN, HIGH);
        delayMicroseconds(value);
        digitalWrite(Config::PPM_PIN, LOW);
    }

    void updateBLEData(float voltage, int32_t rpm) {
        if (!state.deviceConnected) return;
        
        char jsonBuffer[64];
        snprintf(jsonBuffer, sizeof(jsonBuffer),
                R"({"rpm":%ld,"battery":%.2f,"mode":%u})",
                static_cast<long>(rpm), voltage, state.currentMode);
        
        bleCharacteristic->setValue(jsonBuffer);
        bleCharacteristic->notify();
    }

public:
    bool initialize() {
        Serial.begin(115200);
        delay(1000);  // Give serial time to initialize
        
        Serial.println("\n\n=== E-Bike Controller Starting ===");
        Serial.println("Version: 1.0");
        Serial.println("Initializing components...");
        
        initializeLEDs();
        
        if (!initializeWireless()) {
            Serial.println("Wireless initialization failed!");
            return false;
        }
        
        initializeBLE();
        
        Serial.println("Initializing VESC UART...");
        vescUart.setSerialPort(&Serial);
        
        Serial.println("Initialization complete!");
        Serial.println("Waiting for wireless connection...");
        
        return true;
    }

    void update() {
        unsigned long currentMillis = millis();
        
        // Check wireless connection status
        if (currentMillis - state.lastDataTime > Config::WIRELESS_TIMEOUT) {
            if (state.wirelessConnected) {
                Serial.println("Wireless connection lost!");
                state.wirelessConnected = false;
            }
            return;
        }

        if (vescUart.getVescValues()) {
            updateBatteryLEDs(vescUart.data.inpVoltage);
            
            if (state.newDataReceived) {
                state.newDataReceived = false;
                updateModeLEDs();
                
                uint16_t ppmValue = state.acceleratorStatus ? 
                    Config::PPM_MODE_VALUES[state.currentMode - 1] : 
                    Config::PPM_IDLE;
                    
                sendPPMSignal(ppmValue);
            }
            
            updateBLEData(vescUart.data.inpVoltage, vescUart.data.rpm);
        }
    }
};

EBikeController controller;

void setup() {
    g_controller = &controller;
    if (!controller.initialize()) {
        Serial.println("Initialization failed! Restarting...");
        delay(1000);
        ESP.restart();
    }
}

void loop() {
    controller.update();
}