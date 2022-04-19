/*
 * MorseCode HID
 * by Riley Mann
 * 
 * A sketch for a telegraph key that can be used as
 * a USB keyboard. It uses an Adafruit QT Py SAMD21
 * 
 * Wiring:
 * - key/dot input: A0/D0
 * - dash input:    A1/D1
 * - buzzer:        A2/D2
 * - mode switch:   A3/D3
 * - key led:       RX/A7/D7
 * - manual led:    SCK/A8/D8
 * - auto led:      MI/A9/D9
 * - indicator led: MO/A10/D10
 * 
 */

#include "Keyboard.h"

//pins
#define KEY_IN   0
#define DASH_IN  1
#define BUZZER   2
#define MODE_SW  3
#define KEY_LED  7
#define MNL_LED  8
#define AUTO_LED 9
#define IND_LED 10

#define DASH_MULT     3.0 //*dot_t
#define DASH_THRESH   2.0 //*dot_t
#define C_FIN_THRESH  3.0 //*dot_t
#define W_FIN_THRESH  9.0 //*dot_t

#define KEY_LED_PULSE 50 //ms

#define DEFAULT_DOT_T 60 //ms
#define MIN_DOT_T     40 //ms

//morse code index format: Tab|DotDashCode|Space|Char
String CODE_INDEX = "\t.- a\t-... b\t-.-. c\t-.. d\t. e\t..-. f\t--. g\t.... h\t.. i\t.--- j\t-.- k\t.-.. l\t-- m\t-. n\t--- o\t.--. p\t--.- q\t.-. r\t... s\t- t\t..- u\t...- v\t.-- w\t-..- x\t-.-- y\t--.. z\t----- 0\t.---- 1\t..--- 2\t...-- 3\t....- 4\t..... 5\t-.... 6\t--... 7\t---.. 8\t----. 9\t.-.-.- .\t--..-- ,\t..--.. ?\t.----. '\t-.-.-- !\t-..-. /\t-.--. (\t-.--.- )\t.-... &\t---... :\t-.-.-. ;\t-...- =\t.-.-. +\t-....- -\t..--.- _\t.-..-. \"\t...-..- $\t.--.-. @\t..--  \t....-. \x81\t.-.- \xb0\t...-.- \xb0\t---- \xb2\t........ \xb2";
//characters that omit the space after them
char OMIT_SPACE[] = {0x00, ' ', 0x81, 0x85, 0xB0, 0xB2};
// special characters
char SHIFT = 0x81;
char BACKSPACE = 0xB2;

//states
enum {MNL, AUTO};
enum {OFF, ON};
enum {UNDEF, DOT, DASH};

//iambic keying
enum {MODE_A, MODE_B};
#define IAMBIC_MODE MODE_A

/* Function Variables */

//timer struct
struct StateTimer {
  unsigned long t_start;
  uint8_t state;
};

//pulses struct
#define MAX_PULSES 64
struct Pulses {
  uint16_t list[MAX_PULSES];
  uint8_t len = 0;
};

//keyer struct
//stores all of the keyer data
struct Keyer {
  uint16_t dot_t;
  Pulses pulses;
  boolean char_finished;
  boolean word_finished;
  boolean shifted;
  boolean backspace_count;
  boolean dot_pressed;
  boolean dash_pressed;
  uint8_t queue;
  uint8_t soft_queue;
  StateTimer keyer_timer;
  StateTimer ind_timer;
  StateTimer key_led_timer;
};
Keyer keyer;

/* End of Function Variables */

#if defined(ARDUINO_SAMD_ZERO) && defined(SERIAL_PORT_USBVIRTUAL)
  // Required for Serial on Zero based boards
  #define Serial SERIAL_PORT_USBVIRTUAL
#endif

void setup() {
  //set KEY_IN, DASH_IN, MODE_SW as input with pullup
  pinMode(KEY_IN, INPUT_PULLUP);
  pinMode(DASH_IN, INPUT_PULLUP);
  pinMode(MODE_SW, INPUT_PULLUP);
  
  //set BUZZER, KEY_LED, MNL_LED, AUTO_LED, IND_LED as output and low
  pinMode(BUZZER, OUTPUT); digitalWrite(BUZZER, LOW);
  pinMode(KEY_LED, OUTPUT); digitalWrite(KEY_LED, LOW);
  pinMode(MNL_LED, OUTPUT); digitalWrite(MNL_LED, LOW);
  pinMode(AUTO_LED, OUTPUT); digitalWrite(AUTO_LED, LOW);
  pinMode(IND_LED, OUTPUT); digitalWrite(IND_LED, LOW);

  //initialize serial
  Serial.begin(9600);
  
  //initialize keyboard
  Keyboard.begin();

  //initialize timers
  keyerInit(&keyer);
}

void loop() {
  updateKeyIndicator(&(keyer.ind_timer));
  updateKeyLED(&(keyer.key_led_timer));
  
  if (digitalRead(MODE_SW) == LOW) {
    autoKeyer(&keyer);
  } else {
    manualKeyer(&keyer);
  }
  
  delay(10); //good for filtering out extra pulses
}

/* Main Process Functions */

void manualKeyer(Keyer* keyer) {
  //set mode leds
  setModeLEDs(MNL);

  switch (getState(&(keyer->keyer_timer))) {
    case OFF:
      if (keyer->dot_t) { //if dot_t is defined
        //waiting to finish characters and words
        int wait_t = getTime(&(keyer->keyer_timer));
        waitingToFinish(keyer, wait_t);
      }
      
      if (digitalRead(KEY_IN) == LOW || digitalRead(DASH_IN) == LOW) { //start of a pulse
        //set indicators high
        setIndicators(HIGH);
        
        //enable new words and/or characters
        resetCW(keyer);
        
        //set the timer
        setState(&(keyer->keyer_timer), ON);
        setTimer(&(keyer->keyer_timer), 0);
      }
    break;
    case ON:
      if (digitalRead(KEY_IN) == HIGH && digitalRead(DASH_IN) == HIGH) {//end of a pulse
        //set indicators low
        setIndicators(LOW);
        
        //append the new pulse length
        appendPulse(&(keyer->pulses), getTime(&(keyer->keyer_timer)));
        
        //update dot_t
        int avg_dot_t = averageDotTime(&(keyer->pulses), DASH_MULT, DASH_THRESH);
        if (avg_dot_t != -1) keyer->dot_t = avg_dot_t; //update the dot_t
        if (keyer->dot_t < MIN_DOT_T) keyer->dot_t = MIN_DOT_T;
        
        //set the timer
        setState(&(keyer->keyer_timer), OFF);
        setTimer(&(keyer->keyer_timer), 0);
      }
    break;
    default:
      setState(&(keyer->keyer_timer), OFF);
    break;
  }
}

void autoKeyer(Keyer* keyer) {
  //set mode leds
  setModeLEDs(AUTO);
  
  if (keyer->dot_t) { //only works if dot_t is set
    boolean dot_pressed = digitalRead(KEY_IN) == LOW;
    boolean dash_pressed = digitalRead(DASH_IN) == LOW;

    //queue pulses on press
    if (digitalRead(KEY_IN) == HIGH) keyer->dot_pressed = false;
    if (digitalRead(KEY_IN) == LOW && keyer->dot_pressed == false) { //on dot press
      //set queue if queue is not set
      if (keyer->queue == UNDEF) keyer->queue = DOT;
      //dot overrides dash if the current pulse is a dash
      if (getState(&(keyer->keyer_timer)) == DASH && keyer->queue == DASH) keyer->queue = DOT;
      keyer->dot_pressed = true;
    }
    if (digitalRead(DASH_IN) == HIGH) keyer->dash_pressed = false;
    if (digitalRead(DASH_IN) == LOW && keyer->dash_pressed == false) { //on dash press
      //set queue if queue is not set
      if (keyer->queue == UNDEF) keyer->queue = DASH;
      //dot overrides dot if the current pulse is a dot
      if (getState(&(keyer->keyer_timer)) == DOT && keyer->queue == DOT) keyer->queue = DASH;
      keyer->dash_pressed = true;
    }

    //soft queue on held inputs
    if (keyer->dash_pressed && getState(&(keyer->keyer_timer)) != DASH) keyer->soft_queue = DASH;
    if (keyer->dot_pressed && getState(&(keyer->keyer_timer)) != DOT) keyer->soft_queue = DOT;

    //iambic mode A
    if (IAMBIC_MODE == MODE_A && !(keyer->dot_pressed) && !(keyer->dash_pressed)) keyer->soft_queue = UNDEF;

    //finished pulses
    if (timerFinished(&(keyer->keyer_timer)) && getState(&(keyer->keyer_timer)) != UNDEF) {
      Serial.println("finished pulse");
      setState(&(keyer->keyer_timer), UNDEF);
      
      //reset the timer
      setTimer(&(keyer->keyer_timer), 0);
    }

    //start pulses from queue
    if (getState(&(keyer->keyer_timer)) == UNDEF) {
      //if queue is UNDEF try using soft_queue
      if (keyer->queue == UNDEF) keyer->queue = keyer->soft_queue;
      
      //trigger pulse based on queue
      switch (keyer->queue) {
        case DOT:
          //enable new words and/or characters
          resetCW(keyer);
          //trigger a dot
          Serial.println("dot triggered");
          appendPulse(&(keyer->pulses), keyer->dot_t);
          setTimer(&(keyer->ind_timer), keyer->dot_t);
          setTimer(&(keyer->keyer_timer), 2*(keyer->dot_t));
          setState(&(keyer->keyer_timer), DOT);
          //clear queues
          keyer->queue = UNDEF;
          keyer->soft_queue = UNDEF;
        break;
        case DASH:
          //enable new words and/or characters
          resetCW(keyer);
          //trigger a dash
          Serial.println("dash triggered");
          appendPulse(&(keyer->pulses), int(DASH_MULT*float(keyer->dot_t)));
          setTimer(&(keyer->ind_timer), int(DASH_MULT*float(keyer->dot_t)));
          setTimer(&(keyer->keyer_timer), int((DASH_MULT + 1.0)*float(keyer->dot_t)));
          setState(&(keyer->keyer_timer), DASH);
          //clear queues
          keyer->queue = UNDEF;
          keyer->soft_queue = UNDEF;
        break;
      }
    }

    //if state is still UNDEF, wait to finish
    if (getState(&(keyer->keyer_timer)) == UNDEF) { //waiting for input
      int wait_t = getTime(&(keyer->keyer_timer)) + keyer->dot_t;
      waitingToFinish(keyer, wait_t);
    }
  }
}

//completes characters and words when over the time thresholds
void waitingToFinish(Keyer* keyer, int wait_t) {
  if (wait_t > W_FIN_THRESH*(float)(keyer->dot_t) && !(keyer->word_finished)) { //word is now finished
    //type space
    Keyboard.write(' ');
    
    //trigger the key led
    setTimer(&(keyer->key_led_timer), KEY_LED_PULSE);

    //word is finished
    keyer->word_finished = true;
  }
  
  if (wait_t > C_FIN_THRESH*(float)(keyer->dot_t) && !(keyer->char_finished)) { //character is now finished
    //decipher and type the character
    typeCharacter(keyer);
    
    //reset the pulse list
    resetPulseList(&(keyer->pulses));

    //character is finished
    keyer->char_finished = true;
  }
  
}

//types the character in the keyer.pulse_list
void typeCharacter(Keyer* keyer) {
  //translate
  String code = pulsesToDotDashCode(&(keyer->pulses), keyer->dot_t, DASH_THRESH);
  char c = dotDashCodeToChar(code);
  
  //check for a shift
  if (c == SHIFT) {
    keyer->shifted = true;
    //turn the key_led on
    digitalWrite(KEY_LED, HIGH);
  } else if (c != 0x00) { //if it isn't a shift or empty, type it
    if (keyer->shifted) {
      // press shift
      Keyboard.press(0x81);
      keyer->shifted = false;
    }
    //type the character
    Keyboard.write(c);
    Keyboard.releaseAll();

    //trigger the key led
    setTimer(&(keyer->key_led_timer), KEY_LED_PULSE);
  }
  
  //disable word completion if character is in OMIT_SPACE
  for (int i = 0; i < sizeof(OMIT_SPACE); i++) {
    if (c == OMIT_SPACE[i]) keyer->word_finished = true;
  }
}

void setIndicators(boolean val) {
  digitalWrite(IND_LED, val);
  digitalWrite(BUZZER, val);
}

void setModeLEDs(boolean mode) {
  digitalWrite(MNL_LED, mode == MNL);
  digitalWrite(AUTO_LED, mode == AUTO);
}

//set the key indicator based on the inputted timer
void updateKeyIndicator(StateTimer* timer) {
  switch (getState(timer)) {
    case OFF:
      if (!timerFinished(timer)) {
        setIndicators(HIGH);
        setState(timer, ON);
      }
    break;
    case ON:
      if (timerFinished(timer)) {
        setIndicators(LOW);
        setState(timer, OFF);
      }
    break;
    default:
      setState(timer, OFF);
    break;
  }
}

//set the key LED based on the inputted timer
void updateKeyLED(StateTimer* timer) {
  switch (getState(timer)) {
    case OFF:
      if (!timerFinished(timer)) {
        digitalWrite(KEY_LED, HIGH);
        setState(timer, ON);
      }
    break;
    case ON:
      if (timerFinished(timer)) {
        digitalWrite(KEY_LED, LOW);
        setState(timer, OFF);
      }
    break;
    default:
      setState(timer, OFF);
    break;
  }
}

/* End of Main Process Functions */

/* KeyerData Functions */

//KeyerData initialization
void keyerInit(Keyer* keyer) {
  //set the default dot_t
  keyer->dot_t = DEFAULT_DOT_T;
  
  //reset pulse data
  resetPulseList(&(keyer->pulses));
  
  //both characters and words are finished
  keyer->char_finished = true;
  keyer->word_finished = true;

  //the next character is not shifted
  keyer->shifted = false;

  //backspace count start at 0
  keyer->backspace_count = 0;
  
  //set pressed trackers to false
  keyer->dot_pressed = false;
  keyer->dash_pressed = false;
  
  //initialize the timers
  setTimer(&(keyer->keyer_timer), 0); setState(&(keyer->keyer_timer), UNDEF);
  setTimer(&(keyer->ind_timer), 0); setState(&(keyer->ind_timer), OFF);
  setTimer(&(keyer->key_led_timer), 0); setState(&(keyer->key_led_timer), OFF);

  //set queue to UNDEF
  keyer->queue = UNDEF;
  keyer->soft_queue = UNDEF;
}

//reanables char and word completion
void resetCW(Keyer* keyer) {
  keyer->char_finished = false;
  keyer->word_finished = false;
}

/* End of KeyerData Functions */

/* StateTimer Functions */

// sets the timer by setting t_start ahead of millis()
void setTimer(StateTimer* timer, uint16_t duration) {
  timer->t_start = millis() + duration;
}

// sets the state
void setState(StateTimer* timer, uint8_t state) {
  timer->state = state;
}

//returns the time
int getTime(StateTimer* timer) {
  return millis() - timer->t_start;
}

// returns the timer state
uint8_t getState(StateTimer* timer) {
  return timer->state;
}

// returns true if time >= 0
boolean timerFinished(StateTimer* timer) {
  return getTime(timer) >= 0;
}

/* End of Timer Functions */

/* Pulses Functions */

//appends a new pulse value to a pulse list
void appendPulse(Pulses* pulses, int pulse_t) {
  if (pulses->len < sizeof(pulses->list)/sizeof(pulses->list[0])) {
    pulses->list[pulses->len] = pulse_t;
    pulses->len++; //increment index
  }
}

//resets the index
void resetPulseList(Pulses* pulses) {
  pulses->len = 0;
}

//returns an average dot_t from pulse data, returns -1 if data is inconclusive
int averageDotTime(Pulses* pulses, float dash_mult, float dash_thresh) {
  //counter and state variables
  int pulse_type = UNDEF; //pulse reading state
  float dot_sum = 0.0;
  int dot_count = 0;

  //iterate through all the pulses comparing two pulses
  uint16_t prev_pulse = pulses->list[0];
  for (int i = 1; i < pulses->len; i++) {
    uint16_t current_pulse = pulses->list[i];
    if (current_pulse > dash_thresh*(float)prev_pulse) { //changes from dot to dash
      if (pulse_type == UNDEF) { //if the previous pulses were undefined
        //the previous pulses are dots
        for (int j = i-1; j >= 0; j--) {
          dot_sum += (float)(pulses->list[j]);
          dot_count++;
        }
      }
      //the current pulse is a dash
      pulse_type = DASH;
    } else if (current_pulse < (float)prev_pulse/dash_thresh) { //changes from dash to dot
      if (pulse_type == UNDEF) { //if the previous pulses were undefined
        //the previous pulses are dashes
        for (int j = i-1; j >= 0; j--) {
          dot_sum += (float)(pulses->list[j])/dash_mult;
          dot_count++;
        }
      }
      //the current pulse is a dot
      pulse_type = DOT;
    }
    if (pulse_type == DOT) {
      dot_sum += (float)current_pulse;
      dot_count++;
    } else if (pulse_type == DASH) {
      dot_sum += (float)current_pulse/dash_mult;
      dot_count++;
    }
    //the current pulse will now be the previous pulse
    prev_pulse = current_pulse;
  }

  //return the average!
  if (dot_count > 0) { //if there is data
    return int(dot_sum/(float)dot_count); //return the average
  }
  return -1; //otherwise return -1
}

//returns a dot dash string ie. ".-.." from a pulse array
String pulsesToDotDashCode(Pulses* pulses, uint16_t dot_t, float dash_thresh) {
  String output = "";
  //iterate through all the pulses and append output
  for (int i = 0; i < pulses->len; i++) {
    if (pulses->list[i] > (float)dot_t*dash_thresh) output += '-'; //dash
    else output += '.'; //dot
  }
  return output;
}

/* End of Pulses Functions */

/* deciphering functions */

char dotDashCodeToChar(String code) {
  //returns the corresponding char from the CODE_INDEX
  //format: Tab|DotDashCode|Space
  code = '\t' + code + ' ';
  //find the location
  int i = CODE_INDEX.indexOf(code);
  //return the char after the code
  if (i != -1) {
    return CODE_INDEX.charAt(i + code.length());
  }
  //else return 0
  return 0x00;
}
