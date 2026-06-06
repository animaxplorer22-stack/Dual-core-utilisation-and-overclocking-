

// ==================== USER CONFIGURATION ====================
const char* ssid     = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";
const char* ducoUser = "YOUR_DUCO_USERNAME";
const char* miningKey = "";  // Leave empty unless you have a specific key
const char* rigName = "OptimizedMiner";

// Mining settings
const int baseDifficulty = 0;     // 0 = auto (recommended)
const int maxNonceAttempts = 1000000;  // Safety limit

#ifdef ESP32
  #include <WiFi.h>
  #include <HTTPClient.h>
  #include <mbedtls/sha1.h>
  #define BOARD_TYPE "ESP32"
  #define DUAL_CORE_AVAILABLE 1
#elif defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <ESP8266HTTPClient.h>
  #include <ESP8266mDNS.h>
  #include <bearssl/bearssl.h>
  #include <bearssl/bearssl_hash.h>
  #define BOARD_TYPE "ESP8266"
  #define DUAL_CORE_AVAILABLE 0
#endif

// ==================== GLOBAL VARIABLES ====================
WiFiClient client;
const char* server = "server.duinocoin.com";
const int port = 2812;

// Mining state
volatile bool jobAvailable = false;
volatile bool miningActive = true;
String lastBlockHash = "";
String expectedHash = "";
int currentDifficulty = 10;
unsigned long currentNonce = 0;

// Statistics
unsigned long totalHashes = 0;
unsigned long acceptedShares = 0;
unsigned long rejectedShares = 0;
float currentHashrate = 0;
unsigned long lastStatsTime = 0;
unsigned long lastShareTime = 0;

// Performance tracking
unsigned long hashStartTime = 0;
unsigned long hashesInCurrentSecond = 0;


// Fast hex conversion (avoids String overhead)
void bytesToHex(const uint8_t* data, size_t len, char* output) {
  const char hexChars[] = "0123456789abcdef";
  for (size_t i = 0; i < len; i++) {
    output[i * 2] = hexChars[(data[i] >> 4) & 0x0F];
    output[i * 2 + 1] = hexChars[data[i] & 0x0F];
  }
  output[len * 2] = '\0';
}

// Convert hex string to bytes
bool hexToBytes(const char* hex, uint8_t* output, size_t outputLen) {
  size_t hexLen = strlen(hex);
  if (hexLen != outputLen * 2) return false;
  
  for (size_t i = 0; i < outputLen; i++) {
    char high = hex[i * 2];
    char low = hex[i * 2 + 1];
    
    uint8_t highVal = (high >= '0' && high <= '9') ? high - '0' : 
                      (high >= 'a' && high <= 'f') ? high - 'a' + 10 :
                      (high >= 'A' && high <= 'F') ? high - 'A' + 10 : 0;
    uint8_t lowVal = (low >= '0' && low <= '9') ? low - '0' :
                     (low >= 'a' && low <= 'f') ? low - 'a' + 10 :
                     (low >= 'A' && low <= 'F') ? low - 'A' + 10 : 0;
    
    output[i] = (highVal << 4) | lowVal;
  }
  return true;
}

// ==================== HASHING FUNCTIONS ====================

#ifdef ESP32
// ESP32 uses mbedtls hardware-accelerated SHA1
bool computeSHA1(const uint8_t* data, size_t len, uint8_t* output) {
  mbedtls_sha1_context ctx;
  mbedtls_sha1_init(&ctx);
  mbedtls_sha1_starts(&ctx);
  mbedtls_sha1_update(&ctx, data, len);
  mbedtls_sha1_finish(&ctx, output);
  mbedtls_sha1_free(&ctx);
  return true;
}
#else
// ESP8266 uses BearSSL SHA1
bool computeSHA1(const uint8_t* data, size_t len, uint8_t* output) {
  br_sha1_context ctx;
  br_sha1_init(&ctx);
  br_sha1_update(&ctx, data, len);
  br_sha1_out(&ctx, output);
  return true;
}
#endif

// Core mining function - optimized tight loop
bool mineBlock(uint32_t startNonce, uint32_t endNonce, uint32_t* foundNonce) {
  if (lastBlockHash.length() < 40) return false;
  
  // Convert job data once
  uint8_t lastHashBytes[20];
  if (!hexToBytes(lastBlockHash.c_str(), lastHashBytes, 20)) {
    return false;
  }
  
  // Prepare expected hash string for comparison
  char expectedHashLower[41];
  expectedHash.toLowerCase();
  expectedHash.toCharArray(expectedHashLower, 41);
  
  uint32_t nonce = startNonce;
  uint8_t hashResult[20];
  char resultHex[41];
  
  // Mining loop - the critical path
  while (nonce < endNonce && miningActive && jobAvailable) {
    // Build and hash the block
    #ifdef ESP32
      mbedtls_sha1_context ctx;
      mbedtls_sha1_init(&ctx);
      mbedtls_sha1_starts(&ctx);
      mbedtls_sha1_update(&ctx, lastHashBytes, 20);
      mbedtls_sha1_update(&ctx, (const uint8_t*)&nonce, 4);
      mbedtls_sha1_finish(&ctx, hashResult);
      mbedtls_sha1_free(&ctx);
    #else
      br_sha1_context ctx;
      br_sha1_init(&ctx);
      br_sha1_update(&ctx, lastHashBytes, 20);
      br_sha1_update(&ctx, (const uint8_t*)&nonce, 4);
      br_sha1_out(&ctx, hashResult);
    #endif
    
    bytesToHex(hashResult, 20, resultHex);
    
    totalHashes++;
    hashesInCurrentSecond++;
    
    // Check if we found a valid share
    if (strcmp(resultHex, expectedHashLower) == 0) {
      *foundNonce = nonce;
      return true;
    }
    
    nonce++;
    
    // Yield periodically to prevent watchdog issues
    if ((nonce & 0xFFF) == 0) {  // Every 4096 hashes
      yield();
      #ifdef ESP8266
        ESP.wdtFeed();
      #endif
    }
  }
  
  return false;
}

// ==================== NETWORK FUNCTIONS ====================

bool getMiningJob() {
  if (!client.connect(server, port)) {
    Serial.println("[Network] Connection failed");
    return false;
  }
  
  // Request job: JOB,username,key,rigName,difficulty
  String request = "JOB," + String(ducoUser) + "," + String(miningKey) + "," + 
                   String(rigName) + "," + String(baseDifficulty) + "\n";
  client.print(request);
  
  unsigned long timeout = millis() + 10000;
  while (!client.available() && millis() < timeout) {
    delay(10);
  }
  
  if (client.available()) {
    String response = client.readStringUntil('\n');
    response.trim();
    
    // Format: last_hash,expected_hash,difficulty
    int firstComma = response.indexOf(',');
    int secondComma = response.indexOf(',', firstComma + 1);
    
    if (firstComma > 0 && secondComma > 0) {
      lastBlockHash = response.substring(0, firstComma);
      expectedHash = response.substring(firstComma + 1, secondComma);
      currentDifficulty = response.substring(secondComma + 1).toInt();
      
      if (currentDifficulty < 1) currentDifficulty = 10;
      
      Serial.println("\n[Job] New mining job received");
      Serial.print("  Difficulty: "); Serial.println(currentDifficulty);
      Serial.print("  Target: "); Serial.println(expectedHash);
      
      jobAvailable = true;
      currentNonce = 0;
      client.stop();
      return true;
    }
  }
  
  client.stop();
  return false;
}

bool submitShare(uint32_t nonce) {
  if (!client.connect(server, port)) {
    Serial.println("[Network] Submit connection failed");
    return false;
  }
  
  String request = "SHARE," + String(ducoUser) + "," + String(miningKey) + "," + 
                   String(nonce) + "," + String(rigName) + "\n";
  client.print(request);
  
  unsigned long timeout = millis() + 15000;
  while (!client.available() && millis() < timeout) {
    delay(10);
  }
  
  if (client.available()) {
    String response = client.readStringUntil('\n');
    response.trim();
    
    if (response == "GOOD") {
      acceptedShares++;
      lastShareTime = millis();
      Serial.println("[Share] ACCEPTED!");
      client.stop();
      return true;
    } else {
      rejectedShares++;
      Serial.print("[Share] REJECTED: ");
      Serial.println(response);
      client.stop();
      return false;
    }
  }
  
  rejectedShares++;
  client.stop();
  return false;
}

// ==================== ESP32 DUAL CORE TASK ====================

#ifdef ESP32
TaskHandle_t MiningTaskHandle;

void miningTask(void* parameter) {
  Serial.println("[Core1] Mining thread started");
  
  while (true) {
    if (jobAvailable && miningActive) {
      uint32_t foundNonce = 0;
      uint32_t searchRange = currentDifficulty * 100;
      if (searchRange < 1000) searchRange = 1000;
      if (searchRange > 500000) searchRange = 500000;
      
      if (mineBlock(currentNonce, currentNonce + searchRange, &foundNonce)) {
        if (submitShare(foundNonce)) {
          jobAvailable = false;
        }
      }
      currentNonce += searchRange;
      
      // Reset job if we've searched too far
      if (currentNonce > 10000000) {
        jobAvailable = false;
      }
    } else {
      delay(1);
    }
  }
}
#endif

// ==================== CONNECTION MANAGEMENT ====================

void connectWiFi() {
  Serial.print("[WiFi] Connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected!");
    Serial.print("  IP: "); Serial.println(WiFi.localIP());
    Serial.print("  RSSI: "); Serial.print(WiFi.RSSI()); Serial.println(" dBm");
  } else {
    Serial.println("\n[WiFi] Failed to connect!");
    ESP.restart();
  }
}

void printStats() {
  Serial.println("\n╔════════════════════════════════════╗");
  Serial.print  ("║  Board: "); Serial.print(BOARD_TYPE);
  for(int i = 0; i < 20 - strlen(BOARD_TYPE); i++) Serial.print(" ");
  Serial.println("║");
  
  Serial.print("║  Hashrate: ");
  Serial.print(currentHashrate, 0);
  Serial.print(" H/s");
  for(int i = 0; i < 14 - String(currentHashrate,0).length(); i++) Serial.print(" ");
  Serial.println("║");
  
  Serial.print("║  Shares: A/");
  Serial.print(acceptedShares);
  Serial.print(" R/");
  Serial.print(rejectedShares);
  for(int i = 0; i < 12 - String(acceptedShares).length() - String(rejectedShares).length(); i++) Serial.print(" ");
  Serial.println("║");
  
  Serial.print("║  Uptime: ");
  unsigned long uptime = millis() / 1000;
  Serial.print(uptime / 3600); Serial.print("h ");
  Serial.print((uptime % 3600) / 60); Serial.print("m ");
  Serial.print(uptime % 60); Serial.print("s");
  for(int i = 0; i < 8; i++) Serial.print(" ");
  Serial.println("║");
  
  Serial.println("╚════════════════════════════════════╝");
}

// ==================== SETUP ====================

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n\n╔══════════════════════════════════════╗");
  Serial.println("║   DUCO-S1 Optimized Miner v2.0     ║");
  Serial.println("╚══════════════════════════════════════╝");
  
  // Board-specific optimizations
  #ifdef ESP32
    Serial.println("[ESP32] Detected - Applying optimizations");
    setCpuFrequencyMhz(240);
    Serial.print("  CPU Frequency: "); Serial.print(getCpuFrequencyMhz()); Serial.println(" MHz");
  #elif defined(ESP8266)
    Serial.println("[ESP8266] Detected - Applying optimizations");
    system_update_cpu_freq(160);
    Serial.print("  CPU Frequency: "); Serial.print(ESP.getCpuFreqMHz()); Serial.println(" MHz");
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
  #endif
  
  connectWiFi();
  
  // Get initial job
  getMiningJob();
  
  #ifdef ESP32
    // Start mining on Core 1
    xTaskCreatePinnedToCore(
      miningTask,
      "MinerCore",
      8192,
      NULL,
      3,
      &MiningTaskHandle,
      1
    );
    Serial.println("[ESP32] Dual-core mining enabled");
  #endif
  
  lastStatsTime = millis();
  hashStartTime = micros();
  Serial.println("\n[Miner] Ready! Mining started.\n");
}

// ==================== MAIN LOOP ====================

void loop() {
  // Update hashrate calculation
  unsigned long now = millis();
  if (now - lastStatsTime >= 5000) {
    unsigned long elapsedMicros = micros() - hashStartTime;
    if (elapsedMicros > 0) {
      currentHashrate = (totalHashes * 1000000.0) / elapsedMicros;
    }
    printStats();
    lastStatsTime = now;
  }
  
  #ifndef ESP32
    // ESP8266 does mining in the main loop (single core)
    if (jobAvailable && miningActive) {
      uint32_t foundNonce = 0;
      uint32_t searchRange = currentDifficulty * 100;
      if (searchRange < 1000) searchRange = 1000;
      if (searchRange > 500000) searchRange = 500000;
      
      if (mineBlock(currentNonce, currentNonce + searchRange, &foundNonce)) {
        if (submitShare(foundNonce)) {
          jobAvailable = false;
        }
      }
      currentNonce += searchRange;
      
      if (currentNonce > 10000000) {
        jobAvailable = false;
      }
    } else if (!jobAvailable) {
      // Get new job
      getMiningJob();
      currentNonce = 0;
      delay(100);
    }
  #else
    // ESP32 - just monitor and get new jobs when needed
    if (!jobAvailable) {
      getMiningJob();
      currentNonce = 0;
      delay(100);
    }
  #endif
  
  // Check WiFi connection periodically
  static unsigned long lastWiFiCheck = 0;
  if (now - lastWiFiCheck > 30000) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("[WiFi] Connection lost! Reconnecting...");
      connectWiFi();
    }
    lastWiFiCheck = now;
  }
  
  delay(5);  // Small delay to prevent watchdog issues
}put this into a text box for me to easily copy
