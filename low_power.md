# Low Power


Did the following experiment with an Arduino Uno:

```
// Without SLEEP_MODE_*       at 1kHz: 42mA max: 43mA - Doesn't go up with frequency
// With    SLEEP_MODE_IDLE    at 1kHz: 35mA max: 38mA - Goes up with frequency
// With    SLEEP_MODE_STANDBY at 1kHz: 30mA max: 34mA - Goes up with frequency
#include <avr/sleep.h>

// Should support ISR on change
#define PIN_IR_RECEIVER 2
#define PIN_IR_LED 3

void setup() {
  pinMode(PIN_IR_RECEIVER, INPUT_PULLUP);
  pinMode(PIN_IR_LED, OUTPUT);

  digitalWrite(PIN_IR_LED, LOW);

  attachInterrupt(digitalPinToInterrupt(PIN_IR_RECEIVER), pinChangeISR, CHANGE);

  set_sleep_mode(SLEEP_MODE_STANDBY);
  sleep_enable();
}

void loop() {
  sleep_mode();
  //delay(1000);
}

void pinChangeISR() {
  int val = digitalRead(PIN_IR_RECEIVER);
  digitalWrite(PIN_IR_LED, val);
}
```

In every case I saw the output mirror the input with 10-11uS of delay. This shows that `CHANGE` interrupt works even with `SLEEP_MODE_STANDBY`. The same code with `SLEEP_MODE_PWR_DOWN` has 1ms+ delays i.e. doesn't work as expected from the manual.

Tried the main timer/pin ISR mechanism on Arduino Uno and Arduino Mini Pro:

```
#include <avr/sleep.h>

// Should support ISR on change
#define PIN_IR_RECEIVER 2
#define PIN_IR_LED 3

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_IR_RECEIVER, INPUT_PULLUP);
  pinMode(PIN_IR_LED, OUTPUT);

  digitalWrite(PIN_IR_LED, LOW);

  attachInterrupt(digitalPinToInterrupt(PIN_IR_RECEIVER), pinChangeISR, CHANGE);

  noInterrupts();
  TCCR1A = 0;              // Set Timer1 to Normal mode
  TCCR1B = 0;              // Set Timer1 to Normal mode
  TCNT1 = 0;               // Initialize counter to 0
  TCCR1B |= (1 << CS11);   // Set prescaler to 8 (1 tick = 0.5 µs)
  TIMSK1 |= (1 << TOIE1);  // Enable Timer1 overflow interrupt
  interrupts();

  set_sleep_mode(SLEEP_MODE_STANDBY);
  sleep_enable();
}

volatile bool timer1on = false;

volatile int duration = 0;
int cnt = 0;

void enableTimer1() {
  TCNT1 = 0;               // Initialize counter to 0
  TCCR1B |= (1 << CS11);   // Set prescaler to 8 (1 tick = 0.5 µs)
  TIMSK1 |= (1 << TOIE1);  // Enable Timer1 overflow interrupt
  timer1on = true;
}

void disableTimer1() {
  TIMSK1 &= ~(1 << TOIE1);  // Disable Timer1 overflow interrupt
  TCCR1B &= ~(1 << CS11);   // Set prescaler to 0 (No Input)
  
  TCNT1 = 0;                // Initialize counter to 0
  timer1on = false;
}

void loop() {
  if (++cnt > 100) {
    // At 4khz 125uS pulse => with prescaler of 8 => 0.5us Timer tick
    // duration should often be 250.

    float half_period_duration_us = (float)duration * 0.5;
    Serial.print(half_period_duration_us);
    Serial.print(" ");
    Serial.println(1. / (2 * 0.000001 * half_period_duration_us));
    cnt = 0;
  }
  delay(1);

  if (!timer1on) {
    Serial.println("Sleep");
    delay(1);
    sleep_mode();
  }
}

ISR(TIMER1_OVF_vect) {
  disableTimer1();
}

void pinChangeISR() {
  int val = digitalRead(PIN_IR_RECEIVER);
  digitalWrite(PIN_IR_LED, val);

  if (!timer1on) {
    enableTimer1();
  }


  duration = TCNT1;

  TCNT1 = 0;
}
```

It all works as expected, confirming that the timer counts find despite the occassional power downs. I used a 40Hz square to make sure measurments are ok. Then I switched to a 1% duty cycle 10Hz pulse that allows the timer to overflow and the processor enter the sleep mode. Still, it measures the pulse length correctly:

```
973.00 513.87
Sleep
Sleep
Sleep
973.00 513.87
...
```

With this process I confirmed that the Arduino ISRs and timers work fine when in sleep mode. This means that if the Arduino behaves in an unexpected way, it's due to race conditions. I've implemented a double-buffer solution to mitigate such race conditions.
