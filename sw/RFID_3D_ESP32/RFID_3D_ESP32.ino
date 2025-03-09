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
const unsigned long STATE_DELAY = 500;  // 500ms delay before transition

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

  // Relais control output
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
        // when auth check was sucessful, start machine
        nextState = RUNNING;
      } else {
        // when auth check was not sucessful, return to reset state
        nextState = RESET;
      }
      break;
    case RUNNING:
      // Differentiate if the machine need the RFID card connected constantly:
      if (RFIDCARD_AUTH_CONST == true) {
        // The card has to be connected constantly:
        if (digitalRead(BUTTON_STOP) == LOW || digitalRead(BUTTON_RFID) == HIGH) {
          // when Stop Button was pressed or RFID card was removed, change to reset state
          nextState = RESET;
        }
        break;
      } else if (RFIDCARD_AUTH_CONST == false) {
        // Only a singhle sign-on is necessary:
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
  currentState = nextState;
}

void loop() {

  // overwirite state with stop button
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
  digitalWrite(LED_RED_PIN, led_red);        // LED Test
  digitalWrite(LED_YELLOW_PIN, led_yellow);  // LED Test
  digitalWrite(LED_GREEN_PIN, led_green);    // LED Test
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
