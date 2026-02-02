/*
 * CLIPP - Smart Notification Wearable
 * PLATFORM: PlatformIO with Seeed XIAO nRF52840
 * LIBRARY: Adafruit Bluefruit nRF52 (native Nordic BLE stack)
 * 
 * Features:
 * - Device pairing/locking to one phone
 * - Personalized device name (e.g., "Stanley's Clipp")
 * - Triple-click reset from device
 * - Reset from phone command
 * - Deep sleep power management
 * - Display brightness control
 * - Auto-reconnect support
 */

#include <Arduino.h>
#include <bluefruit.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_LC709203F.h>
#include <Adafruit_LittleFS.h>
#include <InternalFileSystem.h>
#include <nrf_power.h>

using namespace Adafruit_LittleFS_Namespace;

// OLED Display settings (0.91" 128x32 I2C)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// LC709203F Fuel Gauge (I2C on SDA/SCL = D4/D5)
Adafruit_LC709203F lc;
bool fuelGaugeInitialized = false;
volatile uint8_t currentBatteryPercent = 0;
volatile uint16_t currentBatteryVoltage = 0;
volatile bool isCharging = false;
const int STAT1_PIN = D6;  // MCP board STAT1
const int STAT2_PIN = D7;  // MCP board STAT2

// Button pin
const int BUTTON_PIN = D0;

// Vibration motor pin (D1 = P0.03 = pin 1 in Arduino mapping)
const int VIBRATOR_PIN = 1;  // D1 on XIAO nRF52840

// Power management constants
const unsigned long LONG_PRESS_TIME = 5000;  // 5 seconds for power on/off

// Device states
enum DeviceState {
  STATE_BOOT,           // Showing CLIPP logo
  STATE_NOT_CONNECTED,  // BLE not connected
  STATE_CONNECTED,      // BLE connected, normal operation
  STATE_SHUTDOWN_PROMPT,// Asking user to confirm shutdown
  STATE_SHUTTING_DOWN,  // Showing "Shutting Down" before sleep
  STATE_RESET_PROMPT    // Asking user to confirm reset (triple-click)
};

volatile DeviceState deviceState = STATE_BOOT;

// ============== PERSISTENT STORAGE ==============
#define CONFIG_FILENAME    "/clipp_config.dat"

// Pairing data structure
struct ClippConfig {
  char deviceName[32];           // Custom device name (e.g., "Stanley's Clipp")
  uint8_t pairedAddress[6];      // Paired phone's BLE address
  bool isPaired;                 // Whether device is paired
  uint8_t brightness;            // Display brightness (0-255)
  uint32_t checksum;             // Simple checksum for validation
};

ClippConfig config;
File configFile(InternalFS);

// FreeRTOS task handles
TaskHandle_t bleTaskHandle;
TaskHandle_t batteryTaskHandle;
TaskHandle_t buttonTaskHandle;

// Shared battery data (protected by mutex)
SemaphoreHandle_t batteryMutex;
volatile uint16_t sharedBatteryMV = 0;
volatile uint8_t sharedBatteryPercent = 0;

// Shared last message data (protected by mutex)
SemaphoreHandle_t messageMutex;
String lastAppName = "";
String lastSender = "";
String lastMessage = "";

// BLE connection state (updated by callbacks)
volatile bool bleConnected = false;
volatile bool pendingReset = false;  // Flag for reset from phone

// Shared display state (for timeout across tasks)
volatile bool displayActive = false;
volatile unsigned long displayOnTime = 0;
const unsigned long DISPLAY_TIMEOUT = 5000;

// BLE Service and Characteristic UUIDs (custom 128-bit)
static BLEUuid SERVICE_UUID("819b2f01-9d7d-42f1-a58e-29e5c07dd6b6");
static BLEUuid RX_CHAR_UUID("5600f473-a667-4c60-b0b1-7c68fbe9720f");
static BLEUuid TX_CHAR_UUID("b2fd3be9-40ca-48cd-80ed-5d04b2a71da2");

// BLE Service and Characteristics
BLEService bleService = BLEService(SERVICE_UUID);
BLECharacteristic rxCharacteristic = BLECharacteristic(RX_CHAR_UUID);
BLECharacteristic txCharacteristic = BLECharacteristic(TX_CHAR_UUID);

// LED Pin - Built-in LED on Seeed XIAO nRF52840 
const int LED_PIN = LED_BUILTIN;

// Buffer for reassembling chunked messages
String messageBuffer = "";
unsigned long lastChunkTime = 0;
const unsigned long CHUNK_TIMEOUT = 1000; // 1 second timeout to complete a message

// Track last reported battery value to reduce BLE notifications
uint8_t lastReportedBatteryPercent = 0;

// Forward declarations
void showBootScreen();
void showNotConnected();
void showShutdownPrompt();
void showShuttingDown();
void showResetPrompt();
void showResetting();
void enterDeepSleep();
void loadConfig();
void saveConfig();
void resetDevice();
void setDisplayBrightness(uint8_t brightness);
bool isAddressMatch(uint8_t* addr1, uint8_t* addr2);
void vibrateNotification();
void setVibrator(bool on);

// ============== CONFIG MANAGEMENT ==============

uint32_t calculateChecksum(ClippConfig* cfg) {
  uint32_t sum = 0;
  uint8_t* data = (uint8_t*)cfg;
  // Sum all bytes except the checksum field itself
  for (size_t i = 0; i < sizeof(ClippConfig) - sizeof(uint32_t); i++) {
    sum += data[i];
  }
  return sum;
}

void loadConfig() {
  // Initialize defaults
  strcpy(config.deviceName, "CLIPP");
  memset(config.pairedAddress, 0, 6);
  config.isPaired = false;
  config.brightness = 143;  // Max brightness for 128x32 OLED (0-143 range)
  config.checksum = 0;
  
  // Initialize internal file system
  InternalFS.begin();
  
  // Try to load config
  if (InternalFS.exists(CONFIG_FILENAME)) {
    configFile.open(CONFIG_FILENAME, FILE_O_READ);
    if (configFile) {
      configFile.read(&config, sizeof(ClippConfig));
      configFile.close();
      
      // Validate checksum
      uint32_t expectedChecksum = calculateChecksum(&config);
      if (config.checksum != expectedChecksum) {
        Serial.println("[CONFIG] Checksum mismatch, using defaults");
        strcpy(config.deviceName, "CLIPP");
        memset(config.pairedAddress, 0, 6);
        config.isPaired = false;
        config.brightness = 143;
      } else {
        Serial.print("[CONFIG] Loaded: ");
        Serial.print(config.deviceName);
        Serial.print(", paired: ");
        Serial.println(config.isPaired ? "yes" : "no");
      }
    }
  } else {
    Serial.println("[CONFIG] No config file, using defaults");
  }
}

void saveConfig() {
  // Calculate checksum
  config.checksum = calculateChecksum(&config);
  
  // Remove old file if exists
  if (InternalFS.exists(CONFIG_FILENAME)) {
    InternalFS.remove(CONFIG_FILENAME);
  }
  
  // Write new config
  configFile.open(CONFIG_FILENAME, FILE_O_WRITE);
  if (configFile) {
    configFile.write((uint8_t*)&config, sizeof(ClippConfig));
    configFile.close();
    Serial.println("[CONFIG] Saved");
  } else {
    Serial.println("[CONFIG] Failed to save");
  }
}

void resetDevice() {
  Serial.println("[RESET] Resetting device to factory defaults...");
  
  // Clear config
  strcpy(config.deviceName, "CLIPP");
  memset(config.pairedAddress, 0, 6);
  config.isPaired = false;
  config.brightness = 143;
  saveConfig();
  
  // Disconnect if connected
  if (Bluefruit.connected()) {
    Bluefruit.disconnect(Bluefruit.connHandle());
  }
  
  // Clear BLE bonds
  Bluefruit.Periph.clearBonds();
  
  // Update device name
  Bluefruit.setName("CLIPP");
  
  Serial.println("[RESET] Device reset complete, restarting...");
  delay(500);
  
  // Restart the device
  NVIC_SystemReset();
}

bool isAddressMatch(uint8_t* addr1, uint8_t* addr2) {
  for (int i = 0; i < 6; i++) {
    if (addr1[i] != addr2[i]) return false;
  }
  return true;
}

// ============== VIBRATION FUNCTIONS ==============

void vibrateNotification() {
  // Vibrate 2 times with short bursts
  for (int i = 0; i < 2; i++) {
    digitalWrite(VIBRATOR_PIN, HIGH);
    delay(100);  // 100ms vibration
    digitalWrite(VIBRATOR_PIN, LOW);
    if (i < 1) {
      delay(100);  // 100ms pause between vibrations
    }
  }
  Serial.println("[VIBRATE] Notification vibration");
}

void setVibrator(bool on) {
  digitalWrite(VIBRATOR_PIN, on ? HIGH : LOW);
  Serial.print("[VIBRATE] Vibrator ");
  Serial.println(on ? "ON" : "OFF");
}

// ============== DISPLAY FUNCTIONS ==============

void setDisplayBrightness(uint8_t brightness) {
  // SSD1306 contrast control for 128x32 display
  // Effective range is 0x00 to 0x8F (0-143), values above 143 don't increase brightness
  if (brightness > 143) brightness = 143;
  display.ssd1306_command(SSD1306_SETCONTRAST);
  display.ssd1306_command(brightness);
  config.brightness = brightness;
  Serial.print("[DISPLAY] Brightness set to: ");
  Serial.println(brightness);
}

void showBrightnessPreview(uint8_t brightness) {
  // Show brightness level on display so user can see the effect
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Brightness:");
  
  // Draw a progress bar
  int barWidth = (brightness * 120) / 143;
  display.drawRect(4, 14, 120, 12, SSD1306_WHITE);
  display.fillRect(6, 16, barWidth, 8, SSD1306_WHITE);
  
  // Show percentage
  int percent = (brightness * 100) / 143;
  display.setCursor(50, 28);
  display.print(percent);
  display.print("%");
  
  display.display();
}

void showBootScreen() {
  display.clearDisplay();
  display.setTextSize(3);
  display.setTextColor(SSD1306_WHITE);
  
  // Center "CLIPP" on display
  int16_t x1, y1;
  uint16_t w, h;
  display.getTextBounds("CLIPP", 0, 0, &x1, &y1, &w, &h);
  display.setCursor((SCREEN_WIDTH - w) / 2, (SCREEN_HEIGHT - h) / 2);
  display.print("CLIPP");
  display.display();
  
  Serial.println("[DISPLAY] Boot screen - CLIPP");
}

void showNotConnected() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 4);
  display.print("Not Connected");
  display.setCursor(0, 18);
  display.print("Searching...");
  display.display();
  
  Serial.println("[DISPLAY] Not connected");
}

void showShutdownPrompt() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Want to shutdown?");
  display.setCursor(0, 12);
  display.print("Long press: No");
  display.setCursor(0, 24);
  display.print("1 click: Yes");
  display.display();
  
  Serial.println("[DISPLAY] Shutdown prompt");
}

void showShuttingDown() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 8);
  display.print("Shutting");
  display.setCursor(30, 24);
  display.print("Down");
  display.display();
  
  Serial.println("[DISPLAY] Shutting down");
}

void showResetPrompt() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print("Reset device?");
  display.setCursor(0, 12);
  display.print("Long hold: Yes");
  display.setCursor(0, 24);
  display.print("Click: No");
  display.display();
  
  Serial.println("[DISPLAY] Reset prompt");
}

void showResetting() {
  display.clearDisplay();
  display.setTextSize(2);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(15, 8);
  display.print("Resetting");
  display.display();
  
  Serial.println("[DISPLAY] Resetting");
}

void drawChargeIcon(int x, int y) {
  // Simple lightning bolt icon (approx 8x12)
  display.drawLine(x + 2, y + 0, x + 6, y + 6, SSD1306_WHITE);
  display.drawLine(x + 6, y + 6, x + 4, y + 6, SSD1306_WHITE);
  display.drawLine(x + 4, y + 6, x + 8, y + 12, SSD1306_WHITE);
  display.drawLine(x + 8, y + 12, x + 3, y + 6, SSD1306_WHITE);
  display.drawLine(x + 3, y + 6, x + 5, y + 6, SSD1306_WHITE);
}

void showBatteryScreen() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  // Title
  display.setCursor(0, 0);
  display.print("Battery Status");
  
  // Battery percentage with large text
  display.setTextSize(2);
  display.setCursor(20, 10);
  String percentText = String(currentBatteryPercent) + "%";
  display.print(percentText);
  
  if (isCharging) {
    int16_t x1, y1;
    uint16_t w, h;
    display.getTextBounds(percentText, 20, 10, &x1, &y1, &w, &h);
    drawChargeIcon(20 + w + 2, 12);
  }
  
  // Voltage and charging status on bottom line
  display.setTextSize(1);
  display.setCursor(0, 28);
  display.print(currentBatteryVoltage);
  display.print("mV");
  
  // Charging indicator text (optional)
  if (isCharging) {
    display.setCursor(85, 28);
    display.print("CHG");
  }
  
  display.display();
  
  Serial.print("[DISPLAY] Battery: ");
  Serial.print(currentBatteryPercent);
  Serial.print("% | ");
  Serial.print(currentBatteryVoltage);
  Serial.print("mV | Charging: ");
  Serial.println(isCharging ? "YES" : "NO");
}

void enterDeepSleep() {
  Serial.println("[POWER] Entering deep sleep...");
  
  // Disconnect BLE if connected
  if (Bluefruit.connected()) {
    Bluefruit.disconnect(Bluefruit.connHandle());
  }
  
  // Stop BLE advertising
  Bluefruit.Advertising.stop();
  
  // Clear and turn off display
  display.clearDisplay();
  display.display();
  
  // Turn off LED
  digitalWrite(LED_PIN, LOW);
  
  // Turn off vibration motor
  digitalWrite(VIBRATOR_PIN, LOW);
  
  // Small delay to let everything settle
  delay(100);
  
  // Configure button pin for wake-up (sense LOW level)
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // Configure the button as wake-up source using SENSE
  NRF_GPIO->PIN_CNF[g_ADigitalPinMap[BUTTON_PIN]] = 
    (GPIO_PIN_CNF_SENSE_Low << GPIO_PIN_CNF_SENSE_Pos) |
    (GPIO_PIN_CNF_DRIVE_S0S1 << GPIO_PIN_CNF_DRIVE_Pos) |
    (GPIO_PIN_CNF_PULL_Pullup << GPIO_PIN_CNF_PULL_Pos) |
    (GPIO_PIN_CNF_INPUT_Connect << GPIO_PIN_CNF_INPUT_Pos) |
    (GPIO_PIN_CNF_DIR_Input << GPIO_PIN_CNF_DIR_Pos);
  
  Serial.println("[POWER] Going to System OFF mode");
  Serial.flush();
  
  // Enter System OFF (lowest power mode)
  NRF_POWER->SYSTEMOFF = 1;
  
  while(1) { }
}

bool readChargingStatusFromStatPins() {
  // MCP STAT pins are active-low
  bool stat1Low = (digitalRead(STAT1_PIN) == LOW);
  bool stat2Low = (digitalRead(STAT2_PIN) == LOW);
  
  // Typical behavior: STAT1 low = charging, STAT2 low = charge complete
  if (stat1Low) return true;
  if (stat2Low) return false;
  return false;
}

// Read battery data from LC709203F fuel gauge
void readFuelGauge() {
  if (!fuelGaugeInitialized) return;
  
  // Read battery percentage (0-100%)
  float percentRaw = lc.cellPercent();
  if (percentRaw < 0.0f) percentRaw = 0.0f;
  if (percentRaw > 100.0f) percentRaw = 100.0f;
  uint8_t batteryPercent = (uint8_t)(percentRaw + 0.5f);
  
  // Read cell voltage in mV
  float voltageRaw = lc.cellVoltage();
  if (voltageRaw < 0.0f) voltageRaw = 0.0f;
  uint16_t batteryMV = (uint16_t)(voltageRaw);
  
  // Detect charging status via MCP STAT pins
  bool newChargeState = readChargingStatusFromStatPins();
  
  // Update shared battery data
  currentBatteryPercent = batteryPercent;
  currentBatteryVoltage = batteryMV;
  isCharging = newChargeState;
  
  Serial.print("[FUEL GAUGE] ");
  Serial.print(batteryPercent);
  Serial.print("% | ");
  Serial.print(batteryMV);
  Serial.print("mV | Charging: ");
  Serial.println(isCharging ? "YES" : "NO");
}

// ============== FreeRTOS TASKS ==============

// Task: Handle BLE message processing
void bleTask(void *pvParameters) {
  (void) pvParameters;
  
  while (1) {
    if (deviceState == STATE_SHUTDOWN_PROMPT || deviceState == STATE_SHUTTING_DOWN || 
        deviceState == STATE_RESET_PROMPT) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    
    // Handle pending reset from phone
    if (pendingReset) {
      pendingReset = false;
      showResetting();
      delay(1000);
      resetDevice();
    }
    
    if (Bluefruit.connected()) {
      if (messageBuffer.length() > 0 && (millis() - lastChunkTime) > CHUNK_TIMEOUT) {
        
        // ========== HANDLE COMMANDS FROM PHONE ==========
        if (messageBuffer.startsWith("CMD:")) {
          String cmd = messageBuffer.substring(4);
          
          if (cmd.startsWith("SETNAME:")) {
            // Set device name: CMD:SETNAME:Stanley's Clipp
            String newName = cmd.substring(8);
            newName.trim();
            if (newName.length() > 0 && newName.length() < 30) {
              strncpy(config.deviceName, newName.c_str(), 31);
              config.deviceName[31] = '\0';
              saveConfig();
              
              // Update BLE name (requires restart to take effect in advertising)
              Serial.print("[CMD] Device name set to: ");
              Serial.println(config.deviceName);
              
              // Send confirmation
              String response = "OK:NAME:" + String(config.deviceName);
              txCharacteristic.notify(response.c_str(), response.length());
            }
          }
          else if (cmd.startsWith("BRIGHTNESS:")) {
            // Set brightness: CMD:BRIGHTNESS:100
            // Valid range is 0-143 for 128x32 SSD1306
            int brightness = cmd.substring(11).toInt();
            if (brightness >= 0 && brightness <= 143) {
              setDisplayBrightness((uint8_t)brightness);
              showBrightnessPreview((uint8_t)brightness);
              
              // Activate display timeout so it turns off after a few seconds
              displayActive = true;
              displayOnTime = millis();
              
              saveConfig();
              
              String response = "OK:BRIGHTNESS:" + String(brightness);
              txCharacteristic.notify(response.c_str(), response.length());
            }
          }
          else if (cmd == "RESET") {
            // Reset device: CMD:RESET
            Serial.println("[CMD] Reset requested from phone");
            pendingReset = true;
          }
          else if (cmd == "GETINFO") {
            // Get device info: CMD:GETINFO
            String info = "INFO:" + String(config.deviceName) + ":" + 
                          String(config.isPaired ? "paired" : "unpaired") + ":" +
                          String(config.brightness);
            txCharacteristic.notify(info.c_str(), info.length());
          }
          else if (cmd == "PAIR") {
            // Pair with current phone: CMD:PAIR
            // Get connected device address using Bluefruit API
            BLEConnection* connection = Bluefruit.Connection(Bluefruit.connHandle());
            if (connection) {
              ble_gap_addr_t addr = connection->getPeerAddr();
              
              memcpy(config.pairedAddress, addr.addr, 6);
              config.isPaired = true;
              saveConfig();
              
              Serial.println("[CMD] Device paired");
              txCharacteristic.notify("OK:PAIRED", 9);
            }
          }
          else if (cmd == "VIBRATE:ON") {
            // Turn vibrator on for testing: CMD:VIBRATE:ON
            setVibrator(true);
            txCharacteristic.notify("OK:VIBRATE:ON", 13);
          }
          else if (cmd == "VIBRATE:OFF") {
            // Turn vibrator off: CMD:VIBRATE:OFF
            setVibrator(false);
            txCharacteristic.notify("OK:VIBRATE:OFF", 14);
          }
          else if (cmd == "VIBRATE:TEST") {
            // Test notification vibration pattern: CMD:VIBRATE:TEST
            vibrateNotification();
            txCharacteristic.notify("OK:VIBRATE:TEST", 15);
          }
        }
        // ========== HANDLE NOTIFICATIONS ==========
        else if (messageBuffer.startsWith("CALL:")) {
          int firstColon = messageBuffer.indexOf(":");
          int lastColon = messageBuffer.lastIndexOf(":");
          
          if (firstColon != -1 && lastColon != -1 && firstColon < lastColon) {
            String phoneNumber = messageBuffer.substring(firstColon + 1, lastColon);
            String duration = messageBuffer.substring(lastColon + 1);
            
            if (xSemaphoreTake(messageMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              lastAppName = "Call";
              lastSender = phoneNumber;
              lastMessage = "Duration: " + duration;
              xSemaphoreGive(messageMutex);
            }
            
            // Vibrate for incoming call
            vibrateNotification();
            
            Serial.print("[CALL] From: ");
            Serial.println(phoneNumber);
          }
        }
        else if (messageBuffer.startsWith("NOTIF:")) {
          int firstColon = messageBuffer.indexOf(":");
          int secondColon = messageBuffer.indexOf(":", firstColon + 1);
          int thirdColon = messageBuffer.indexOf(":", secondColon + 1);
          
          if (firstColon != -1 && secondColon != -1) {
            String appName = messageBuffer.substring(firstColon + 1, secondColon);
            String sender = "";
            String content = "";
            
            if (thirdColon != -1) {
              sender = messageBuffer.substring(secondColon + 1, thirdColon);
              content = messageBuffer.substring(thirdColon + 1);
            } else {
              content = messageBuffer.substring(secondColon + 1);
            }
            
            // Clean up app name
            if (appName.startsWith("com.")) {
              int lastDot = appName.lastIndexOf(".");
              if (lastDot > 4) {
                appName = appName.substring(lastDot + 1);
              }
            }
            if (appName.length() > 0) {
              appName.setCharAt(0, toupper(appName.charAt(0)));
            }
            
            if (xSemaphoreTake(messageMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              lastAppName = appName;
              lastSender = sender;
              lastMessage = content;
              xSemaphoreGive(messageMutex);
            }
            
            // Vibrate for notification
            vibrateNotification();
            
            Serial.print("[NOTIF] ");
            Serial.print(appName);
            Serial.print(": ");
            Serial.println(sender);
          }
        }
        
        messageBuffer = "";
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// Task: Battery monitoring via LC709203F fuel gauge
void batteryTask(void *pvParameters) {
  (void) pvParameters;
  
  while (1) {
    if (deviceState == STATE_SHUTDOWN_PROMPT || deviceState == STATE_SHUTTING_DOWN ||
        deviceState == STATE_RESET_PROMPT) {
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    
    // Read fuel gauge periodically
    readFuelGauge();
    
    // Send battery status via BLE if percentage changed significantly or charging state changed
    if (Bluefruit.connected()) {
      if (abs((int)currentBatteryPercent - (int)lastReportedBatteryPercent) >= 1 || lastReportedBatteryPercent == 0) {
        lastReportedBatteryPercent = currentBatteryPercent;
        
        String batteryStatus = "BATT:" + String(currentBatteryPercent) + "%:" + 
                              String(currentBatteryVoltage) + "mV:" + 
                              (isCharging ? "charging" : "discharging");
        txCharacteristic.notify(batteryStatus.c_str(), batteryStatus.length());
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 seconds
  }
}

// Task: Button and display handling
void buttonTask(void *pvParameters) {
  (void) pvParameters;
  
  bool lastButtonState = HIGH;
  
  // Click detection
  unsigned long lastClickTime = 0;
  int clickCount = 0;
  const unsigned long MULTI_CLICK_TIME = 400;
  const unsigned long CLICK_WAIT_TIME = 500;
  bool waitingForMoreClicks = false;
  
  // Long press detection
  unsigned long buttonPressStartTime = 0;
  bool buttonHeld = false;
  bool longPressTriggered = false;
  
  bool lastBleConnected = false;
  
  while (1) {
    bool currentButtonState = digitalRead(BUTTON_PIN);
    bool currentBleConnected = Bluefruit.connected();
    
    // ============== HANDLE RESET PROMPT STATE ==============
    if (deviceState == STATE_RESET_PROMPT) {
      if (currentButtonState == LOW && lastButtonState == HIGH) {
        buttonPressStartTime = millis();
        buttonHeld = true;
        longPressTriggered = false;
      }
      
      if (currentButtonState == HIGH && lastButtonState == LOW) {
        if (buttonHeld && !longPressTriggered) {
          // Short press = No, cancel reset
          Serial.println("[RESET] User cancelled reset");
          deviceState = currentBleConnected ? STATE_CONNECTED : STATE_NOT_CONNECTED;
          if (!currentBleConnected) {
            showNotConnected();
          } else {
            display.clearDisplay();
            display.display();
            displayActive = false;
          }
        }
        buttonHeld = false;
      }
      
      // Long hold = Yes, reset
      if (buttonHeld && !longPressTriggered && (millis() - buttonPressStartTime) >= LONG_PRESS_TIME) {
        longPressTriggered = true;
        Serial.println("[RESET] User confirmed reset");
        showResetting();
        delay(1000);
        resetDevice();
      }
      
      lastButtonState = currentButtonState;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    
    // ============== HANDLE SHUTDOWN PROMPT STATE ==============
    if (deviceState == STATE_SHUTDOWN_PROMPT) {
      if (currentButtonState == LOW && lastButtonState == HIGH) {
        buttonPressStartTime = millis();
        buttonHeld = true;
        longPressTriggered = false;
      }
      
      if (currentButtonState == HIGH && lastButtonState == LOW) {
        if (buttonHeld && !longPressTriggered) {
          Serial.println("[POWER] User confirmed shutdown");
          deviceState = STATE_SHUTTING_DOWN;
          showShuttingDown();
          delay(1500);
          enterDeepSleep();
        }
        buttonHeld = false;
      }
      
      if (buttonHeld && !longPressTriggered && (millis() - buttonPressStartTime) >= LONG_PRESS_TIME) {
        longPressTriggered = true;
        Serial.println("[POWER] User cancelled shutdown");
        deviceState = currentBleConnected ? STATE_CONNECTED : STATE_NOT_CONNECTED;
        if (!currentBleConnected) {
          showNotConnected();
        } else {
          display.clearDisplay();
          display.display();
          displayActive = false;
        }
      }
      
      lastButtonState = currentButtonState;
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    
    // ============== HANDLE BLE CONNECTION STATE CHANGES ==============
    if (currentBleConnected != lastBleConnected) {
      lastBleConnected = currentBleConnected;
      
      if (currentBleConnected) {
        deviceState = STATE_CONNECTED;
        display.clearDisplay();
        display.display();
        displayActive = false;
        Serial.println("[STATE] Connected");
      } else {
        deviceState = STATE_NOT_CONNECTED;
        showNotConnected();
        displayActive = true;
        displayOnTime = millis();
        Serial.println("[STATE] Disconnected");
      }
    }
    
    // ============== BUTTON PRESS/RELEASE DETECTION ==============
    if (currentButtonState == LOW && lastButtonState == HIGH) {
      // Button pressed
      buttonPressStartTime = millis();
      buttonHeld = true;
      longPressTriggered = false;
    }
    
    // Long press for shutdown
    if (buttonHeld && !longPressTriggered && (millis() - buttonPressStartTime) >= LONG_PRESS_TIME) {
      longPressTriggered = true;
      deviceState = STATE_SHUTDOWN_PROMPT;
      showShutdownPrompt();
      waitingForMoreClicks = false;
      clickCount = 0;
    }
    
    // Button released
    if (currentButtonState == HIGH && lastButtonState == LOW) {
      buttonHeld = false;
      
      if (!longPressTriggered) {
        unsigned long now = millis();
        
        if (waitingForMoreClicks && (now - lastClickTime) < MULTI_CLICK_TIME) {
          clickCount++;
        } else {
          clickCount = 1;
        }
        
        lastClickTime = now;
        waitingForMoreClicks = true;
      }
    }
    
    lastButtonState = currentButtonState;
    
    // ============== PROCESS CLICKS AFTER WAIT TIME ==============
    if (waitingForMoreClicks && (millis() - lastClickTime) >= CLICK_WAIT_TIME) {
      waitingForMoreClicks = false;
      
      Serial.print("[BUTTON] Click count: ");
      Serial.println(clickCount);
      
      if (clickCount == 3) {
        // Triple-click: Reset prompt
        deviceState = STATE_RESET_PROMPT;
        showResetPrompt();
      }
      else if (clickCount == 2 && deviceState == STATE_CONNECTED) {
        // Double-click: Show battery screen with fuel gauge data
        displayOnTime = millis();
        displayActive = true;
        showBatteryScreen();
      }
      else if (clickCount == 1 && deviceState == STATE_CONNECTED) {
        // Single-click: Toggle message display
        if (displayActive) {
          display.clearDisplay();
          display.display();
          displayActive = false;
          
          if (xSemaphoreTake(messageMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            lastAppName = "";
            lastSender = "";
            lastMessage = "";
            xSemaphoreGive(messageMutex);
          }
          
          Serial.println("[SINGLE-CLICK] Display off");
        } else {
          displayOnTime = millis();
          displayActive = true;
          
          String appName = "";
          String sender = "";
          String message = "";
          if (xSemaphoreTake(messageMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            appName = lastAppName;
            sender = lastSender;
            message = lastMessage;
            xSemaphoreGive(messageMutex);
          }
          
          display.clearDisplay();
          display.setTextSize(1);
          display.setTextColor(SSD1306_WHITE);
          
          if (appName.length() > 0) {
            display.setCursor(0, 0);
            String header = appName;
            if (sender.length() > 0) {
              header += ": " + sender;
            }
            if (header.length() > 21) {
              header = header.substring(0, 18) + "...";
            }
            display.print(header);
            
            display.setCursor(0, 12);
            if (message.length() > 21) {
              message = message.substring(0, 18) + "...";
            }
            display.print(message);
          } else {
            display.setCursor(0, 8);
            display.print("No messages");
          }
          display.display();
          
          Serial.println("[SINGLE-CLICK] Show message");
        }
      }
      
      clickCount = 0;
    }
    
    // ============== DISPLAY TIMEOUT ==============
    if (deviceState == STATE_CONNECTED && displayActive && (millis() - displayOnTime) >= DISPLAY_TIMEOUT) {
      display.clearDisplay();
      display.display();
      displayActive = false;
      
      if (xSemaphoreTake(messageMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lastAppName = "";
        lastSender = "";
        lastMessage = "";
        xSemaphoreGive(messageMutex);
      }
      
      Serial.println("[DISPLAY] Timeout");
    }
    
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ============== BLE CALLBACKS ==============

void rx_write_callback(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  for (uint16_t i = 0; i < len; i++) {
    messageBuffer += (char)data[i];
  }
  lastChunkTime = millis();
  
  digitalWrite(LED_PIN, LOW);
  delay(50);
  digitalWrite(LED_PIN, HIGH);
}

void connect_callback(uint16_t conn_handle) {
  // Get connected device address using Bluefruit API
  BLEConnection* connection = Bluefruit.Connection(conn_handle);
  if (!connection) {
    Serial.println("[BLE] Failed to get connection");
    return;
  }
  
  ble_gap_addr_t peerAddr = connection->getPeerAddr();
  
  Serial.print("[BLE] Device connecting: ");
  for (int i = 5; i >= 0; i--) {
    Serial.print(peerAddr.addr[i], HEX);
    if (i > 0) Serial.print(":");
  }
  Serial.println();
  
  // Check if device is paired and if this is the paired device
  if (config.isPaired) {
    if (!isAddressMatch(peerAddr.addr, config.pairedAddress)) {
      Serial.println("[BLE] Rejecting connection - not paired device");
      Bluefruit.disconnect(conn_handle);
      return;
    }
    Serial.println("[BLE] Paired device connected");
  } else {
    Serial.println("[BLE] Unpaired - accepting any connection");
  }
  
  digitalWrite(LED_PIN, HIGH);
  bleConnected = true;
}

void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  Serial.println("[BLE] Device disconnected");
  digitalWrite(LED_PIN, LOW);
  bleConnected = false;
}

// ============== SETUP ==============

void setup() {
  Serial.begin(115200);
  delay(100);
  
  Serial.println("\n=== Starting CLIPP ===");
  
  // Load configuration from flash
  loadConfig();
  
  // Initialize LED pin
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  // Initialize button
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // Initialize vibration motor
  pinMode(VIBRATOR_PIN, OUTPUT);
  digitalWrite(VIBRATOR_PIN, LOW);

  // Initialize MCP STAT pins (active-low)
  pinMode(STAT1_PIN, INPUT_PULLUP);
  pinMode(STAT2_PIN, INPUT_PULLUP);
  
  // Initialize OLED display
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("SSD1306 allocation failed");
  } else {
    Serial.println("OLED display initialized");
    setDisplayBrightness(config.brightness);
  }
  
  // Initialize LC709203F fuel gauge
  if (!lc.begin()) {
    Serial.println("LC709203F fuel gauge failed to initialize");
    fuelGaugeInitialized = false;
  } else {
    Serial.println("LC709203F fuel gauge initialized");
    lc.setPowerMode(LC709203F_POWER_OPERATE);
    lc.setTemperatureMode(LC709203F_TEMPERATURE_THERMISTOR);
    lc.setThermistorB(3950);  // Typical thermistor B coefficient
    lc.setPackSize(LC709203F_APA_100MAH);  // EL 371030 - 80mAh battery (closest to 100mAh)
    lc.initRSOC();
    fuelGaugeInitialized = true;
  }
  
  // Show boot screen
  deviceState = STATE_BOOT;
  showBootScreen();
  delay(2000);
  
  // Initialize ADC
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  
  // Initialize Bluefruit
  Bluefruit.begin();
  Bluefruit.setTxPower(4);
  Bluefruit.setName(config.deviceName);
  
  Serial.print("[BLE] Device name: ");
  Serial.println(config.deviceName);
  Serial.print("[BLE] Paired: ");
  Serial.println(config.isPaired ? "yes" : "no");
  
  // Set connection callbacks
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);
  
  // Configure BLE Service
  bleService.begin();
  
  // Configure RX Characteristic
  rxCharacteristic.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
  rxCharacteristic.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);
  rxCharacteristic.setMaxLen(244);
  rxCharacteristic.setWriteCallback(rx_write_callback);
  rxCharacteristic.begin();
  
  // Configure TX Characteristic
  txCharacteristic.setProperties(CHR_PROPS_NOTIFY);
  txCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  txCharacteristic.setMaxLen(244);
  txCharacteristic.begin();
  
  // Start advertising
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleService);
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);
  Bluefruit.Advertising.setFastTimeout(30);
  Bluefruit.Advertising.start(0);
  
  Serial.println("[BLE] Advertising...");
  
  // Show not connected screen
  deviceState = STATE_NOT_CONNECTED;
  showNotConnected();
  
  // Create mutexes
  batteryMutex = xSemaphoreCreateMutex();
  messageMutex = xSemaphoreCreateMutex();
  
  // Initial fuel gauge reading
  readFuelGauge();
  
  // Create FreeRTOS tasks
  xTaskCreate(bleTask, "BLE", 512, NULL, 2, &bleTaskHandle);
  xTaskCreate(batteryTask, "Battery", 256, NULL, 1, &batteryTaskHandle);
  xTaskCreate(buttonTask, "Button", 512, NULL, 1, &buttonTaskHandle);
  
  Serial.println("FreeRTOS tasks started");
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
