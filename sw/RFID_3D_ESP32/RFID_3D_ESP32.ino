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

// Note: for flashing software the stop button has to be pressed

//------------------------------------------------------------------------------
// Pin Definitions
//------------------------------------------------------------------------------

#define MACHINE_RELAY_PIN 22  // Pin controlling the machine relay
#define CARD_DETECT_PIN 4     // Button to stop machine / Taster 1
#define STOP_BUTTON_PIN 2     // Button to start machine / Taster 2

#define RFID_RST_PIN 5  // Reset pin for the RFID module
#define RFID_SS_PIN 21  // SDA pin for the RFID module

#define LED_RED_PIN 32     // Pin for red LED
#define LED_YELLOW_PIN 33  // Pin for yellow LED
#define LED_GREEN_PIN 25   // Pin for green LED

//------------------------------------------------------------------------------
// Interrupt Service routines
//------------------------------------------------------------------------------

// current button state; have to be reset after been read
volatile bool cardDetected_flag = false;
volatile bool stopPressed_flag = false;

// ISR for RFID Button
void IRAM_ATTR cardDetectISR_flag() {
  cardDetected_flag = true;
}

// ISR for Stop Button
void IRAM_ATTR stopButtonISR_flag() {
  stopPressed_flag = true;
}

const int TIME_DEBOUNCE = 100;  // 100 ms button debounce

//------------------------------------------------------------------------------
// WiFi Credentials (from config_3D.h)
//------------------------------------------------------------------------------
const char *ssid = WIFI_SSID;
const char *pass = WIFI_PASSWORD;

//------------------------------------------------------------------------------
// Global Variables and Instances
//------------------------------------------------------------------------------
// Instantiate the RFID module
MFRC522 mfrc522(RFID_SS_PIN, RFID_RST_PIN);

// State of ouput switch
volatile bool machineRunning = false;

// Global variables for tracking state
String loggedInID = "0";     // Currently logged-in RFID card ID
String uid = "";             // UID read from an RFID card
int loginUpdateCounter = 0;  // Counter to trigger session extension

bool loginState = LOW;      // Flag indicating if login was successful

bool isHttpRequestInProgress = false;        // Flag to indicate an ongoing HTTP request
bool isCardAuthConst = RFIDCARD_AUTH_CONST;  // Constant for card authentication

//------------------------------------------------------------------------------
// Setup
//------------------------------------------------------------------------------

void setup() {
  // For serial debug
  Serial.begin(115200);

  delay(100);
  Serial.println("Starting setup ...");

  // Relais control output
  pinMode(MACHINE_RELAY_PIN, OUTPUT);

  // LEDs pins
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_YELLOW_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);


  // RFID card detection button
  pinMode(CARD_DETECT_PIN, INPUT_PULLUP);
  // Logout button
  pinMode(STOP_BUTTON_PIN, INPUT_PULLUP);

  // set initial states
  digitalWrite(MACHINE_RELAY_PIN, LOW);  // Ensure machine is off initially
  setLED_ryg(1, 1, 1);                   // all reds on

  delay(10);  // wait for pullups to get active

  // read button states
  cardDetected_flag = digitalRead(CARD_DETECT_PIN);
  stopPressed_flag = digitalRead(CARD_DETECT_PIN);

  if (cardDetected_flag == 0) {
    // button pressed, should not be the case. display error and halt
    setLED_ryg(1, 0, 0);  // show red
    // block startup
    while (1) {}
  }
  if (stopPressed_flag == 0) {
    // button pressed, should not be the case. display error and halt
    setLED_ryg(1, 0, 0);  // show red
    // block startup
    while (1) {}
  }

  // Button test sucessful, attach interrups
  attachInterrupt(digitalPinToInterrupt(CARD_DETECT_PIN), cardDetectISR_flag, FALLING);
  cardDetected_flag = false;
  attachInterrupt(digitalPinToInterrupt(STOP_BUTTON_PIN), stopButtonISR_flag, FALLING);
  stopPressed_flag = false;

  // Connect to WiFi
  connectToWiFi();

  // Initialize the RFID module
  initRFID();

  // indicate sucessfull startup
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



void loop() {
  // TODO check wifi every 5 seconds

  /* wifi code*/

  if (machineRunning == false) {

    // Check Stop Button while machine is not running

    if (stopPressed_flag == false) {
      // nothing to do
      Serial.println("[loop] Machnine not running, Stop button not pressed -> do nothing");
    } else if (stopPressed_flag == true) {
      // stop buttin was initially triggered and machine is not running
      Serial.println("[loop] Machnine not running, Stop button pressed -> abort loop");
      // debounce switch and reset flag
      delay(TIME_DEBOUNCE);
      stopPressed_flag = false;

      return;  // abort main loop to avoud RFID checking
    }

    // Check RFID button while machine is not running

    if (cardDetected_flag == false) {
      // nothing to do, abort loop
      Serial.println("[loop] Machnine not running, RFID button not pressed -> do nothing");
    } else if (cardDetected_flag == true) {
      // carddetect was initially triggered and machine is not running
      Serial.println("[loop] Machnine not running, RFID button pressed -> check authentification");

      // debounce switch and reset flag
      delay(TIME_DEBOUNCE);
      cardDetected_flag = false;

      // indicate that authentification is checked
      setLED_ryg(0, 1, 0);  // yellow

      // TODO contact server

      bool authentification = AUTH_OVERRIDE;

      if (authentification == false) {
        // authenfication failed, keep machine off
        Serial.println("[loop] Authentification not sucessful -> don't enable relais");

        // indicate failed attempt
        setLED_ryg(1, 0, 0);  // red

        delay(1000);
      } else if (authentification == true) {
        // authenfication pass, enable machine
        Serial.println("[loop] Authentification sucessful -> enable relais");

        // indicate sucessful attempt
        setLED_ryg(0, 0, 1);  // green

        // enable output
        digitalWrite(MACHINE_RELAY_PIN, HIGH);
        machineRunning = true;
      }
    }
  } else if (machineRunning == true) {

    // Check Stop Button while machine is running

    if (stopPressed_flag == false) {
      // nothing to do
      Serial.println("[loop] Machnine running, Stop button not pressed -> do nothing");
    } else if (stopPressed_flag == true) {
      // stop buttin was initially triggered and machine is running
      Serial.println("[loop] Machnine running, Stop button pressed -> stop machine and abort loop");

      // debounce switch and reset flag
      delay(TIME_DEBOUNCE);
      stopPressed_flag = false;

      // indicate that machine will be stopped
      setLED_ryg(1, 1, 0);  // red + yellow

      // disable output
      digitalWrite(MACHINE_RELAY_PIN, LOW);
      machineRunning = false;

      delay(1000);

      // reset LEDs
      setLED_ryg(0, 0, 0);  // all off

      return;  // abort main loop
    }

    // Check RFID button while machine is running

    if (cardDetected_flag == false) {
      // nothing to do, abort loop
      Serial.println("[loop] Machnine already running, RFID button not pressed -> do nothing");
    } else if (cardDetected_flag == true) {
      // nothing to do, abort loop
      Serial.println("[loop] Machnine already running, RFID button pressed -> do nothing");

      // debounce switch and reset flag
      delay(TIME_DEBOUNCE);
      cardDetected_flag = false;
    }
  }
  delay(200);  // slow down loop
}

void setLED_ryg(bool led_red, bool led_yellow, bool led_green) {
  digitalWrite(LED_RED_PIN, led_red);        // LED Test
  digitalWrite(LED_YELLOW_PIN, led_yellow);  // LED Test
  digitalWrite(LED_GREEN_PIN, led_green);    // LED Test
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
    Serial.println("WiFi connection attempt %d...\n, retries");
  }

  // Verify connection status
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[connectToWiFi] WiFi connected successfully.\n");
    digitalWrite(LED_YELLOW_PIN, LOW);  // Turn off yellow LED once connected
  } else {
    // If connection fails, log error and restart ESP32
    digitalWrite(LED_RED_PIN, HIGH);
    Serial.println("[connectToWiFi] WiFi connection failed after %d attempts. Restarting ESP32...\n, retries");
    delay(2000);  // Allow time for error visibility
    ESP.restart(); // TODO might be problem
  }
}

/**
 * @brief Initializes the RFID module.
 *
 * Starts SPI communication and performs a self-test on the RFID reader.
 * If the self-test fails, the ESP32 restarts.
 */
void initRFID() {
  Serial.println("[initRFID] Setting up SPI ... ");
  SPI.begin();         // Start SPI communication
  Serial.println("done.\n");
  
  Serial.println("[initRFID] Setting up RFID Module ... ");
  mfrc522.PCD_Init();  // Initialize RFID module
  Serial.println("done.\n");

  // Perform self-test; restart ESP32 if initialization fails
  if (!mfrc522.PCD_PerformSelfTest()) {
    Serial.println("[initRFID] RFID self-test failed. Restarting ESP32...\n");
    delay(2000);
    ESP.restart(); // TODO might be problem
  } else {
    Serial.println("[initRFID] RFID reader initialized successfully.\n");
  }
}
