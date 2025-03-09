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
  STANDBY,        ///< System is idle, waiting for RFID input.
  IDENTIFICATION, ///< System is verifying RFID credentials.
  RUNNING,        ///< System is active, machine is running.
  RESET          ///< System is resetting to standby state.
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

//------------------------------------------------------------------------------
// Interrupt Service routines
//------------------------------------------------------------------------------

volatile bool buttonStopPressed = false; ///< Flag for stop button press

portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED; ///< Mutex for interrupt safety

/**
 * @brief Interrupt handler for the stop button press.
 *
 * Sets the flag when the button is pressed.
 */
void IRAM_ATTR handleButtonStopFalling() {
  portENTER_CRITICAL_ISR(&mux);
  buttonStopPressed = true;
  portEXIT_CRITICAL_ISR(&mux);
}

const int TIME_DEBOUNCE = 100;  ///< 100 ms button debounce time

//------------------------------------------------------------------------------
// WiFi Credentials (from config_3D.h)
//------------------------------------------------------------------------------
const char *ssid = WIFI_SSID;
const char *pass = WIFI_PASSWORD;

//------------------------------------------------------------------------------
// Global Variables and Instances
//------------------------------------------------------------------------------
MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN); ///< Instance of the RFID module

volatile bool machineRunning = false; ///< Tracks machine running state

String loggedInID = "0";     ///< Currently logged-in RFID card ID
String uid = "";             ///< UID read from an RFID card
int loginUpdateCounter = 0;  ///< Counter to trigger session extension

bool loginState = LOW; ///< Flag indicating if login was successful

bool isHttpRequestInProgress = false; ///< Flag to indicate an ongoing HTTP request
bool isCardAuthConst = RFIDCARD_AUTH_CONST; ///< Constant for card authentication

//------------------------------------------------------------------------------
// Setup Function
//------------------------------------------------------------------------------

/**
 * @brief Initializes hardware, connects to WiFi, and sets up the RFID module.
 */

void setup() {
  // For serial debug
  Serial.begin(115200);

  delay(100);
  Serial.println("Starting setup ...");

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
  setLED_ryg(1, 1, 1);                   // all reds on


  delay(1000);  // wait for pullups to get active

  attachInterrupt(digitalPinToInterrupt(BUTTON_STOP), handleButtonStopFalling, FALLING);

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

  Serial.println("Setup complete.");
}


/**
 * @brief Handles state transitions based on system inputs.
 */
void next_State() {
  switch (currentState) {
    case STANDBY:
      if (digitalRead(BUTTON_RFID) == LOW) {
        // when the RFID card is entered, proceed with identification
        nextState = IDENTIFICATION;
      }
      break;
    case IDENTIFICATION:
      if (auth_check) {
        // when auth check was successful, start machine
        nextState = RUNNING;
      } else {
        // when auth check was not successful, return to reset state
        nextState = RESET;
      }
      break;
    case RUNNING:
      // Differentiate if the machine needs the RFID card connected constantly:
      if (RFIDCARD_AUTH_CONST == true) {
        // The card has to be connected constantly:
        if (digitalRead(BUTTON_STOP) == LOW || digitalRead(BUTTON_RFID) == HIGH) {
          // when Stop Button was pressed or RFID card was removed, change to reset state
          nextState = RESET;
        }
        break;
      } else if (RFIDCARD_AUTH_CONST == false) {
        // Only a single sign-on is necessary:
        if (digitalRead(BUTTON_STOP) == LOW) {
          // when Stop Button was pressed, change to reset state
          nextState = RESET;
        }
        break;
      }
    case RESET:
      if ((digitalRead(BUTTON_RFID) == HIGH) && (digitalRead(BUTTON_STOP) == HIGH)) {
        // when both buttons are inactive, change to standby state
        nextState = STANDBY;
      }
      break;
  }

  if (currentState != nextState) {
    // slow down transistion and debounce switches
    delay(TIME_DEBOUNCE);
  }

  currentState = nextState;
}

void loop() {

  // overwrite state with stop button
  if (buttonStopPressed == true) {
    // reset flag
    buttonStopPressed = 0;
    nextState = RESET;
  }

  switch (currentState) {
    case STANDBY:
      Serial.println("State: STANDBY");
      setLED_ryg(0, 1, 0);
      break;

    case IDENTIFICATION:
      Serial.println("State: IDENTIFICATION");
      setLED_ryg(0, 1, 1);
      // Simulate authentication process
      auth_check = (random(0, 2) == 1);  // Randomly set auth_check for testing
      if (auth_check == 0) {
        Serial.println("Identification failed");
      } else if (auth_check == 1) {
        Serial.println("Identification passed");
      }

      break;

    case RUNNING:
      Serial.println("State: RUNNING");
      setLED_ryg(0, 0, 1);
      break;

    case RESET:
      Serial.println("State: RESET");
      setLED_ryg(1, 0, 0);
      break;
  }

  next_State();
  delay(100);  // Small delay for stability
}

void setLED_ryg(bool led_red, bool led_yellow, bool led_green) {
  Serial.printf("Setting LEDs - Red: %d, Yellow: %d, Green: %d\n", led_red, led_yellow, led_green);
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
  int retryDelay = 500;  // Start with 500ms delay

  while ((WiFi.status() != WL_CONNECTED) && (retries < 10)) {
    Serial.printf("WiFi connection attempt %d...\n", retries + 1);
    delay(retryDelay);
    retries++;
    retryDelay *= 2;  // Exponential backoff
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[connectToWiFi] WiFi connected successfully.");
    digitalWrite(LED_YELLOW_PIN, LOW);
  } else {
    Serial.println("[connectToWiFi] WiFi connection failed after 10 attempts. Retrying in 30 sec...");
    delay(30000);     // Wait before trying again instead of restarting
    connectToWiFi();  // Retry connection
  }
}

/**
 * @brief Initializes the RFID module and performs a self-test.
 */
void initRFID() {
  Serial.println("[initRFID] Setting up SPI ... ");
  SPI.begin();  // Start SPI communication
  Serial.println("done.\n");

  Serial.println("[initRFID] Setting up RFID Module ... ");
  mfrc522.PCD_Init();  // Initialize RFID module
  Serial.println("done.\n");

  // Perform self-test; restart ESP32 if initialization fails
  if (!mfrc522.PCD_PerformSelfTest()) {
    Serial.println("[initRFID] RFID self-test failed. Restarting ESP32...\n");
    delay(2000);
    ESP.restart();  // TODO might be problem
  } else {
    Serial.println("[initRFID] RFID reader initialized successfully.\n");
  }
}


/**
 * @brief Updates the login session by sending a session extension request.
 *
 * Increments a counter and sends an HTTP GET request to extend the session when the threshold is reached.
 */
void updateLogin() {
  // Reset LED statuses
  //digitalWrite(LED_RED, LOW);
  //digitalWrite(LED_YELLOW, LOW);

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
    //digitalWrite(LED_RED, HIGH);
    //digitalWrite(LED_YELLOW, HIGH);
  }

  http.end();  // End HTTP connection
  isHttpRequestInProgress = false;
  Log.verbose("[updateLogin] HTTP connection closed.\n");
}

/**
 * @brief Handles login
 *
 * In this mode, the main switch (active low) triggers:
 *   - Login when the button is pressed.
 *   - Logout automatically when the button is released.
 */
bool perform_auth_check() {
  // Read the RFID card UID and attempt login
  uid = readID();
  Log.notice("[auth_check] Card UID: %s\n", uid.c_str());
  // try authentification with the server
  return tryLoginID(uid);
}

/**
 * @brief Attempts to log in using the provided RFID card UID.
 *
 * Sends an HTTP GET request to the server for authentication.
 * Updates LED status based on the server response.
 *
 * @param uid The RFID card UID.
 */
int tryLoginID(String uid) {

  // return value: 0 = failed, 1=success, -1=busy
  bool authentication_success = 0;

  // Indicate login attempt: turn on yellow LED and ensure red LED is off
  // digitalWrite(LED_YELLOW, HIGH);
  // digitalWrite(LED_RED, LOW);

  HTTPClient http;
  WiFiClient client;

  // DEBUG ++++++++++++++ start ++++++++++++++++++++++++++++++++++++++++++++
  /*
  Log.verbose("[tryLoginID] Circumvent Auth; force sucessful login.\n");
  loginState = HIGH;  //TODO: DEBUG
  Log.notice("[tryLoginID] Login successful. UID: %s\n", uid.c_str());
  Log.notice("[tryLoginID] Enabling Output.\n");
  digitalWrite(LED_YELLOW, LOW);   // Turn off yellow LED
  digitalWrite(LED_GREEN, HIGH);   // Indicate success with green LED
  digitalWrite(RELAIS_PIN, HIGH);  // Activate relais pin
  return;*/

  // DEBUG ++++++++++++++ end ++++++++++++++++++++++++++++++++++++++++++++++

  // Avoid duplicate HTTP requests
  if (isHttpRequestInProgress) {
    Log.warning("[tryLoginID] HTTP request already in progress. Skipping login attempt.\n");
    return -1;
  }

  Log.info("[tryLoginID] Initiating login request...\n");

  // Construct URL for login request
  String url = "http://" + String(SERVER_IP) + "/machine_try_login/" + AUTHENTICATION_TOKEN + "/" + MACHINE_NAME + "/" + MACHINE_ID + "/" + uid;
  http.setTimeout(5000);
  if (!http.begin(client, url)) {
    Log.error("[tryLoginID] Failed to initialize HTTP client for URL: %s\n", url.c_str());
    return 0;
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
        authentication_success = 1;
      } else {
        Log.error("[tryLoginID] Login failed. Server response did not confirm login.\n");
        authentication_success = 0;
      }
    }
  } else {
    // HTTP GET failed; log the error details
    Log.error("[tryLoginID] HTTP GET failed: %s\n", http.errorToString(httpCode).c_str());
    authentication_success = 0;
  }

  http.end();  // End HTTP connection
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
  Log.verbose("[readID] Attempt to Read RFID Card ... ");
  for (int attempt = 0; attempt < 3; attempt++) {
    if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
      String uid = "";
      for (byte i = 0; i < mfrc522.uid.size; i++) {
        uid += (mfrc522.uid.uidByte[i] < 16 ? "0" : "") + String(mfrc522.uid.uidByte[i], 16);
        if (i < mfrc522.uid.size - 1) uid += ":";
      }
      mfrc522.PICC_HaltA();  // Halt the RFID card
      mfrc522.PCD_StopCrypto1(); // Stop encryption
      return uid;
    }
    delay(100);
  }
  Log.error("[readID] No RFID card detected after multiple attempts.\n");
  return "0";
}