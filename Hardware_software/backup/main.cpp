/*
 * PLATFORM: PlatformIO with Seeed XIAO nRF52840
 * LIBRARY: Adafruit Bluefruit nRF52 (native Nordic BLE stack)
 * 
 * Uses FreeRTOS for concurrent BLE, battery monitoring, and button handling
 */

#include <Arduino.h>
#include <bluefruit.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

// OLED Display settings (0.91" 128x32 I2C)
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32
#define OLED_RESET -1
#define SCREEN_ADDRESS 0x3C
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);

// Button pin
const int BUTTON_PIN = D0;

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

// Battery monitoring constants
const int BATTERY_BUFFER_SIZE = 8;
uint16_t batteryBuffer[BATTERY_BUFFER_SIZE] = {0};
int batteryBufferIndex = 0;
uint16_t lastReportedBattery = 0;

// Read battery voltage using internal VDD measurement (more stable than GPIO ADC)
uint16_t readBatteryVoltage() {
  // Use internal 3.0V reference and measure VDD/4
  // This gives us the actual supply voltage which comes from the battery
  analogReference(AR_INTERNAL_3_0);
  analogReadResolution(12);
  
  // Take multiple samples
  const int numSamples = 16;
  long sum = 0;
  
  for (int i = 0; i < numSamples; i++) {
    // Read from internal VDD channel (PIN_VBAT is defined in variant for nRF52)
    // On XIAO nRF52840, we read the regulated output which tracks battery
    sum += analogRead(PIN_VBAT);
    delayMicroseconds(100);
  }
  
  int rawValue = sum / numSamples;
  
  // Convert to millivolts: (raw / 4096) * 3.0V * 4 (because VDD/4 is measured)
  // = raw * 3000 * 4 / 4096 = raw * 12000 / 4096 = raw * 2.9296875
  uint16_t batteryVoltage = (uint32_t)rawValue * 3000 * 4 / 4096;
  
  // Add to rolling buffer for median filtering
  batteryBuffer[batteryBufferIndex] = batteryVoltage;
  batteryBufferIndex = (batteryBufferIndex + 1) % BATTERY_BUFFER_SIZE;
  
  // Sort buffer and return median to reject outliers
  uint16_t sortedBuffer[BATTERY_BUFFER_SIZE];
  for (int i = 0; i < BATTERY_BUFFER_SIZE; i++) {
    sortedBuffer[i] = batteryBuffer[i];
  }
  
  // Simple bubble sort
  for (int i = 0; i < BATTERY_BUFFER_SIZE - 1; i++) {
    for (int j = 0; j < BATTERY_BUFFER_SIZE - i - 1; j++) {
      if (sortedBuffer[j] > sortedBuffer[j + 1]) {
        uint16_t temp = sortedBuffer[j];
        sortedBuffer[j] = sortedBuffer[j + 1];
        sortedBuffer[j + 1] = temp;
      }
    }
  }
  
  // Return median value
  return sortedBuffer[BATTERY_BUFFER_SIZE / 2];
}

// Function to calculate battery percentage (3.7V LiPo: 4.2V full, 2.8V empty)
uint8_t calculateBatteryPercentage(uint16_t millivolts) {
  const uint16_t MAX_MV = 4200;  // 4.2V fully charged
  const uint16_t MIN_MV = 2800;  // 2.8V cutoff
  
  if (millivolts >= MAX_MV) return 100;
  if (millivolts <= MIN_MV) return 0;
  
  uint8_t percentage = ((millivolts - MIN_MV) * 100) / (MAX_MV - MIN_MV);
  return percentage;
}

// ============== FreeRTOS TASKS ==============

// Task: Handle BLE message processing (high priority)
void bleTask(void *pvParameters) {
  (void) pvParameters;
  
  while (1) {
    if (Bluefruit.connected()) {
      // Timeout incomplete messages after 1 second of inactivity
      if (messageBuffer.length() > 0 && (millis() - lastChunkTime) > CHUNK_TIMEOUT) {
        
        // Parse message and store for display
        if (messageBuffer.startsWith("CALL:")) {
          // Format: CALL:+1234567890:45s
          int firstColon = messageBuffer.indexOf(":");
          int lastColon = messageBuffer.lastIndexOf(":");
          
          if (firstColon != -1 && lastColon != -1 && firstColon < lastColon) {
            String phoneNumber = messageBuffer.substring(firstColon + 1, lastColon);
            String duration = messageBuffer.substring(lastColon + 1);
            
            // Store for display
            if (xSemaphoreTake(messageMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              lastAppName = "Call";
              lastSender = phoneNumber;
              lastMessage = "Duration: " + duration;
              xSemaphoreGive(messageMutex);
            }
            
            Serial.print("[CALL] From: ");
            Serial.println(phoneNumber);
          }
        } else if (messageBuffer.startsWith("NOTIF:")) {
          // Format: NOTIF:AppName:Sender:Content or NOTIF:AppName:Title:Content
          int firstColon = messageBuffer.indexOf(":");
          int secondColon = messageBuffer.indexOf(":", firstColon + 1);
          int thirdColon = messageBuffer.indexOf(":", secondColon + 1);
          
          if (firstColon != -1 && secondColon != -1) {
            String appName = messageBuffer.substring(firstColon + 1, secondColon);
            String sender = "";
            String content = "";
            
            if (thirdColon != -1) {
              // Has sender/title and content: NOTIF:App:Sender:Content
              sender = messageBuffer.substring(secondColon + 1, thirdColon);
              content = messageBuffer.substring(thirdColon + 1);
            } else {
              // Just app and content, no sender: NOTIF:App:Content
              content = messageBuffer.substring(secondColon + 1);
              sender = "";
            }
            
            // Clean up app name (remove com. prefix if present)
            if (appName.startsWith("com.")) {
              int lastDot = appName.lastIndexOf(".");
              if (lastDot > 4) {
                appName = appName.substring(lastDot + 1);
              }
            }
            // Capitalize first letter
            if (appName.length() > 0) {
              appName.setCharAt(0, toupper(appName.charAt(0)));
            }
            
            // Store for display
            if (xSemaphoreTake(messageMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              lastAppName = appName;
              lastSender = sender;
              lastMessage = content;
              xSemaphoreGive(messageMutex);
            }
            
            Serial.print("[NOTIF] ");
            Serial.print(appName);
            Serial.print(": ");
            Serial.println(sender);
          }
        }
        
        messageBuffer = "";
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(10));  // Check every 10ms
  }
}

// Task: Battery monitoring (low priority, runs every 10 seconds)
void batteryTask(void *pvParameters) {
  (void) pvParameters;
  
  while (1) {
    uint16_t batteryMV = readBatteryVoltage();
    
    // Only report if value has changed significantly (dead-band of 30mV)
    if (abs((int)batteryMV - (int)lastReportedBattery) >= 30 || lastReportedBattery == 0) {
      lastReportedBattery = batteryMV;
      uint8_t batteryPercent = calculateBatteryPercentage(batteryMV);
      
      // Update shared battery data (protected by mutex)
      if (xSemaphoreTake(batteryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        sharedBatteryMV = batteryMV;
        sharedBatteryPercent = batteryPercent;
        xSemaphoreGive(batteryMutex);
      }
      
      Serial.print("[BATTERY] ");
      Serial.print(batteryMV);
      Serial.print("mV (");
      Serial.print(batteryPercent);
      Serial.println("%)");
      
      // Send battery status to app via TX characteristic if connected
      if (Bluefruit.connected()) {
        String batteryStatus = "BATT:" + String(batteryPercent) + "%:" + String(batteryMV) + "mV";
        txCharacteristic.notify(batteryStatus.c_str(), batteryStatus.length());
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(10000));  // Check every 10 seconds
  }
}

// Task: Button and display handling (medium priority)
void buttonTask(void *pvParameters) {
  (void) pvParameters;
  
  bool lastButtonState = HIGH;
  bool displayActive = false;
  unsigned long displayOnTime = 0;
  const unsigned long DISPLAY_TIMEOUT = 5000;
  
  // Double-click detection
  unsigned long lastClickTime = 0;
  int clickCount = 0;
  const unsigned long DOUBLE_CLICK_TIME = 400;  // Max time between clicks for double-click
  const unsigned long CLICK_WAIT_TIME = 450;    // Time to wait before processing single click
  bool waitingForSecondClick = false;
  
  while (1) {
    bool currentButtonState = digitalRead(BUTTON_PIN);
    
    // Button press detection (active LOW with pull-up)
    if (currentButtonState == LOW && lastButtonState == HIGH) {
      unsigned long now = millis();
      
      // Check for double-click first (works whether display is on or off)
      if (waitingForSecondClick && (now - lastClickTime) < DOUBLE_CLICK_TIME) {
        // Double-click detected - show battery
        clickCount = 2;
        waitingForSecondClick = false;
        
        displayOnTime = millis();
        displayActive = true;
        
        // Get battery data
        uint8_t batteryPercent = 0;
        if (xSemaphoreTake(batteryMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          batteryPercent = sharedBatteryPercent;
          xSemaphoreGive(batteryMutex);
        }
        
        // Show battery on OLED
        display.clearDisplay();
        display.setTextSize(2);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 0);
        display.print("Battery:");
        display.setCursor(0, 18);
        display.print(batteryPercent);
        display.print("%");
        display.display();
        
        Serial.print("[DOUBLE-CLICK] Battery: ");
        Serial.print(batteryPercent);
        Serial.println("%");
      } else {
        // First click - wait for possible second click
        lastClickTime = now;
        waitingForSecondClick = true;
        clickCount = 1;
      }
    }
    lastButtonState = currentButtonState;
    
    // Check if single-click wait time expired (no second click came)
    if (waitingForSecondClick && (millis() - lastClickTime) >= CLICK_WAIT_TIME) {
      waitingForSecondClick = false;
      
      // If display is on, turn it off (single click dismisses)
      if (displayActive) {
        display.clearDisplay();
        display.display();
        displayActive = false;
        
        // Clear the last message so next single-click shows "No messages"
        if (xSemaphoreTake(messageMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          lastAppName = "";
          lastSender = "";
          lastMessage = "";
          xSemaphoreGive(messageMutex);
        }
        
        Serial.println("[SINGLE-CLICK] Display off - message cleared");
      } else {
        // Display was off - show last message
        displayOnTime = millis();
        displayActive = true;
        
        // Get message data
        String appName = "";
        String sender = "";
        String message = "";
        if (xSemaphoreTake(messageMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
          appName = lastAppName;
          sender = lastSender;
          message = lastMessage;
          xSemaphoreGive(messageMutex);
        }
        
        // Show message on OLED
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        
        if (appName.length() > 0) {
          // Line 1: App: Sender (truncate if needed, max ~21 chars)
          display.setCursor(0, 0);
          String header = appName;
          if (sender.length() > 0) {
            header += ": " + sender;
          }
          if (header.length() > 21) {
            header = header.substring(0, 18) + "...";
          }
          display.print(header);
          
          // Line 2: Message content (truncate with ... if too long)
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
        
        Serial.print("[SINGLE-CLICK] ");
        Serial.print(appName);
        Serial.print(": ");
        Serial.println(sender);
      }
    }
    
    // Turn off display after timeout
    if (displayActive && (millis() - displayOnTime) >= DISPLAY_TIMEOUT) {
      display.clearDisplay();
      display.display();
      displayActive = false;
      
      // Clear the last message so next single-click shows "No messages"
      if (xSemaphoreTake(messageMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        lastAppName = "";
        lastSender = "";
        lastMessage = "";
        xSemaphoreGive(messageMutex);
      }
      
      Serial.println("[DISPLAY] Off - message cleared");
    }
    
    vTaskDelay(pdMS_TO_TICKS(20));  // Check button every 20ms
  }
}
// Callback when data is written to RX characteristic
void rx_write_callback(uint16_t conn_hdl, BLECharacteristic* chr, uint8_t* data, uint16_t len) {
  Serial.print("[RX Callback] Received chunk (");
  Serial.print(len);
  Serial.print(" bytes): ");
  
  // Append chunk to buffer
  for (uint16_t i = 0; i < len; i++) {
    messageBuffer += (char)data[i];
  }
  Serial.println(messageBuffer);  // Show accumulated message so far
  
  lastChunkTime = millis();
  
  // Blink LED to indicate reception
  digitalWrite(LED_PIN, LOW);
  delay(50);
  digitalWrite(LED_PIN, HIGH);
}

// Callback when a device connects
void connect_callback(uint16_t conn_handle) {
  Serial.println("Device connected!");
  digitalWrite(LED_PIN, HIGH);
}

// Callback when a device disconnects
void disconnect_callback(uint16_t conn_handle, uint8_t reason) {
  Serial.println("Device disconnected");
  digitalWrite(LED_PIN, LOW);
}

void setup() {
  // Initialize Serial for debugging
  Serial.begin(115200);
  while (!Serial) {
    delay(10);
  }
  
  Serial.println("\n=== Starting BLE Peripheral ===");
  Serial.println("Service UUID: 819b2f01-9d7d-42f1-a58e-29e5c07dd6b6");
  Serial.println("RX UUID: 5600f473-a667-4c60-b0b1-7c68fbe9720f (WRITE)");
  Serial.println("TX UUID: b2fd3be9-40ca-48cd-80ed-5d04b2a71da2 (NOTIFY)");
  
  // Initialize LED pin
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  
  // Initialize button with internal pull-up
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  
  // Initialize OLED display
  Wire.begin();
  if (!display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS)) {
    Serial.println("SSD1306 allocation failed");
  } else {
    Serial.println("OLED display initialized");
    display.clearDisplay();
    display.display();  // Start with display off (cleared)
  }
  
  // Initialize ADC for internal VDD battery reading
  analogReference(AR_INTERNAL_3_0);  // Use internal 3.0V reference
  analogReadResolution(12);          // 12-bit resolution
  
  // Initialize Bluefruit
  Bluefruit.begin();
  Bluefruit.setTxPower(4);  // Max power
  Bluefruit.setName("nRF52840-BLE");
  
  // Set connection callbacks
  Bluefruit.Periph.setConnectCallback(connect_callback);
  Bluefruit.Periph.setDisconnectCallback(disconnect_callback);
  
  // Configure and Start BLE Service
  bleService.begin();
  
  // Configure RX Characteristic (Write)
  rxCharacteristic.setProperties(CHR_PROPS_WRITE | CHR_PROPS_WRITE_WO_RESP);
  rxCharacteristic.setPermission(SECMODE_NO_ACCESS, SECMODE_OPEN);
  rxCharacteristic.setMaxLen(244);  // Max payload is 244 bytes (MTU 247 - 3 bytes header)
  rxCharacteristic.setWriteCallback(rx_write_callback);
  rxCharacteristic.begin();
  Serial.println("RX Characteristic configured (UUID: 5600f473-a667-4c60-b0b1-7c68fbe9720f)");
  
  // Configure TX Characteristic (Notify)
  txCharacteristic.setProperties(CHR_PROPS_NOTIFY);
  txCharacteristic.setPermission(SECMODE_OPEN, SECMODE_NO_ACCESS);
  txCharacteristic.setMaxLen(244);  // Match RX max length
  txCharacteristic.begin();
  
  // Start advertising
  Bluefruit.Advertising.addFlags(BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE);
  Bluefruit.Advertising.addTxPower();
  Bluefruit.Advertising.addService(bleService);
  Bluefruit.Advertising.addName();
  Bluefruit.Advertising.restartOnDisconnect(true);
  Bluefruit.Advertising.setInterval(32, 244);    // in unit of 0.625 ms
  Bluefruit.Advertising.setFastTimeout(30);      // seconds
  Bluefruit.Advertising.start(0);                // 0 = Don't stop advertising
  
  Serial.println("BLE device advertising...");
  
  // Create mutexes for shared data
  batteryMutex = xSemaphoreCreateMutex();
  messageMutex = xSemaphoreCreateMutex();
  
  // Take an initial battery reading
  uint16_t initialBattery = readBatteryVoltage();
  sharedBatteryMV = initialBattery;
  sharedBatteryPercent = calculateBatteryPercentage(initialBattery);
  lastReportedBattery = initialBattery;
  
  // Create FreeRTOS tasks
  xTaskCreate(
    bleTask,           // Task function
    "BLE",             // Task name
    256,               // Stack size (words)
    NULL,              // Parameters
    2,                 // Priority (higher = more important)
    &bleTaskHandle     // Task handle
  );
  
  xTaskCreate(
    batteryTask,       // Task function
    "Battery",         // Task name
    256,               // Stack size
    NULL,              // Parameters
    1,                 // Priority (low)
    &batteryTaskHandle // Task handle
  );
  
  xTaskCreate(
    buttonTask,        // Task function
    "Button",          // Task name
    256,               // Stack size
    NULL,              // Parameters
    1,                 // Priority (low)
    &buttonTaskHandle  // Task handle
  );
  
  Serial.println("FreeRTOS tasks started");
}

void loop() {
  // Empty - all work is done in FreeRTOS tasks
  // The scheduler handles task switching automatically
  vTaskDelay(pdMS_TO_TICKS(1000));
}