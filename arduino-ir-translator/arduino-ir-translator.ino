#include <avr/sleep.h>
#include <avr/wdt.h>
#include <avr/interrupt.h>

#define BUFFER_LIMIT 200

// Solution to (2^16 - TIMEOUT_OFFSET) * 0.5uS = 12ms
// We want timeout at 12ms from the last pulse because
// that's when we know for sure it's the end of a packet
#define TIMEOUT_OFFSET 41536

struct Frame {
  unsigned long times[BUFFER_LIMIT];
  int len;
  void reset() {
    len = 0;
  }
  Frame() {
    reset();
  }
};

Frame b1, b2;  // Double buffer scheme
Frame* read;   // This can be NULL or a buffer for the consumer (loop) to process
Frame* write;  // This always points to a write buffer for producers (ISRs)

bool timer1on = false;

// Should support ISR on change
#define PIN_IR_RECEIVER 2
#define PIN_IR_LED 3

void debugPrint(const Frame& frame) {
  Serial.print("code (");
  Serial.print(frame.len);
  Serial.print("): ");
  for (int i = 0; i < frame.len; ++i) {
    Serial.print((float)frame.times[i] / 2000);
    Serial.print(" ");
  }
  Serial.println();
}

// One ms is 2k * .5us which is the timer clock
#define MS_TO_TICK(x) ((int)(x * 2000))
#define TONE_OVERHEAD_US 360

void send(int* decoded) {
  tone(PIN_IR_LED, 38000);  // Start
  delay(9);
  noTone(PIN_IR_LED);

  delayMicroseconds(4500);

  for (int i = 0; i < 32; ++i) {
    tone(PIN_IR_LED, 38000);
    delayMicroseconds(610 - TONE_OVERHEAD_US);
    noTone(PIN_IR_LED);

    if (decoded[i >> 3] & (1 << (7 - (i & 7)))) {
      delayMicroseconds(1670);
    } else {
      delayMicroseconds(520);
    }
  }

  // Stop
  tone(PIN_IR_LED, 38000);
  delayMicroseconds(620 - TONE_OVERHEAD_US);
  noTone(PIN_IR_LED);
}

void sendOn() {
  int decoded[] = { 0, 255, 98, 157 };
  send(decoded);
}

void send6h() {
  int decoded[] = { 0, 255, 168, 87 };
  send(decoded);
}

void sendOffOnce() {
  int decoded[] = { 0, 255, 24, 231 };
  send(decoded);
}

void sendOffTwice() {
  int decoded[] = { 0, 255, 24, 231 };
  send(decoded);
  delay(300);
  send(decoded);
}

void sendOffThreeTimes() {
  int decoded[] = { 0, 255, 24, 231 };
  send(decoded);
  delay(300);
  send(decoded);
  delay(300);
  send(decoded);
}

int decodeDisco(const Frame& frame, int* decoded) {
  // Button ON:  decoded: 0 255 98 157
  // Button 6H:  decoded: 0 255 168 87
  // Button OFF: decoded: 0 255 24 231

  // All times milliseconds
  // times[0]; // This is random - time of first pulse after last timeout
  // times[1,2] // 9.14 4.47 low and high-edge

  // 8 Bits of zero
  // 0.61 0.52 | 0.61 0.52 | 0.61 0.52 | 0.61 0.52 | 0.61 0.52 | 0.61 0.52 | 0.61 0.52 | 0.61 0.52
  // 8 bits of one
  // 0.61 1.67 | 0.61 1.67 | 0.61 1.67 | 0.61 1.67 | 0.61 1.67 | 0.61 1.67 | 0.61 1.67 | 0.61 1.67
  // Actual code and its complement:
  // 0.61 0.52 | 0.61 1.67 | 0.61 1.67 | 0.61 0.52 | 0.61 0.52 | 0.61 0.52 | 0.61 1.67 | 0.61 0.52
  // 0.61 1.67 | 0.61 0.52 | 0.61 0.52 | 0.61 1.67 | 0.61 1.67 | 0.61 1.67 | 0.61 0.52 | 0.61 1.67

  // 0.59 // Last raising:

  if (frame.len != 68) {
    return -1;
  }

  const unsigned long* times = frame.times;

  // Start
  if (!(times[1] > MS_TO_TICK(7.5) && times[1] < MS_TO_TICK(10))) {
    return -2;
  }
  if (!(times[2] > MS_TO_TICK(3.5) && times[2] < MS_TO_TICK(5))) {
    return -3;
  }

  for (int i = 0; i < 32; ++i) {
    int high = times[i * 2 + 3];
    int low = times[i * 2 + 4];

    bool zero = (high > MS_TO_TICK(0.5) && high < MS_TO_TICK(0.8)) && (low > MS_TO_TICK(0.5) && low < MS_TO_TICK(0.8));
    bool one = (high > MS_TO_TICK(0.5) && high < MS_TO_TICK(0.8)) && (low > MS_TO_TICK(1.5) && low < MS_TO_TICK(1.8));
    if (!(one ^ zero)) {
      return -4;
    }
    decoded[i >> 3] <<= 1;
    if (one) {
      ++decoded[i >> 3];
    }
  }

  if ((decoded[0] + decoded[1]) != 255) {
    return -5;
  }

  if ((decoded[2] + decoded[3]) != 255) {
    return -6;
  }

  return 0;
}

void decodeDiscoAndPrint(const Frame& frame) {
  int decoded[4] = { 0, 0, 0, 0 };
  int rc = decodeDisco(frame, decoded);
  if (rc == 0) {
    Serial.print("decoded: ");
    for (int i = 0; i < 4; ++i) {
      Serial.print(decoded[i]);
      Serial.print(" ");
    }
    Serial.println();
  }
}


int decodeLg(const Frame& frame, int* decoded) {
  // key 1: decoded: 32 223 136 119
  // key 2: decoded: 32 223 72 183
  // key 3: decoded: 32 223 40 215
  // key 4: decoded: 32 223 200 55
  // key 5: decoded: 32 223 168 87

  if (frame.len != 68) {
    return -1;
  }

  const unsigned long* times = frame.times;

  // Start
  if (!(times[1] > MS_TO_TICK(7.5) && times[1] < MS_TO_TICK(10))) {
    return -2;
  }
  if (!(times[2] > MS_TO_TICK(3.5) && times[2] < MS_TO_TICK(5))) {
    return -3;
  }

  for (int i = 0; i < 32; ++i) {
    int high = times[i * 2 + 3];
    int low = times[i * 2 + 4];

    bool zero = (high > MS_TO_TICK(0.38) && high < MS_TO_TICK(0.75)) && (low > MS_TO_TICK(0.25) && low < MS_TO_TICK(0.75));
    bool one = (high > MS_TO_TICK(0.38) && high < MS_TO_TICK(0.75)) && (low > MS_TO_TICK(1.25) && low < MS_TO_TICK(1.75));
    if (!(one ^ zero)) {
      Serial.print("Error in digit: ");
      Serial.print(i);
      Serial.print(": ");
      Serial.print((float)(high * 2) / 1000);
      Serial.print(" ");
      Serial.println((float)(low * 2) / 1000);
      return -4;
    }
    decoded[i >> 3] <<= 1;
    if (one) {
      ++decoded[i >> 3];
    }
  }

  if ((decoded[0] + decoded[1]) != 255) {
    return -5;
  }

  if ((decoded[2] + decoded[3]) != 255) {
    return -6;
  }

  return 0;
}

// Remote Control: LG 32LE4500
void decodeLGAndPrint(const Frame& frame) {
  int decoded[4] = { 0, 0, 0, 0 };
  int rc = decodeLg(frame, decoded);
  if (rc == 0) {
    Serial.print("decoded: ");
    for (int i = 0; i < 4; ++i) {
      Serial.print(decoded[i]);
      Serial.print(" ");
    }
    Serial.println();
  } else {
    debugPrint(frame);
  }
}

void enableTimer1() {
  TCNT1 = TIMEOUT_OFFSET;  // Initialize counter to 0
  TCCR1B |= (1 << CS11);   // Set prescaler to 8 (1 tick = 0.5 µs)
  TIMSK1 |= (1 << TOIE1);  // Enable Timer1 overflow interrupt
  timer1on = true;
}

void disableTimer1() {
  TIMSK1 &= ~(1 << TOIE1);  // Disable Timer1 overflow interrupt
  TCCR1B &= ~(1 << CS11);   // Set prescaler to 0 (No Input)

  TCNT1 = TIMEOUT_OFFSET;  // Initialize counter to 0
  timer1on = false;
}

void setup() {
  Serial.begin(115200);
  delay(300);

  pinMode(PIN_IR_RECEIVER, INPUT_PULLUP);
  pinMode(PIN_IR_LED, OUTPUT);
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(PIN_IR_LED, LOW);
  digitalWrite(LED_BUILTIN, LOW);

  read = NULL;
  write = &b1;
  write->reset();

  noInterrupts();
  TCCR1A = 0;  // Set Timer1 to Normal mode
  TCCR1B = 0;  // Set Timer1 to Normal mode
  // enableTimer1(); // Only enable on pin change
  interrupts();

  attachInterrupt(digitalPinToInterrupt(PIN_IR_RECEIVER), pinChangeISR, CHANGE);

  // set_sleep_mode(SLEEP_MODE_IDLE);
  // set_sleep_mode(SLEEP_MODE_PWR_SAVE);
  set_sleep_mode(SLEEP_MODE_STANDBY);
  // set_sleep_mode(SLEEP_MODE_PWR_DOWN);

  // sendOn();
  // send6h();
  // sendOffOnce();
  // sendOffTwice();
  // sendOffThreeTimes();
}

void processRead(const Frame& frame) {
  //decodeDiscoAndPrint();
  //debugPrint();
  //decodeLGAndPrint();
  int decoded[4] = { 0, 0, 0, 0 };
  int rc = decodeLg(frame, decoded);
  if (rc != 0) {
    return;
  }

  const int unit = decoded[0];
  if (unit != 32) {
    return;
  }

  const int command = decoded[2];
  if (command <= 0) {
    return;
  }

  // Let the old command settle down. That's
  // 500 ms to settle on top of 200ms so that
  // we get rid of up to two repeats and their
  // timeouts
  delay(700);

  switch (command) {
    case 136:  // Key 1
      Serial.println("Sending on");
      sendOn();
      break;
    case 72:  // Key 2
      Serial.println("Sending 6h");
      send6h();
      break;
    case 40:  // Key 3
      Serial.println("Sending off once");
      sendOffOnce();
      break;
    case 200:  // Key 4
      Serial.println("Sending off twice");
      sendOffTwice();
      break;
    case 168:  // Key 5
      Serial.println("Sending off three times");
      sendOffThreeTimes();
      break;
    default:
      return;  // Don't blink the led
  }

  digitalWrite(LED_BUILTIN, HIGH);
  delay(200);
  digitalWrite(LED_BUILTIN, LOW);
}

void loop() {
  Frame* frame = NULL;
  noInterrupts();
  if (read) {
    frame = read;
    read = NULL;
  }
  interrupts();

  if (frame) {
    processRead(*frame);
    frame->reset();
  }

  // Avoid test race condition - see:
  // https://www.nongnu.org/avr-libc/user-manual/group__avr__sleep.html
  cli();
  if (!timer1on && !read) {
    sleep_enable();
    sei();
    sleep_cpu();
    sleep_disable();
  }
  sei();
}

// Timeout. On next wake up do `timout()`
ISR(TIMER1_OVF_vect) {

  // No need to disable interrupts while we're in the ISR
  if (!read) {
    read = write;
    write = (write == &b1) ? &b2 : &b1;
  }  // implicitly else - discard this write buffer
  write->reset();

  TCNT1 = TIMEOUT_OFFSET;
  disableTimer1();
}

// ISR. Mark time since last pulse
void pinChangeISR() {
  if (!timer1on) {
    enableTimer1();
  }
  if (write->len < BUFFER_LIMIT) {
    write->times[write->len++] = TCNT1 - TIMEOUT_OFFSET;
  }
  TCNT1 = TIMEOUT_OFFSET;
}
