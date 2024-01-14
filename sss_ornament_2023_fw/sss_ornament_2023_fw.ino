#include <avr/sleep.h>

#define PANO_LED_R_PIN PIN_PA7
#define PANO_LED_G_PIN PIN_PA2
#define PANO_LED_B_PIN PIN_PA1
#define HOUSE_LED_PIN PIN_PA6
#define PIR_IN PIN_PA3

/*
 * Calling it duty cycle but it's the 0-255 value used for analogWrite PWM
 */
#define PANO_LED_DUTY_CYCLE (70)

/*
 * Value between 0 (fully off) and 0xFFF (fully on)
 * Set for house LEDs manually by finding a balance between bright enough to look good, while dim enough to not waste a ton of power since they're always on while awake
 */
#define HOUSE_LED_CMPCLR (0x0FF)

#define SLEEP_TIMEOUT_MIN (15)
#define SLEEP_TIMEOUT_MS (1000UL * 60 * SLEEP_TIMEOUT_MIN)

#if F_CPU != 1000000
#error Must set clock to 1MHz and must burn bootloader for 1MHz operation! PWM for pano LEDs and the delay() timing is fucky with 20MHz clock when we use both TCA (analogWrite) and TCD
#endif

typedef enum {
  PANO_COLOR_RED,
  PANO_COLOR_GREEN,
  PANO_COLOR_BLUE,
  PANO_COLOR_BROWN,
  PANO_COLOR_CYAN,
  PANO_COLOR_YELLOW,

  PANO_COLOR_COUNT,
} pano_color_t;

volatile uint32_t start_ms;
pano_color_t current_pano_color;

/*
 * PIR ISR to reset timeout start_ms  
 */
ISR(PORTA_PORT_vect) {
  PORTA.INTFLAGS = 0xFF;

  // Refresh start time every time PIR triggers. millis doesn't increment in an ISR but we don't
  // care, just refresh to the most recent time. It's also fast enough and ISR safe to do here.
  start_ms = millis();
}

/*
 * Write specified 0-255 duty cycle to correct RGB pins depending on color
 */
void pano_led_write(pano_color_t color, uint8_t duty_cycle) {
    switch (color) {
      case PANO_COLOR_RED:
        analogWrite(PANO_LED_R_PIN, duty_cycle);
        digitalWrite(PANO_LED_G_PIN, LOW);
        digitalWrite(PANO_LED_B_PIN, LOW);
        break;
      case PANO_COLOR_GREEN:
        analogWrite(PANO_LED_G_PIN, duty_cycle);
        digitalWrite(PANO_LED_R_PIN, LOW);
        digitalWrite(PANO_LED_B_PIN, LOW);
        break;
      case PANO_COLOR_BLUE:
        analogWrite(PANO_LED_B_PIN, duty_cycle);
        digitalWrite(PANO_LED_R_PIN, LOW);
        digitalWrite(PANO_LED_G_PIN, LOW);
        break;
      case PANO_COLOR_BROWN:
        analogWrite(PANO_LED_R_PIN, duty_cycle);
        analogWrite(PANO_LED_G_PIN, duty_cycle);
        digitalWrite(PANO_LED_B_PIN, LOW);
        break;
      case PANO_COLOR_CYAN:
        analogWrite(PANO_LED_G_PIN, duty_cycle);
        analogWrite(PANO_LED_B_PIN, duty_cycle);
        digitalWrite(PANO_LED_R_PIN, LOW);
        break;
      case PANO_COLOR_YELLOW:
        analogWrite(PANO_LED_R_PIN, duty_cycle);
        analogWrite(PANO_LED_B_PIN, duty_cycle);
        digitalWrite(PANO_LED_G_PIN, LOW);
        break;
       default:
        digitalWrite(PANO_LED_R_PIN, LOW);
        digitalWrite(PANO_LED_G_PIN, LOW);
        digitalWrite(PANO_LED_B_PIN, LOW);
    }
}

/*
 * Fade in / out in specified timeframes
 */
void pano_led_pulse(pano_color_t color, uint32_t on_duration_ms, uint32_t off_duration_ms) {
  int step_delay_ms = 10;
  
  int steps = on_duration_ms / step_delay_ms;
  int duty_increment = PANO_LED_DUTY_CYCLE / steps;
  for (int new_duty = 0; new_duty <= PANO_LED_DUTY_CYCLE; new_duty += duty_increment) {
    pano_led_write(color, new_duty);
    delay(step_delay_ms);
  }

  steps = off_duration_ms / step_delay_ms;
  duty_increment = PANO_LED_DUTY_CYCLE / steps;
  for (int new_duty = PANO_LED_DUTY_CYCLE; new_duty >= 0; new_duty -= duty_increment) {
    pano_led_write(color, new_duty);
    delay(step_delay_ms);
  }
}

void deep_sleep() {
  // Turn off all LEDs before sleeping
  digitalWrite(PANO_LED_R_PIN, LOW);
  digitalWrite(PANO_LED_G_PIN, LOW);
  digitalWrite(PANO_LED_B_PIN, LOW);
  digitalWrite(HOUSE_LED_PIN, LOW);
  
  sleep_enable();
  set_sleep_mode(SLEEP_MODE_STANDBY);
  sleep_cpu();

  // Disable once (if) we wake back up and refresh sleep timer
  start_ms = millis();
  sleep_disable();
}

void house_led_pwm_init(void) {
    // Enable-protected reg must have 0 written to enable bit before any other bits can be changed, and it defaults to
    // enabled on startup
    TCD0.CTRLA &= ~TCD_ENABLE_bm;

    // Don't need overlapping PWM signals so just do oneramp
    TCD0.CTRLB = TCD_WGMODE_ONERAMP_gc;

    // Disable all input control
    TCD0.INPUTCTRLA = TCD_INPUTMODE_NONE_gc;

    // Set/clear values to create desired duty cycle. Don't care about CMPB (outputs on PA7) at all so leave it off
    TCD0.CMPASET = 0x000;
    TCD0.CMPACLR = HOUSE_LED_CMPCLR;
    
    // System 1MHz clock w/ DIV4 standard prescaler but no synchronization prescaler (I think they multiply together, not add).
    // Must be done as last operation before starting timer with ENABLE bit
    // NOTE :: DEFAULT TIMER FOR MILLIS/MICROS FUNCTIONS ON 1-SERIES ATTINIES IS TCD. I think this still works with it set as the default,
    // but if not, try switching it to TCB0.
    TCD0.CTRLA = TCD_CLKSEL_SYSCLK_gc | TCD_CNTPRES_DIV4_gc | TCD_SYNCPRES_DIV1_gc;
}

void house_led_pwm_start(void) {
  // Turn off output override (because we want pwm to run it duh)
  TCD0.CTRLC &= ~TCD_CMPOVR_bm;
    
  // Enable WOA (PA6) but not and WOB/C/D (PA7/?/?). Since FAULTCTRL reg is write protected we need to make it editable in
  // CCP reg. Must be done in start func because we can't take over output pins in stop func unless we disable them in FAULTCTRL.
  CPU_CCP        = CCP_IOREG_gc;
  TCD0.FAULTCTRL = TCD_CMPAEN_bm;
  
  while (!(TCD0.STATUS & TCD_ENRDY_bm)) {
      ;
  }

  TCD0.CTRLA |= TCD_ENABLE_bm;
}

/*
 * Must be called every time TCD0.CMPASET or TCD0.CMPACLR are updated for them to take effect if they're changed while PWM running.
 */
void house_led_pwm_sync(void) {
    TCD0.CTRLE = TCD_SYNCEOC_bm;
}


void setup() {
  pinMode(PANO_LED_R_PIN, OUTPUT);
  pinMode(PANO_LED_G_PIN, OUTPUT);
  pinMode(PANO_LED_B_PIN, OUTPUT);
  pinMode(HOUSE_LED_PIN, OUTPUT);

  house_led_pwm_init();
  house_led_pwm_start();

  current_pano_color = PANO_COLOR_RED;

  // Set up interrupt on PIR pin. Must be BOTHEDGES or LEVEL since we're using a partially-async pin. If using PA2
  // or PA6 (fully async), can wake from deep sleep with any trigger.
  PORTA.PIN3CTRL = (PORTA.PIN3CTRL & ~PORT_ISC_gm) | PORT_ISC_BOTHEDGES_gc | PORT_PULLUPEN_bm;
  
  sei();
}

void loop() {
//   If we've reached timeout with nothing, that means either no movement or constant movement for the timeout period.
//   If the PIR pin is still high, that means it's continuously reported movement, so just refresh the timeout time. If
//   it's low, then the timeout has actually elapsed with no trigger, so sleep.
  if ((millis() - start_ms) > SLEEP_TIMEOUT_MS) {
    if (digitalRead(PIR_IN) == LOW) {
      deep_sleep();
    } else {
      start_ms = millis();
    }
  }

// 150BPM I think...
  pano_led_pulse(current_pano_color, 200, 200);
  pano_led_pulse(current_pano_color, 200, 200);
  pano_led_pulse(current_pano_color, 200, 200);

  pano_led_pulse(current_pano_color, 100, 100);
  pano_led_pulse(current_pano_color, 100, 100);

  current_pano_color = (pano_color_t)((uint8_t)current_pano_color + 1);
  if (current_pano_color == PANO_COLOR_COUNT) {
    current_pano_color = PANO_COLOR_RED;
  }
}
