/*
 * PLATFORM: PlatformIO with Seeed XIAO nRF52840
 * LIBRARY: Adafruit Bluefruit nRF52 (native Nordic BLE stack)
 *
 * TASK: Create a BLE Peripheral setup.
 * 1. Define Service UUID: "19B10000-E8F2-537E-4F6C-D104768A1214"
 * 2. Define RX Characteristic (Write): "19B10001-E8F2-537E-4F6C-D104768A1214"
 * -> Logic: When data is written here, print it to Serial and toggle the built-in LED.
 * 3. Define TX Characteristic (Notify): "19B10002-E8F2-537E-4F6C-D104768A1214"
 * -> Logic: Send a "Counter: [x]" string to the app every 2 seconds using non-blocking delay (millis).
 * 4. Setup standard setup() and loop() functions.
 */

#include <Arduino.h>
#include <bluefruit.h>

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
}

void loop() {
  // Check if we're connected
  if (Bluefruit.connected()) {
    // Timeout incomplete messages after 1 second of inactivity
    if (messageBuffer.length() > 0 && (millis() - lastChunkTime) > CHUNK_TIMEOUT) {
      Serial.println("\n=== MESSAGE RECEIVED ===");
      Serial.print("Raw: ");
      Serial.println(messageBuffer);
      
      // Parse and display message
      if (messageBuffer.startsWith("CALL:")) {
        // Format: CALL:+1234567890:45s
        int firstColon = messageBuffer.indexOf(":");
        int lastColon = messageBuffer.lastIndexOf(":");
        
        if (firstColon != -1 && lastColon != -1 && firstColon < lastColon) {
          String phoneNumber = messageBuffer.substring(firstColon + 1, lastColon);
          String duration = messageBuffer.substring(lastColon + 1);
          
          Serial.print("INCOMING CALL FROM: ");
          Serial.print(phoneNumber);
          Serial.print(" | Duration: ");
          Serial.println(duration);
        }
      } else if (messageBuffer.startsWith("NOTIF:")) {
        // Format: NOTIF:com.whatsapp:New message:Hello there
        int firstColon = messageBuffer.indexOf(":");
        int secondColon = messageBuffer.indexOf(":", firstColon + 1);
        
        if (firstColon != -1 && secondColon != -1) {
          String appName = messageBuffer.substring(firstColon + 1, secondColon);
          String content = messageBuffer.substring(secondColon + 1);
          
          Serial.print("NOTIFICATION | App: ");
          Serial.print(appName);
          Serial.print(" | Content: ");
          Serial.println(content);
        }
      } else {
        // Regular user message
        Serial.print("USER MESSAGE: ");
        Serial.println(messageBuffer);
      }
      
      Serial.println("======================\n");
      messageBuffer = "";
    }
  }
  
  delay(10);
}