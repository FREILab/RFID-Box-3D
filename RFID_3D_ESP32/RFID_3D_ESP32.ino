/**
 * @file main.cpp
 * @brief ESP32 RFID Login System with Enhanced Logging
 *
 * This code initializes an ESP32 to perform RFID-based login operations.
 * It connects to WiFi, initializes an MFRC522 RFID reader, reads RFID cards,
 * and sends HTTP GET requests to a server for login and session extension.
 *
 * Wiring details and configuration values (e.g., WiFi credentials, server settings)
 * are provided in "config_3D.h".
 *
 * This version uses the ArduinoLog library for structured logging.
 */

#include "config_3D.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include "MFRC522.h"
#include <ArduinoLog.h>  // Advanced logging library

//------------------------------------------------------------------------------
// Pin Definitions
//------------------------------------------------------------------------------
#define RST_PIN 5  // Reset pin for the RFID module
#define SS_PIN 21  // SDA pin for the RFID module

#define LED_RED 32     // Pin for red LED
#define LED_YELLOW 33  // Pin for yellow LED
#define LED_GREEN 25   // Pin for green LED

#define RFID_SWITCH_PIN 4     // Pin for RFID switch
#define OFF_SWITCH_PIN 27  // Pin for switch-off button

#define RELAIS_PIN 22  // Pin for relais output

//------------------------------------------------------------------------------
// WiFi Credentials (from config_3D.h)
//------------------------------------------------------------------------------
const char *ssid = WIFI_SSID;
const char *pass = WIFI_PASSWORD;

//------------------------------------------------------------------------------
// Global Variables and Instances
//------------------------------------------------------------------------------
// Instantiate the RFID module
MFRC522 mfrc522(SS_PIN, RST_PIN);

// Global variables for tracking state
String loggedInID = "0";     // Currently logged-in RFID card ID
String uid = "";             // UID read from an RFID card
int loginUpdateCounter = 0;  // Counter to trigger session extension

bool switchState = LOW;     // Current state of the main switch
bool edgeFlagSwitch = LOW;  // Flag for detecting a rising edge on the switch
bool loginState = LOW;      // Flag indicating if login was successful
bool switchOffState = LOW;  // State of the switch-off button

bool isHttpRequestInProgress = false;        // Flag to indicate an ongoing HTTP request
bool isCardAuthConst = RFIDCARD_AUTH_CONST;  // Constant for card authentication

//------------------------------------------------------------------------------
// Function Prototypes
//------------------------------------------------------------------------------
void initPins();
void connectToWiFi();
void initRFID();
void logout();
String readID();
void tryLoginID(String uid);
void updateLogin();

//------------------------------------------------------------------------------
// Setup Function
//------------------------------------------------------------------------------
void setup() {
  // Initialize hardware pins
  initPins();

  // Start serial communication and initialize logging
  Serial.begin(115200);
  Log.begin(LOG_LEVEL_VERBOSE, &Serial);  // Set desired log level (e.g., LOG_LEVEL_NOTICE for less verbosity)

  Log.notice(F("\n[Setup] Booting system...\n"));

  // Connect to WiFi
  connectToWiFi();

  // Initialize the RFID module
  initRFID();

  Log.notice(F("[Setup] System ready. Awaiting RFID card scans...\n"));
  Log.notice(F("======================================================\n"));
  Log.notice(F("Scan for Card and print UID:\n"));
}

//------------------------------------------------------------------------------
// Main Loop
//------------------------------------------------------------------------------
void loop() {
  // Log system status at verbose level
  Log.verbose("Free heap: %d bytes\n", ESP.getFreeHeap());
  Log.verbose("Switch state RFID switch: %d\n", digitalRead(RFID_SWITCH_PIN));
  Log.verbose("Switch state logout switch: %d\n", digitalRead(OFF_SWITCH_PIN));
  Log.verbose("Login State: %d\n", loginState);

  // Select behavior based on RFIDCARD_AUTH_CONST:
  if (RFIDCARD_AUTH_CONST) {
    handleAuthConstTrue();
  } else {
    handleAuthConstFalse();
  }
}


//------------------------------------------------------------------------------
// Function Definitions
//------------------------------------------------------------------------------

/**
 * @brief Initializes all required hardware pins.
 *
 * Configures switch pins as inputs and LED/signal pins as outputs.
 * Runs a startup sequence that briefly turns on all LEDs.
 */
void initPins() {
  Log.verbose("[initPins] Setting up Pins... ");
  // Configure switch pins as inputs
  pinMode(RFID_SWITCH_PIN, INPUT_PULLUP);
  pinMode(OFF_SWITCH_PIN, INPUT_PULLUP);

  // Configure LED and signal pins as outputs
  pinMode(LED_RED, OUTPUT);
  pinMode(LED_YELLOW, OUTPUT);
  pinMode(LED_GREEN, OUTPUT);
  pinMode(RELAIS_PIN, OUTPUT);

  // Set Relais pin to LOW initially
  digitalWrite(RELAIS_PIN, LOW);

  // Startup sequence: Turn all LEDs on for 1 second
  digitalWrite(LED_RED, HIGH);
  digitalWrite(LED_YELLOW, HIGH);
  digitalWrite(LED_GREEN, HIGH);
  delay(1000);

  // Boot state: Turn off red and green LEDs, keep yellow LED on
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_YELLOW, HIGH);
  digitalWrite(LED_GREEN, LOW);

  Log.verbose("done\n");
}

/**
 * @brief Connects the ESP32 to the WiFi network.
 *
 * Attempts to establish a connection up to 10 times.
 * If the connection fails, the ESP32 restarts.
 */
void connectToWiFi() {
  WiFi.begin(ssid, pass);
  int retries = 0;

  // Attempt connection up to 10 times
  while ((WiFi.status() != WL_CONNECTED) && (retries < 10)) {
    retries++;
    delay(500);
    Log.verbose("WiFi connection attempt %d...\n", retries);
  }

  // Verify connection status
  if (WiFi.status() == WL_CONNECTED) {
    Log.notice("[connectToWiFi] WiFi connected successfully.\n");
    digitalWrite(LED_YELLOW, LOW);  // Turn off yellow LED once connected
  } else {
    // If connection fails, log error and restart ESP32
    digitalWrite(LED_RED, HIGH);
    Log.error("[connectToWiFi] WiFi connection failed after %d attempts. Restarting ESP32...\n", retries);
    delay(2000);  // Allow time for error visibility
    ESP.restart();
  }
}

/**
 * @brief Initializes the RFID module.
 *
 * Starts SPI communication and performs a self-test on the RFID reader.
 * If the self-test fails, the ESP32 restarts.
 */
void initRFID() {
  Log.verbose("[initRFID] Setting up SPI ... ");
  SPI.begin();         // Start SPI communication
  Log.verbose("done.\n");
  
  Log.verbose("[initRFID] Setting up RFID Module ... ");
  mfrc522.PCD_Init();  // Initialize RFID module
  Log.verbose("done.\n");

  // Perform self-test; restart ESP32 if initialization fails
  if (!mfrc522.PCD_PerformSelfTest()) {
    Log.error("[initRFID] RFID self-test failed. Restarting ESP32...\n");
    delay(2000);
    ESP.restart();
  } else {
    Log.notice("[initRFID] RFID reader initialized successfully.\n");
  }
}

/**
 * @brief Logs out the current session.
 *
 * Resets the login state and turns off all LEDs and the signal pin.
 */
void logout() {
  Log.notice("[logout] Logging out current session.\n");
  loggedInID = "0";

  // Turn off LEDs and relais output
  digitalWrite(LED_GREEN, LOW);
  digitalWrite(LED_YELLOW, LOW);
  digitalWrite(LED_RED, LOW);
  digitalWrite(RELAIS_PIN, LOW);
}

/**
 * @brief Reads the RFID card UID.
 *
 * Attempts to read an RFID card up to 3 times.
 *
 * @return A String representing the UID in hexadecimal format, or "0" if no card is found.
 */
String readID() {
  Log.verbose("[initRFID] Setting up RFID Module ... ");
  mfrc522.PCD_Init();  // Reinitialize the RFID module
  Log.verbose("done.\n");

  Log.verbose("[initRFID] Reading UID:\n");

  // Attempt to read the card up to 3 times
  for (int i = 0; i < 3; i++) {
    if (mfrc522.PICC_IsNewCardPresent()) {
      if (mfrc522.PICC_ReadCardSerial()) {
        String uid = "";
        // Process each byte of the UID
        for (byte i = 0; i < mfrc522.uid.size; i++) {
          if (i != 0) {
            uid += ":";  // Use colon as a delimiter
          }
          int byteVal = mfrc522.uid.uidByte[i];
          uid += (byteVal < 16 ? "0" : "") + String(byteVal, 16);
        }
        if (uid.isEmpty()) {
          Log.error("[readID] UID is empty. Aborting login.\n");
          return "0";
        }
        return uid;
      }
    }
    delay(100);  // Delay between attempts
  }
  Log.warning("[readID] No RFID card detected after multiple attempts.\n");
  return "0";  // Return "0" if no card is detected
}

/**
 * @brief Attempts to log in using the provided RFID card UID.
 *
 * Sends an HTTP GET request to the server for authentication.
 * Updates LED status based on the server response.
 *
 * @param uid The RFID card UID.
 */
void tryLoginID(String uid) {
  // Indicate login attempt: turn on yellow LED and ensure red LED is off
  digitalWrite(LED_YELLOW, HIGH);
  digitalWrite(LED_RED, LOW);

  HTTPClient http;
  WiFiClient client;

  // DEBUG ++++++++++++++ start ++++++++++++++++++++++++++++++++++++++++++++

  Log.verbose("[tryLoginID] Circumvent Auth; force sucessful login.\n");
  loginState = HIGH;  //TODO: DEBUG
  Log.notice("[tryLoginID] Login successful. UID: %s\n", uid.c_str());
  Log.notice("[tryLoginID] Enabling Output.\n");
  digitalWrite(LED_YELLOW, LOW);   // Turn off yellow LED
  digitalWrite(LED_GREEN, HIGH);   // Indicate success with green LED
  digitalWrite(RELAIS_PIN, HIGH);  // Activate relais pin
  return;

  // DEBUG ++++++++++++++ end ++++++++++++++++++++++++++++++++++++++++++++++

  // Avoid duplicate HTTP requests
  if (isHttpRequestInProgress) {
    Log.warning("[tryLoginID] HTTP request already in progress. Skipping login attempt.\n");
    return;
  }

  Log.info("[tryLoginID] Initiating login request...\n");

  // Construct URL for login request
  String url = "http://" + String(SERVER_IP) + "/machine_try_login/" + AUTHENTICATION_TOKEN + "/" + MACHINE_NAME + "/" + MACHINE_ID + "/" + uid;
  http.setTimeout(5000);
  if (!http.begin(client, url)) {
    Log.error("[tryLoginID] Failed to initialize HTTP client for URL: %s\n", url.c_str());
    return;
  }

  Log.info("[tryLoginID] Sending HTTP GET request for login...\n");
  int httpCode = http.GET();

  if (httpCode > 0) {  // HTTP request successful
    Log.verbose("[tryLoginID] HTTP GET returned code: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {  // 200 OK
      String payload = http.getString();
      Log.verbose("[tryLoginID] Server response: %s\n", payload.c_str());

      if (payload.indexOf("true") >= 0) {
        loggedInID = uid;
        Log.notice("[tryLoginID] Login successful. UID: %s\n", uid.c_str());
        digitalWrite(LED_YELLOW, LOW);   // Turn off yellow LED
        digitalWrite(LED_GREEN, HIGH);   // Indicate success with green LED
        digitalWrite(RELAIS_PIN, HIGH);  // Activate relais pin
        loginState = HIGH;
      } else {
        Log.error("[tryLoginID] Login failed. Server response did not confirm login.\n");
        digitalWrite(LED_YELLOW, LOW);
        digitalWrite(LED_RED, HIGH);
        digitalWrite(RELAIS_PIN, LOW);
        loginState = LOW;
        delay(2000);
        digitalWrite(LED_RED, LOW);
      }
    }
  } else {
    // HTTP GET failed; log the error details
    Log.error("[tryLoginID] HTTP GET failed: %s\n", http.errorToString(httpCode).c_str());
    digitalWrite(LED_RED, HIGH);
  }

  http.end();  // End HTTP connection
  isHttpRequestInProgress = false;
  Log.verbose("[tryLoginID] HTTP connection closed.\n");
}

/**
 * @brief Updates the login session by sending a session extension request.
 *
 * Increments a counter and sends an HTTP GET request to extend the session when the threshold is reached.
 */
void updateLogin() {
  // Reset LED statuses
  digitalWrite(LED_RED, LOW);
  digitalWrite(LED_YELLOW, LOW);

  // Increment counter and check threshold
  loginUpdateCounter++;
  if (loginUpdateCounter < 200) {
    return;
  }
  loginUpdateCounter = 0;  // Reset counter

  HTTPClient http;
  WiFiClient client;

  Log.info("[updateLogin] Sending session extension request...\n");

  if (isHttpRequestInProgress) {
    Log.warning("[updateLogin] HTTP request already in progress. Skipping session update.\n");
    return;
  }

  // Construct URL for session extension request
  String url = "http://" + String(SERVER_IP) + "/machine_extend_login/" + AUTHENTICATION_TOKEN + "/" + MACHINE_NAME;
  http.setTimeout(5000);
  if (!http.begin(client, url)) {
    Log.error("[updateLogin] Failed to initialize HTTP client for URL: %s\n", url.c_str());
    return;
  }

  Log.info("[updateLogin] Sending HTTP GET request for session extension...\n");
  int httpCode = http.GET();

  if (httpCode > 0) {  // HTTP request succeeded
    Log.verbose("[updateLogin] HTTP GET returned code: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {  // 200 OK
      String payload = http.getString();
      Log.verbose("[updateLogin] Server response: %s\n", payload.c_str());
    }
  } else {
    // HTTP GET failed; log error details and indicate failure with LEDs
    Log.error("[updateLogin] HTTP GET failed: %s\n", http.errorToString(httpCode).c_str());
    digitalWrite(LED_RED, HIGH);
    digitalWrite(LED_YELLOW, HIGH);
  }

  http.end();  // End HTTP connection
  isHttpRequestInProgress = false;
  Log.verbose("[updateLogin] HTTP connection closed.\n");
}

/**
 * @brief Handles login and logout when RFIDCARD_AUTH_CONST is true.
 *
 * In this mode, the main switch (active low) triggers:
 *   - Login when the button is pressed.
 *   - Logout automatically when the button is released.
 */
void handleAuthConstTrue() {
  Log.verbose("[handleAuthConstTrue] Waiting for main switch press (active low)...\n");
  // Wait until the main switch is pressed (active low)
  while (digitalRead(RFID_SWITCH_PIN) != LOW) {
    delay(50);
  }

  delay(100);  // Debounce delay

  // Button pressed: read the RFID card UID and attempt login
  uid = readID();
  Log.notice("[handleAuthConstTrue] Button pressed. Card UID: %s\n", uid.c_str());
  tryLoginID(uid);

  // While the button remains pressed, do nothing
  while (digitalRead(RFID_SWITCH_PIN) == LOW) {
    delay(50);
  }

  delay(100);  // Debounce delay after release

  // Button released: log out automatically
  Log.notice("[handleAuthConstTrue] Button released. Logging out...\n");
  logout();
}


/**
 * @brief Handles login and logout when RFIDCARD_AUTH_CONST is false.
 *
 * In this mode, the system logs in when the main switch (active low) is pressed,
 * and remains logged in regardless of further changes on the main switch.
 * Logout occurs only when the separate switch-off button (active low) is pressed.
 */
void handleAuthConstFalse() {
  if (!loginState) {
    Log.verbose("[handleAuthConstFalse] Waiting for main switch press to log in (active low)...\n");
    while (digitalRead(RFID_SWITCH_PIN) != LOW) { delay(50); }
    delay(100);  // Debounce delay
    uid = readID();
    Log.notice("[handleAuthConstFalse] Card UID: %s\n", uid.c_str());
    tryLoginID(uid);
    while (digitalRead(RFID_SWITCH_PIN) == LOW) { delay(50); }
  }
  if (loginState) {
    Log.verbose("[handleAuthConstFalse] Logged in. Waiting for switch-off button (active low) to log out...\n");
    while (digitalRead(OFF_SWITCH_PIN) != LOW) { delay(50); }
    delay(100);  // Debounce delay
    logout();
    while (digitalRead(OFF_SWITCH_PIN) == LOW) { delay(50); }
  }
}
