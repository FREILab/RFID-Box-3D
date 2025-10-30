/**
 * @file RFID_3D_ESP32.cpp
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

#define AUTH_OVERRIDE true

#include "config_3D.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <SPI.h>
#include "MFRC522.h"
#include <ArduinoLog.h>

//------------------------------------------------------------------------------
// State Definitions
//------------------------------------------------------------------------------

/**
 * @enum State
 * @brief Defines the different states of the RFID login system.
 */
enum State {
  STANDBY,         ///< System is idle, waiting for RFID input.
  IDENTIFICATION,  ///< System is verifying RFID credentials.
  RUNNING,         ///< System is active, machine is running.
  RESET            ///< System is resetting to standby state.
};

State currentState = STANDBY;
State nextState = STANDBY;
bool auth_check = true;
unsigned long stateChangeTime = 0;

//------------------------------------------------------------------------------
// Pin Definitions
//------------------------------------------------------------------------------

#define MACHINE_RELAY_PIN 22  ///< Pin controlling the machine relay
#define BUTTON_RFID 4         ///< Button to stop machine / Taster 1
#define BUTTON_STOP 13        ///< Button to start machine / Taster 2

#define RFID_RST_PIN 5  ///< Reset pin for the RFID module
#define RFID_SS_PIN 21  ///< SDA pin for the RFID module

#define LED_RED_PIN 32     ///< Pin for red LED
#define LED_YELLOW_PIN 33  ///< Pin for yellow LED
#define LED_GREEN_PIN 26   ///< Pin for green LED

#define BUTTON_PRESSED 0   ///< Buttons active low

//------------------------------------------------------------------------------
// Interrupt Service routines
//------------------------------------------------------------------------------

volatile bool buttonStopPressed = false;  ///< Flag for stop button press

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;  ///< Mutex for interrupt safety

const int TIME_GLITCH_FILTER_STOP = 100;  ///< 0.5s button debounce time
const int TIME_GLITCH_FILTER_RFID = 3000; ///< 3s button debounce time

//------------------------------------------------------------------------------
// WiFi Credentials (from config_3D.h)
//------------------------------------------------------------------------------
const char *ssid = WIFI_SSID;
const char *pass = WIFI_PASSWORD;

//------------------------------------------------------------------------------
// Global Variables and Instances
//------------------------------------------------------------------------------
MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);  ///< Instance of the RFID module

volatile bool machineRunning = false;  ///< Tracks machine running state

String loggedInID = "0";     ///< Currently logged-in RFID card ID
String uid = "";             ///< UID read from an RFID card
int loginUpdateCounter = 0;  ///< Counter to trigger session extension

bool loginState = LOW;  ///< Flag indicating if login was successful

bool isHttpRequestInProgress = false;        ///< Flag to indicate an ongoing HTTP request
bool isCardAuthConst = RFIDCARD_AUTH_CONST;  ///< Constant for card authentication

//------------------------------------------------------------------------------
// Setup Function
//------------------------------------------------------------------------------

/**
 * @brief Initializes hardware, connects to WiFi, and sets up the RFID module.
 */
void setup() {

  Log.begin(LOG_LEVEL_VERBOSE, &Serial);  // Initialize logging

  // For serial debug
  Serial.begin(115200);

  delay(100);

  Log.notice("Starting setup ...\n");

  // Relay control output
  pinMode(MACHINE_RELAY_PIN, OUTPUT);

  // LEDs pins
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_YELLOW_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);

  // RFID card detection button
  pinMode(BUTTON_RFID, INPUT_PULLUP);
  // Logout button
  pinMode(BUTTON_STOP, INPUT_PULLUP);

  // set initial states
  digitalWrite(MACHINE_RELAY_PIN, LOW);  // Ensure machine is off initially
  setLED_ryg(1, 1, 1);                   // all LEDs on

  delay(1000);  // wait for pullups to get active

  // Connect to WiFi
  connectToWiFi();

  // Initialize the RFID module
  initRFID();

  // indicate successful startup
  delay(500);
  setLED_ryg(0, 0, 0);  // all off
  delay(100);
  setLED_ryg(0, 0, 1);  // green
  delay(100);
  setLED_ryg(0, 0, 0);  // all off
  delay(100);
  setLED_ryg(0, 0, 1);  // green
  delay(100);
  setLED_ryg(0, 0, 0);  // all off

  buttonStopPressed = false;

  Log.notice("Setup complete.\n");
}


/**
 * @brief Handles state transitions based on system inputs.
 */
void next_State() {

  //Glitch filter variables for RFID button
  static unsigned long rfidButtonPressTime = 0;
  static bool rfidButtonTimerActive = false;
  static unsigned long stopButtonPressTime = 0;
  static bool stopButtonTimerActive = false;

  switch (currentState) {
    case STANDBY:
      if (digitalRead(BUTTON_RFID) == BUTTON_PRESSED) {
        // when the RFID card is entered, proceed with identification
        nextState = IDENTIFICATION;
      }
      break;

    case IDENTIFICATION:
      if (perform_auth_check()) {
        // when auth check was successful, start machine
        nextState = RUNNING;
        delay(500); // Allow some buffer time before transitioning to RUNNING
      } else {
        // when auth check was not successful, return to reset state
        Log.verbose("[next_State] Identification not successful.\n");
        nextState = RESET;
      }
      break;

    case RUNNING:
      // Differentiate if the machine needs the RFID card connected constantly:
      if (RFIDCARD_AUTH_CONST == true) {
        // The card has to be connected constantly:
        if (digitalRead(BUTTON_STOP) == BUTTON_PRESSED) {
          // Start the timer if not already active
          if (!stopButtonTimerActive) {
            stopButtonPressTime = millis();
            stopButtonTimerActive = true;
          }
          // If BUTTON_STOP has been LOW for at least TIME_GLITCH_FILTER_STOP ms, enter RESET state
          if (millis() - stopButtonPressTime >= TIME_GLITCH_FILTER_STOP) {
            Log.verbose("[next_State] Stop button pressed.\n");
            nextState = RESET;
          }
        } else {
          // Reset the timer if BUTTON_STOP goes HIGH
          stopButtonTimerActive = false;
        }
        
        if (digitalRead(BUTTON_RFID) != BUTTON_PRESSED) {
          // Start the timer if not already active
          if (!rfidButtonTimerActive) {
            rfidButtonPressTime = millis();
            rfidButtonTimerActive = true;
          }
          // If BUTTON_RFID has been pulled for at least 3 seconds, enter RESET state
          if (millis() - rfidButtonPressTime >= TIME_GLITCH_FILTER_RFID) {
            Log.verbose("[next_State] RFID Card pulled.\n");
            nextState = RESET;
          }
        } else {
          // Reset the timer if BUTTON_RFID goes LOW
          rfidButtonTimerActive = false;
        }
        break;
      } else if (RFIDCARD_AUTH_CONST == false) {
        // Only a single sign-on is necessary:
        if (digitalRead(BUTTON_STOP) == BUTTON_PRESSED) {
          // Start the timer if not already active
          if (!stopButtonTimerActive) {
            stopButtonPressTime = millis();
            stopButtonTimerActive = true;
          }
          // If BUTTON_STOP has been LOW for at least TIME_GLITCH_FILTER_STOP ms, enter RESET state
          if (millis() - stopButtonPressTime >= TIME_GLITCH_FILTER_STOP) {
            Log.verbose("[next_State] Stop button pressed.\n");
            nextState = RESET;
          }
        } else {
          // Reset the timer if BUTTON_STOP goes HIGH
          stopButtonTimerActive = false;
        }
        break;
      }

    case RESET:
      if ((digitalRead(BUTTON_RFID) != BUTTON_PRESSED) && (digitalRead(BUTTON_STOP) != BUTTON_PRESSED)) {
        // when both buttons are inactive, change to standby state
        nextState = STANDBY;
      }
      break;
  }

  if (currentState != nextState) {
    // Slow down transition and debounce switches
    delay(TIME_GLITCH_FILTER_STOP);

    switch (nextState) {
      case STANDBY:
        Log.verbose("[next_State] Next State: STANDBY\n");
        break;
      case IDENTIFICATION:
        Log.verbose("[next_State] Next State: IDENTIFICATION\n");
        break;
      case RUNNING:
        Log.verbose("[next_State] Next State: RUNNING\n");
        break;
      case RESET:
        Log.verbose("[next_State] Next State: RESET\n");
        break;
    }
  }

  currentState = nextState;
}

/**
 * @brief Main Loop
 */
void loop() {

  // Check WiFi
  checkWiFiConnection();

  switch (currentState) {
    case STANDBY:
      digitalWrite(MACHINE_RELAY_PIN, LOW);
      setLED_ryg(0, 1, 0);
      break;

    case IDENTIFICATION:
      digitalWrite(MACHINE_RELAY_PIN, LOW);
      setLED_ryg(0, 1, 1);
      break;

    case RUNNING:
      digitalWrite(MACHINE_RELAY_PIN, HIGH);
      setLED_ryg(0, 0, 1);
      break;

    case RESET:
      digitalWrite(MACHINE_RELAY_PIN, LOW);
      setLED_ryg(1, 0, 0);
      break;
  }

  next_State();
  delay(100);  // Small delay for stability
}

/**
 * @brief Set external LEDs
 */
void setLED_ryg(bool led_red, bool led_yellow, bool led_green) {
  digitalWrite(LED_RED_PIN, led_red);
  digitalWrite(LED_YELLOW_PIN, led_yellow);
  digitalWrite(LED_GREEN_PIN, led_green);
}

/**
 * @brief Connects the ESP32 to the WiFi network.
 */
void connectToWiFi() {
  WiFi.begin(ssid, pass);
  int retries = 0;

  while (WiFi.status() != WL_CONNECTED && retries < 10) {
    Log.notice("[connectToWiFi] Attempt %d...\n", retries + 1);
    delay(1000);
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Log.notice("[connectToWiFi] Connected to WiFi.\n");
  } else {
    Log.warning("[connectToWiFi] WiFi connection failed after 10 attempts!\n");
  }
}

/**
 * @brief Checks WiFi network connection.
 */
void checkWiFiConnection() {
  if (WiFi.status() != WL_CONNECTED) {
    Log.warning("[checkWiFiConnection] WiFi disconnected! Reconnecting...\n");
    connectToWiFi();
  }
}

/**
 * @brief Initializes the RFID module and verifies communication.
 */
void initRFID() {
  Log.notice("[initRFID] Setting up SPI...\n");
  SPI.begin();  // Start SPI communication

  Log.notice("[initRFID] Setting up RFID Module...\n");
  mfrc522.PCD_Init();  // Initialize RFID module
  
  delay(50);  // Short delay after initialization
  
  // Verify RFID module communication by reading firmware version
  byte version = mfrc522.PCD_ReadRegister(mfrc522.VersionReg);
  
  if (version == 0x00 || version == 0xFF) {
    Log.error("[initRFID] RFID module not responding! Check wiring. Version: 0x%02X\n", version);
    Log.error("[initRFID] Expected version: 0x91 or 0x92. Restarting ESP32...\n");
    delay(2000);
    ESP.restart();
  } else {
    Log.notice("[initRFID] RFID reader initialized successfully.\n");
    Log.notice("[initRFID] Firmware version: 0x%02X\n", version);
  }
}


/**
 * @brief Handles login authentication
 *
 * Reads RFID card and attempts server authentication.
 */
bool perform_auth_check() {
  // Read the RFID card UID and attempt login
  uid = readID();
  if (uid.equals("0")) {
    Log.error("[perform_auth_check] Reading UID failed, aborting login\n");
    return false;
  }
  Log.notice("[perform_auth_check] Card UID: %s\n", uid.c_str());
  // try authentication with the server
  return tryLoginID(uid);
}

/**
 * @brief Attempts to log in using the provided RFID card UID.
 *
 * Sends an HTTP GET request to the server for authentication.
 * Updates LED status based on the server response.
 *
 * @param uid The RFID card UID.
 * @return 1 if successful, 0 if failed, -1 if busy
 */
int tryLoginID(String uid) {

  // return value: 0 = failed, 1=success, -1=busy
  int authentication_success = 0;

  static HTTPClient http;
  static WiFiClient client;

  // Avoid duplicate HTTP requests
  if (isHttpRequestInProgress || WiFi.status() != WL_CONNECTED) {
    Log.warning("[tryLoginID] HTTP request already in progress or WiFi not connected. Skipping login attempt.\n");
    return -1;
  }
  isHttpRequestInProgress = true;

  Log.info("[tryLoginID] Initiating login request...\n");

  // Construct URL for login request
  String url = "http://" + String(SERVER_IP) + "/machine_try_login/" + AUTHENTICATION_TOKEN + "/" + MACHINE_NAME + "/" + MACHINE_ID + "/" + uid;
  http.setReuse(true);
  http.setTimeout(3000);

  if (!http.begin(client, url)) {
    Log.error("[tryLoginID] Failed to initialize HTTP client for URL: %s\n", url.c_str());
    isHttpRequestInProgress = false;
    http.end();
    return 0;
  }

  Log.info("[tryLoginID] Sending HTTP GET request for login...\n");
  int httpCode = http.GET();

  if (httpCode > 0) {
    Log.verbose("[tryLoginID] HTTP GET returned code: %d\n", httpCode);

    if (httpCode == HTTP_CODE_OK) {
      String payload = http.getString();
      Log.verbose("[tryLoginID] Server response: %s\n", payload.c_str());

      if (payload.indexOf("true") >= 0) {
        loggedInID = uid;
        Log.notice("[tryLoginID] Login successful. UID: %s\n", uid.c_str());
        authentication_success = 1;
      } else {
        Log.error("[tryLoginID] Login failed. Server response did not confirm login.\n");
        authentication_success = 0;
      }
    } else {
      Log.error("[tryLoginID] HTTP error code: %d\n", httpCode);
      authentication_success = 0;
    }
  } else {
    Log.error("[tryLoginID] HTTP GET failed: %s\n", http.errorToString(httpCode).c_str());
    authentication_success = 0;
  }

  http.end();
  isHttpRequestInProgress = false;
  Log.verbose("[tryLoginID] HTTP connection closed.\n");

  return authentication_success;
}

/**
 * @brief Reads the RFID card UID.
 *
 * Attempts to read an RFID card up to 3 times.
 *
 * @return A String representing the UID in hexadecimal format, or "0" if no card is found.
 */
String readID() {
  Log.verbose("[readID] Attempting to read RFID card...\n");

  for (int attempt = 0; attempt < 3; attempt++) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      String uid = "";
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        uid += (mfrc522.uid.uidByte[i] < 16 ? "0" : "") + String(mfrc522.uid.uidByte[i], 16);
        if (i < mfrc522.uid.size - 1) uid += ":";
      }
      mfrc522.PICC_HaltA();       // Halt the RFID card
      mfrc522.PCD_StopCrypto1();  // Stop encryption
      
      Log.verbose("[readID] Card read successfully: %s\n", uid.c_str());
      return uid;
    }
    delay(500);
  }
  
  Log.error("[readID] No RFID card detected after 3 attempts.\n");
  return "0";
}