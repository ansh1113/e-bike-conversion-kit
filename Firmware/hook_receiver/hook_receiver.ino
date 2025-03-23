#include <NimBLEDevice.h>
#include <VescUart.h>
#include <esp_now.h>
#include <WiFi.h>
#include <esp_wifi.h>  // Added for esp_wifi_set_channel function

// Configuration Constants
namespace Config {
    // Pin Definitions
    constexpr uint8_t PPM_PIN = 22;
    constexpr uint8_t BATTERY_LEDS[] = {32, 33, 25, 26};
    constexpr uint8_t MODE_LEDS[] = {5, 18, 19};
    constexpr uint8_t CONNECTION_LED = 2;  // Built-in LED for connection status
    
    // Timing Parameters
    constexpr unsigned long CONNECTION_TIMEOUT = 10000;  // 10 seconds for initial connection
    constexpr unsigned long WIRELESS_TIMEOUT = 4000;    // 3 seconds for connection loss detection
    
    // Voltage Parameters
    constexpr float MIN_VOLTAGE = 30.0f;
    constexpr float VOLTAGE_RANGE = 12.0f;
    
    // PPM Signal Values - Properly calibrated for all modes
    constexpr uint16_t PPM_IDLE = 600;
    constexpr uint16_t PPM_MODE_VALUES[] = {1030, 1180, 1250};  // Low, Medium, High modes
    
    // BLE Parameters
    constexpr char DEVICE_NAME[] = "ebike-controller";
    constexpr char SERVICE_UUID[] = "12345678-1234-1234-1234-123456789012";
    constexpr char CHAR_UUID[] = "87654321-4321-4321-4321-210987654321";
    
    // ESP-NOW Parameters
    constexpr uint8_t WIFI_CHANNEL = 1;  // Fixed channel for better ESP-NOW reliability
    constexpr int MAX_CONNECTION_RETRIES = 10;  // Maximum number of reconnection attempts
}

// Data structure for wireless communication
struct __attribute__((packed)) StatusData {
    bool acceleratorPressed;
    uint8_t mode;
};

// Forward declaration
class EBikeController;
EBikeController* g_controller = nullptr;

class EBikeController {
private:
    // State management
    struct {
        bool bleConnected = false;
        bool acceleratorActive = false;
        bool newDataReceived = false;
        bool wirelessConnected = false;
        unsigned long lastDataTime = 0;
        uint8_t currentMode = 1;
        uint8_t previousMode = 1;  // Track mode changes
        unsigned long lastReconnectAttempt = 0;
        int reconnectAttempts = 0;
    } state;

    // Components
    VescUart vescUart;
    NimBLEServer* bleServer = nullptr;
    NimBLECharacteristic* bleCharacteristic = nullptr;

    // BLE Server Callbacks
    class ServerCallbacks: public NimBLEServerCallbacks {
    private:
        EBikeController& controller;
    public:
        ServerCallbacks(EBikeController& ctrl) : controller(ctrl) {}
        
        void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) override {
            controller.state.bleConnected = true;
            Serial.println("BLE Client connected");
        }
        
        void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) override {
            controller.state.bleConnected = false;
            Serial.println("BLE Client disconnected");
        }
    };

public:
    // Initialize hardware components
    bool initialize() {
        Serial.begin(115200);
        delay(1000);  // Increased delay for serial to initialize properly
        
        Serial.println("\n\n=== E-Bike Controller v1.2 ===");
        Serial.println("Bugfix version with improved connection stability");
        
        // Initialize LEDs
        initializeLEDs();
        
        // Initialize wireless communication
        if (!initializeWireless()) {
            Serial.println("Wireless initialization failed!");
            return false;
        }
        
        // Initialize BLE
        initializeBLE();
        
        // Initialize VESC UART
        Serial.println("Initializing VESC UART...");
        vescUart.setSerialPort(&Serial);
        
        Serial.println("Initialization complete! Waiting for wireless connection...");
        return true;
    }
    
    // Main update loop
    void update() {
        unsigned long currentMillis = millis();
        
        // Check wireless connection status
        if (currentMillis - state.lastDataTime > Config::WIRELESS_TIMEOUT) {
            if (state.wirelessConnected) {
                Serial.println("Wireless connection lost!");
                state.wirelessConnected = false;
                digitalWrite(Config::CONNECTION_LED, LOW);
                
                // Force motor to idle when connection is lost
                sendPPMSignal(Config::PPM_IDLE);
            }
            
            // Attempt reconnection periodically
            if (currentMillis - state.lastReconnectAttempt > 2000 && 
                state.reconnectAttempts < Config::MAX_CONNECTION_RETRIES) {
                
                state.lastReconnectAttempt = currentMillis;
                state.reconnectAttempts++;
                
                // Re-initialize ESP-NOW
                Serial.printf("Reconnection attempt %d of %d...\n", 
                            state.reconnectAttempts, Config::MAX_CONNECTION_RETRIES);
                
                WiFi.disconnect();
                WiFi.mode(WIFI_STA);
                esp_now_deinit();
                delay(100);
                
                if (esp_now_init() == ESP_OK) {
                    esp_now_register_recv_cb([](const esp_now_recv_info* info, const uint8_t* data, int len) {
                        if (g_controller) {
                            g_controller->handleWirelessData(data, len);
                        }
                    });
                    Serial.println("ESP-NOW reinitialized");
                }
            }
            
            // Blink connection LED to indicate waiting for connection
            digitalWrite(Config::CONNECTION_LED, ((currentMillis / 300) % 2) ? HIGH : LOW);
            return;
        }

        // Reset reconnection counter when connected
        if (state.wirelessConnected) {
            state.reconnectAttempts = 0;
        }

        // Process VESC data when available
        if (vescUart.getVescValues()) {
            // Update battery display
            updateBatteryLEDs(vescUart.data.inpVoltage);
            
            // Process new control data if available
            if (state.newDataReceived || state.previousMode != state.currentMode) {
                state.newDataReceived = false;
                state.previousMode = state.currentMode;
                
                updateModeLEDs();
                
                // Debug output for mode changes
                Serial.printf("Mode: %d, Accelerator: %s\n", 
                            state.currentMode, 
                            state.acceleratorActive ? "ON" : "OFF");
                
                uint16_t ppmValue = state.acceleratorActive ? 
                    Config::PPM_MODE_VALUES[state.currentMode - 1] : 
                    Config::PPM_IDLE;
                
                // Debug PPM value
                Serial.printf("Sending PPM value: %d\n", ppmValue);
                    
                sendPPMSignal(ppmValue);
            }
            
            // Send telemetry data via BLE if connected
            if (state.bleConnected) {
                updateBLEData(vescUart.data.inpVoltage, vescUart.data.rpm);
            }
        } else {
            // Debug VESC communication issues
            static unsigned long lastVescErrorTime = 0;
            if (currentMillis - lastVescErrorTime > 5000) {
                Serial.println("Warning: Unable to get VESC values");
                lastVescErrorTime = currentMillis;
            }
        }
    }
    
    // Handle wireless data reception
    void handleWirelessData(const uint8_t* data, int length) {
        if (length == sizeof(StatusData)) {
            const StatusData& status = *reinterpret_cast<const StatusData*>(data);
            
            // Always update state to ensure mode changes are processed
            bool stateChanged = (state.acceleratorActive != status.acceleratorPressed || 
                                state.currentMode != status.mode);
            
            // Verify mode is within valid range (1-3)
            uint8_t safeMode = constrain(status.mode, 1, 3);
            
            // Debug output for state changes
            if (stateChanged) {
                Serial.printf("Received - Accelerator: %s, Mode: %d\n",
                            status.acceleratorPressed ? "ON" : "OFF",
                            safeMode);
            }
            
            state.acceleratorActive = status.acceleratorPressed;
            state.currentMode = safeMode;
            state.newDataReceived = true;
            state.lastDataTime = millis();
            
            if (!state.wirelessConnected) {
                Serial.println("Wireless connection established");
                state.wirelessConnected = true;
                digitalWrite(Config::CONNECTION_LED, HIGH);
            }
        } else {
            Serial.printf("Received invalid data length: %d (expected %d)\n", 
                        length, sizeof(StatusData));
        }
    }

private:
    // Initialize LED pins and test them
    void initializeLEDs() {
        Serial.println("Initializing LEDs...");
        
        pinMode(Config::PPM_PIN, OUTPUT);
        pinMode(Config::CONNECTION_LED, OUTPUT);
        
        for (auto pin : Config::MODE_LEDS) pinMode(pin, OUTPUT);
        for (auto pin : Config::BATTERY_LEDS) pinMode(pin, OUTPUT);
        
        // Test all LEDs
        for (auto pin : Config::MODE_LEDS) digitalWrite(pin, HIGH);
        for (auto pin : Config::BATTERY_LEDS) digitalWrite(pin, HIGH);
        digitalWrite(Config::CONNECTION_LED, HIGH);
        delay(800);  // Longer delay to clearly see the test pattern
        for (auto pin : Config::MODE_LEDS) digitalWrite(pin, LOW);
        for (auto pin : Config::BATTERY_LEDS) digitalWrite(pin, LOW);
        digitalWrite(Config::CONNECTION_LED, LOW);
        
        Serial.println("LED initialization complete");
    }

    // Initialize wireless communication (ESP-NOW)
    bool initializeWireless() {
        Serial.println("Initializing ESP-NOW...");
        
        // Set WiFi mode and channel explicitly for better stability
        WiFi.mode(WIFI_STA);
        WiFi.disconnect();
        delay(100);
        
        // Optional: Set WiFi to a specific channel for better ESP-NOW stability
        esp_wifi_set_channel(Config::WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
        
        if (esp_now_init() != ESP_OK) {
            Serial.println("ESP-NOW initialization failed!");
            return false;
        }
        
        // Register callback for data reception
        esp_now_register_recv_cb([](const esp_now_recv_info* info, const uint8_t* data, int len) {
            if (g_controller) {
                g_controller->handleWirelessData(data, len);
            }
        });
        
        Serial.println("ESP-NOW initialized successfully on channel " + String(Config::WIFI_CHANNEL));
        return true;
    }

    // Initialize Bluetooth Low Energy
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
        
        Serial.println("BLE initialized as: " + String(Config::DEVICE_NAME));
    }

    // Update battery level LEDs based on voltage
    void updateBatteryLEDs(float voltage) {
        static float lastVoltage = 0;
        
        // Only update if voltage changed significantly (reduce LED flickering)
        if (abs(voltage - lastVoltage) > 0.1f) {  
            int percentage = constrain(
                static_cast<int>((voltage - Config::MIN_VOLTAGE) * 100 / Config::VOLTAGE_RANGE),
                0, 100
            );
            
            for (size_t i = 0; i < 4; ++i) {
                digitalWrite(Config::BATTERY_LEDS[i], percentage >= (75 - i * 25));
            }
            
            Serial.printf("Battery: %.1fV (%d%%)\n", voltage, percentage);
            lastVoltage = voltage;
        }
    }

    // Update mode indicator LEDs
    void updateModeLEDs() {
        // Clear all mode LEDs first
        for (uint8_t i = 0; i < 3; ++i) {
            digitalWrite(Config::MODE_LEDS[i], LOW);
        }
        
        // Set the current mode LED (with bounds check)
        if (state.currentMode >= 1 && state.currentMode <= 3) {
            digitalWrite(Config::MODE_LEDS[state.currentMode - 1], HIGH);
        } else {
            // If mode is invalid, flash all LEDs as warning
            for (uint8_t i = 0; i < 3; ++i) {
                digitalWrite(Config::MODE_LEDS[i], HIGH);
            }
            delay(50);
            for (uint8_t i = 0; i < 3; ++i) {
                digitalWrite(Config::MODE_LEDS[i], LOW);
            }
        }
    }

    // Send PPM signal to the ESC with validation
    void sendPPMSignal(uint16_t value) {
        // Safety check for reasonable PPM values
        if (value < 500 || value > 2000) {
            Serial.printf("Warning: Invalid PPM value (%d), using default\n", value);
            value = Config::PPM_IDLE;
        }
        
        // Send the signal
        digitalWrite(Config::PPM_PIN, HIGH);
        delayMicroseconds(value);
        digitalWrite(Config::PPM_PIN, LOW);
        // Add a small delay to ensure the signal is properly detected
        delayMicroseconds(100);
    }

    // Send telemetry data via BLE
    void updateBLEData(float voltage, int32_t rpm) {
        if (!state.bleConnected) return;
        
        char jsonBuffer[64];
        snprintf(jsonBuffer, sizeof(jsonBuffer),
                R"({"rpm":%ld,"battery":%.1f,"mode":%u})",
                static_cast<long>(rpm), voltage, state.currentMode);
        
        bleCharacteristic->setValue(jsonBuffer);
        bleCharacteristic->notify();
    }
};

// Global controller instance
EBikeController controller;

void setup() {
    g_controller = &controller;
    if (!controller.initialize()) {
        Serial.println("Initialization failed! Restarting in 5 seconds...");
        delay(5000);
        ESP.restart();
    }
}

void loop() {
    controller.update();
    // Small delay to prevent excessive CPU usage
    delay(20);
}