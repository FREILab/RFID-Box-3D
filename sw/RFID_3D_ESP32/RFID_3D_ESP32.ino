

#define AUTH_OVERRIDE true

// Note: for flashing software the stop button has to be pressed

//------------------------------------------------------------------------------
// Pin Definitions
//------------------------------------------------------------------------------

#define MACHINE_RELAY_PIN 22  // Pin controlling the machine relay
#define CARD_DETECT_PIN 4     // Button to stop machine / Taster 1 /TODO/ correct
#define STOP_BUTTON_PIN 2     // Button to start machine / Taster 2 /TODO/ richtiger pin, led paralle, braucht stärkeren PU


// Taster 1 geht /rfid

#define LED_RED_PIN 32     // Pin for red LED
#define LED_YELLOW_PIN 33  // Pin for yellow LED
#define LED_GREEN_PIN 25   // Pin for green LED

// current button state; have to be reset after been read
volatile bool cardDetected_flag = false;
volatile bool stopPressed_flag = false;

volatile bool machineRunning = false;

//------------------------------------------------------------------------------
// Interrupt Service routines
//------------------------------------------------------------------------------

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
// Setup
//------------------------------------------------------------------------------

void setup() {
  // For serial debug
  Serial.begin(115200);

  delay(100);
  Serial.print("Starting setup ...");

  // Relais control output
  pinMode(MACHINE_RELAY_PIN, OUTPUT);

  // LEDs pins
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_YELLOW_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);


  // RFID card detection button
  //pinMode(CARD_DETECT_PIN, INPUT_PULLUP);
  pinMode(CARD_DETECT_PIN, INPUT);
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

  Serial.print("Setup complete.");
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
