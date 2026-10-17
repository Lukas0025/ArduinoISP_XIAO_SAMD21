/*
 * ArduinoISP pro Seeed XIAO SAMD21 (SAMD21G18A, 3.3 V)
 *
 * Původ: ArduinoISP (c) 2008-2011 Randall Bohn, úpravy pro ESP (pcbreflux, vince).
 * Licence: http://www.opensource.org/licenses/bsd-license.php
 *
 * Tento sketch udělá z XIAO programátor AVR (protokol STK500v1) - použitelný
 * s avrdude (-c stk500v1) i s Arduino IDE ("Arduino as ISP").
 *
 * ZAPOJENÍ (podrobnosti v ArduinoISP_XIAO_SAMD21.md):
 *
 *   XIAO pin        Funkce               Cíl (AVR)
 *   ----------------------------------------------------
 *   D8  (PA07)      SCK   (HW SPI)   ->  SCK
 *   D9  (PA05)      MISO  (HW SPI)   <-  MISO
 *   D10 (PA06)      MOSI  (HW SPI)   ->  MOSI
 *   D4  (PA08)      RESET            ->  RESET
 *   D2  (PA10)      JUMPER "SLOW"    ->  GND = pomalý SW-SPI (~21 kHz)
 *   3V3             napájení cíle    ->  VCC (jen malá zátěž!)
 *   GND             zem              ->  GND
 *
 *   Indikace (vestavěné LED na XIAO, aktivní v LOW):
 *     žlutá  (LED_BUILTIN) = programovací režim / blikne při zápisu
 *     modrá  (RX LED)      = chyba
 *     modrá  (TX LED)      = heartbeat (program běží)
 *
 * !!! XIAO NENÍ 5V TOLERANTNÍ. Cíl napájej 3.3 V, nebo použij převodník úrovní !!!
 */

#include <Arduino.h>
#include <SPI.h>

#ifndef ARDUINO_ARCH_SAMD
#warning "Tento sketch je určen pro SAMD21 (Seeed XIAO SAMD21)."
#endif

// ---------------------------------------------------------------------------
// Konfigurace
// ---------------------------------------------------------------------------

// Pin pro RESET cílového čipu
#define ISP_PIN_RESET   4      // D4

// Pin pro jumper "pomalý režim" (INPUT_PULLUP; spojit s GND = pomalý SW SPI)
#define ISP_PIN_SLOW    2      // D2

// Chování pinu RESET u AVR cílů (aktivní v LOW):
// XIAO RESET jen stahuje do GND (open-drain styl) a při uvolnění ho pustí:
//   INPUT_PULLUP = uvolnit s interním pull-upem XIAO (cíl na 3.3 V, vnější rezistor netřeba)
//   INPUT        = uvolnit bez pull-upu (cíl na 5 V / cíl má vlastní silný pull-up)
//   OUTPUT       = původní chování: aktivně tlačit 3.3 V do RESET (nedoporučeno pro 5V cíl)
#define ISP_RESET_RELEASE  INPUT_PULLUP

// Čekání na dokončení zápisu (flash stránka / bajt EEPROM):
//   0 = pevná prodleva jako v originálním ArduinoISP (nejbezpečnější, výchozí)
//   1 = polling RDY/BSY (rychlejší; jako maximální čas se použije stejná hodnota)
#define ISP_USE_POLL_READY       0
#define ISP_FLASH_WRITE_DELAY_MS  30
#define ISP_EEPROM_WRITE_DELAY_MS 45

// Prodleva po příkazu Chip Erase (0xAC 0x80). Čip se maže ~9 ms; bez čekání
// se může ztratit první příkaz zápisu (první slovo flash zůstane 0xFFFF).
#define ISP_CHIP_ERASE_DELAY_MS   30

// SPI hodiny
//  - rychlý režim: HW SPI. SAMD21 při 48 MHz neumí méně než ~94 kHz,
//    proto 100 kHz. Cíl musí běžet na > 400 kHz (např. výchozí 1 MHz je OK).
//  - pomalý režim: SW (bit-bang) SPI pro cíle na 128 kHz (128000/6 ~ 21 kHz)
#define SPI_CLOCK_FAST  100000UL
#define SPI_CLOCK_SLOW  (128000UL / 6)

// Formát adresy EEPROM, kterou posílá PC příkazem 'U':
//   0 = bajtová adresa (avrdude -c stk500v1, Arduino IDE "Arduino as ISP")  <-- výchozí
//   1 = slovní adresa  (avrdude >= 8.0 s "-c arduino_as_isp", některé jiné programy)
// Flash je VŽDY slovní adresa.
#define EEPROM_ADDR_IS_WORD 0

// Baud rate se u nativního USB (CDC) ignoruje, avrdude ale používá 19200.
#define BAUDRATE        19200

// Při zápisu zhasnout LED "programování"
#define PROG_FLICKER    true

// ---------------------------------------------------------------------------
// Sériový port: nativní USB
// ---------------------------------------------------------------------------
#undef SERIAL
#ifdef SERIAL_PORT_USBVIRTUAL
#define SERIAL SERIAL_PORT_USBVIRTUAL
#else
#define SERIAL Serial
#endif

// ---------------------------------------------------------------------------
// LED (na XIAO jsou aktivní v LOW)
// ---------------------------------------------------------------------------
#define LED_PMODE   LED_BUILTIN
#ifdef PIN_LED_RXL
#define LED_ERR     PIN_LED_RXL
#endif
#ifdef PIN_LED_TXL
#define LED_HB      PIN_LED_TXL
#endif

static inline void led(int pin, bool on) {
  digitalWrite(pin, on ? LOW : HIGH);
}

// ---------------------------------------------------------------------------
// STK500 definice
// ---------------------------------------------------------------------------
#define HWVER 2
#define SWMAJ 1
#define SWMIN 18

#define STK_OK      0x10
#define STK_FAILED  0x11
#define STK_UNKNOWN 0x12
#define STK_INSYNC  0x14
#define STK_NOSYNC  0x15
#define CRC_EOP     0x20   // je to mezera

// ---------------------------------------------------------------------------
// Stav
// ---------------------------------------------------------------------------
static int error = 0;
static int pmode = 0;
static unsigned int here;           // adresa nastavená příkazem 'U'
static uint8_t buff[256];           // blok dat

#define beget16(addr) (*(addr) * 256 + *((addr) + 1))

typedef struct param {
  uint8_t  devicecode;
  uint8_t  revision;
  uint8_t  progtype;
  uint8_t  parmode;
  uint8_t  polling;
  uint8_t  selftimed;
  uint8_t  lockbytes;
  uint8_t  fusebytes;
  uint8_t  flashpoll;
  uint16_t eeprompoll;
  uint16_t pagesize;
  uint16_t eepromsize;
  uint32_t flashsize;
} parameter;

static parameter param;
static bool rst_active_high;

// ---------------------------------------------------------------------------
// SPI vrstva: HW SPI (rychlý) nebo bit-bang (pomalý) na stejných pinech
// ---------------------------------------------------------------------------
static bool softSpi = false;
static unsigned int softHalfPeriodUs = 24;

static void spi_begin() {
  softSpi = (digitalRead(ISP_PIN_SLOW) == LOW);
  if (softSpi) {
    softHalfPeriodUs = (500000UL + SPI_CLOCK_SLOW - 1) / SPI_CLOCK_SLOW;
    if (softHalfPeriodUs == 0) softHalfPeriodUs = 1;
    digitalWrite(SCK, LOW);
    digitalWrite(MOSI, LOW);
    pinMode(SCK, OUTPUT);
    pinMode(MOSI, OUTPUT);
    pinMode(MISO, INPUT);
  } else {
    SPI.begin();
    SPI.beginTransaction(SPISettings(SPI_CLOCK_FAST, MSBFIRST, SPI_MODE0));
  }
}

static uint8_t spi_xfer(uint8_t b) {
  if (!softSpi) {
    return SPI.transfer(b);
  }
  for (uint8_t i = 0; i < 8; i++) {
    digitalWrite(MOSI, (b & 0x80) ? HIGH : LOW);
    digitalWrite(SCK, HIGH);
    delayMicroseconds(softHalfPeriodUs);
    b = (uint8_t)((b << 1) | (digitalRead(MISO) ? 1 : 0));
    digitalWrite(SCK, LOW);
    delayMicroseconds(softHalfPeriodUs);
  }
  return b;
}

static void spi_end() {
  if (!softSpi) {
    SPI.endTransaction();
    SPI.end();
  }
  // piny uvolnit (cíl se má rozběhnout bez rušení)
  pinMode(MOSI, INPUT);
  pinMode(SCK, INPUT);
  pinMode(MISO, INPUT);
}

static uint8_t spi_transaction(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  spi_xfer(a);
  spi_xfer(b);
  spi_xfer(c);
  return spi_xfer(d);
}

// Čekání na dokončení zápisu (flash page / EEPROM)
static void wait_write(uint16_t fixed_ms) {
#if ISP_USE_POLL_READY
  // "Poll RDY/BSY", maximálně fixed_ms
  uint32_t t0 = millis();
  while ((spi_transaction(0xF0, 0x00, 0x00, 0x00) & 0x01) &&
         (millis() - t0 < fixed_ms)) {
    // busy
  }
#else
  delay(fixed_ms);
#endif
}

// ---------------------------------------------------------------------------
// Pomocné funkce
// ---------------------------------------------------------------------------
static void put(uint8_t c) {
  SERIAL.write(c);
}

static uint8_t getch() {
  while (!SERIAL.available());
  return (uint8_t)SERIAL.read();
}

static void fill(unsigned int n) {
  for (unsigned int x = 0; x < n; x++) {
    buff[x] = getch();
  }
}

// přečte a zahodí n bajtů (aby zůstala synchronizace protokolu)
static void drain(unsigned int n) {
  while (n--) getch();
}

#define PTIME 30
static void pulse(int pin, int times) {
  do {
    led(pin, true);
    delay(PTIME);
    led(pin, false);
    delay(PTIME);
  } while (times--);
}

static void prog_lamp(bool on) {
  if (PROG_FLICKER) {
    led(LED_PMODE, on);
  }
}

static void heartbeat() {
#ifdef LED_HB
  static unsigned long last_time = 0;
  static bool state = false;
  unsigned long now = millis();
  if ((now - last_time) < 500) return;
  last_time = now;
  state = !state;
  led(LED_HB, state);
#endif
}

static void reset_target(bool reset) {
  // AVR (reset aktivní v LOW) a uvolnění přes INPUT / INPUT_PULLUP
  if (!rst_active_high && ISP_RESET_RELEASE != OUTPUT) {
    if (reset) {
      pinMode(ISP_PIN_RESET, OUTPUT);
      digitalWrite(ISP_PIN_RESET, LOW);
    } else {
      pinMode(ISP_PIN_RESET, ISP_RESET_RELEASE);
    }
    return;
  }
  // push-pull (AT89Sx nebo ISP_RESET_RELEASE == OUTPUT)
  pinMode(ISP_PIN_RESET, OUTPUT);
  digitalWrite(ISP_PIN_RESET,
               ((reset && rst_active_high) || (!reset && !rst_active_high)) ? HIGH : LOW);
}

static void empty_reply() {
  if (CRC_EOP == getch()) {
    put(STK_INSYNC);
    put(STK_OK);
  } else {
    error++;
    put(STK_NOSYNC);
  }
}

static void breply(uint8_t b) {
  if (CRC_EOP == getch()) {
    put(STK_INSYNC);
    put(b);
    put(STK_OK);
  } else {
    error++;
    put(STK_NOSYNC);
  }
}

static void get_version(uint8_t c) {
  switch (c) {
    case 0x80: breply(HWVER); break;
    case 0x81: breply(SWMAJ); break;
    case 0x82: breply(SWMIN); break;
    case 0x93: breply('S');   break;  // serial programmer
    default:   breply(0);
  }
}

static void set_parameters() {
  param.devicecode = buff[0];
  param.revision   = buff[1];
  param.progtype   = buff[2];
  param.parmode    = buff[3];
  param.polling    = buff[4];
  param.selftimed  = buff[5];
  param.lockbytes  = buff[6];
  param.fusebytes  = buff[7];
  param.flashpoll  = buff[8];
  // buff[9] ignorujeme (= buff[8])
  param.eeprompoll = beget16(&buff[10]);
  param.pagesize   = beget16(&buff[12]);
  param.eepromsize = beget16(&buff[14]);

  param.flashsize = ((uint32_t)buff[16] << 24)
                  | ((uint32_t)buff[17] << 16)
                  | ((uint32_t)buff[18] << 8)
                  |  (uint32_t)buff[19];

  // AVR mají reset aktivní v LOW, AT89Sx v HIGH
  rst_active_high = (param.devicecode >= 0xe0);
}

static void start_pmode() {
  // RESET nastavit dřív, než začneme řídit SCK/MOSI
  reset_target(true);
  spi_begin();

  // viz datasheet AVR, "Serial Programming Algorithm":
  // puls na RESET při SCK = LOW
  delay(20);
  reset_target(false);
  delayMicroseconds(100);
  reset_target(true);

  delay(50);  // datasheet: > 20 ms
  spi_transaction(0xAC, 0x53, 0x00, 0x00);  // Programming Enable
  pmode = 1;
}

static void end_pmode() {
  spi_end();
  reset_target(false);
  // push-pull režim nechat po skončení v hi-Z; u open-drain zůstává zvolené uvolnění
  if (rst_active_high || ISP_RESET_RELEASE == OUTPUT) {
    pinMode(ISP_PIN_RESET, INPUT);
  }
  pmode = 0;
}

static void universal() {
  fill(4);
  uint8_t ch = spi_transaction(buff[0], buff[1], buff[2], buff[3]);
  if (buff[0] == 0xAC && (buff[1] & 0xE0) == 0x80) {
    // Chip Erase (0xAC 0x80) nebo Page Erase (0xAC 0x83) - počkat na dokončení
    delay(ISP_CHIP_ERASE_DELAY_MS);
  }
  breply(ch);
}

// ---------------------------------------------------------------------------
// FLASH
// ---------------------------------------------------------------------------
static void flash(uint8_t hilo, unsigned int addr, uint8_t data) {
  spi_transaction(0x40 + 8 * hilo, (addr >> 8) & 0xFF, addr & 0xFF, data);
}

static void commit(unsigned int addr) {
  prog_lamp(false);
  spi_transaction(0x4C, (addr >> 8) & 0xFF, addr & 0xFF, 0);
  wait_write(ISP_FLASH_WRITE_DELAY_MS);
  prog_lamp(true);
}

// začátek aktuální stránky ve slovech (pagesize je v bajtech)
static unsigned int current_page() {
  unsigned int words = param.pagesize / 2;
  if (words >= 2 && (words & (words - 1)) == 0) {
    return here & ~(words - 1);
  }
  return here;
}

static uint8_t write_flash_pages(int length) {
  int x = 0;
  unsigned int page = current_page();
  while (x < length) {
    if (page != current_page()) {
      commit(page);
      page = current_page();
    }
    flash(LOW,  here, buff[x++]);
    flash(HIGH, here, buff[x++]);
    here++;
  }
  commit(page);
  return STK_OK;
}

static void write_flash(unsigned int length) {
  if (length > sizeof(buff)) {
    drain(length);
    getch();                       // CRC_EOP
    error++;
    put(STK_INSYNC);
    put(STK_FAILED);
    return;
  }
  fill(length);
  if (CRC_EOP == getch()) {
    put(STK_INSYNC);
    put(write_flash_pages(length));
  } else {
    error++;
    put(STK_NOSYNC);
  }
}

static uint8_t flash_read(uint8_t hilo, unsigned int addr) {
  return spi_transaction(0x20 + hilo * 8, (addr >> 8) & 0xFF, addr & 0xFF, 0);
}

static uint8_t flash_read_page(unsigned int length) {
  uint8_t out[64];
  unsigned int n = 0;
  for (unsigned int x = 0; x < length; x += 2) {
    out[n++] = flash_read(LOW,  here);
    out[n++] = flash_read(HIGH, here);
    here++;
    if (n >= sizeof(out)) {
      SERIAL.write(out, n);
      n = 0;
    }
  }
  if (n) SERIAL.write(out, n);
  return STK_OK;
}

// ---------------------------------------------------------------------------
// EEPROM
// ---------------------------------------------------------------------------
// 'here' z příkazu 'U' -> bajtová adresa v EEPROM
static inline unsigned int ee_byte_addr() {
#if EEPROM_ADDR_IS_WORD
  return here * 2;
#else
  return here;
#endif
}

// posun 'here' po přenesení 'length' bajtů
static inline void ee_advance(unsigned int length) {
#if EEPROM_ADDR_IS_WORD
  here += (length + 1) / 2;
#else
  here += length;
#endif
}

static uint8_t write_eeprom(unsigned int length) {
  // Data MUSÍME vždy načíst, jinak se rozsype synchronizace protokolu
  if (length > sizeof(buff)) {
    drain(length);
    error++;
    return STK_FAILED;
  }
  fill(length);

  if (param.eepromsize && length > param.eepromsize) {
    error++;
    return STK_FAILED;
  }

  unsigned int addr = ee_byte_addr();
  prog_lamp(false);
  for (unsigned int x = 0; x < length; x++, addr++) {
    spi_transaction(0xC0, (addr >> 8) & 0xFF, addr & 0xFF, buff[x]);
    wait_write(ISP_EEPROM_WRITE_DELAY_MS);
  }
  prog_lamp(true);
  ee_advance(length);
  return STK_OK;
}

static uint8_t eeprom_read_page(unsigned int length) {
  uint8_t out[64];
  unsigned int n = 0;
  unsigned int addr = ee_byte_addr();
  for (unsigned int x = 0; x < length; x++, addr++) {
    out[n++] = spi_transaction(0xA0, (addr >> 8) & 0xFF, addr & 0xFF, 0xFF);
    if (n >= sizeof(out)) {
      SERIAL.write(out, n);
      n = 0;
    }
  }
  if (n) SERIAL.write(out, n);
  ee_advance(length);
  return STK_OK;
}

// ---------------------------------------------------------------------------
// Stránkový zápis / čtení
// ---------------------------------------------------------------------------
static void program_page() {
  uint8_t result = STK_FAILED;
  unsigned int length = 256 * getch();
  length += getch();
  char memtype = getch();

  if (memtype == 'F') {
    write_flash(length);
    return;
  }
  if (memtype == 'E') {
    result = write_eeprom(length);
    if (CRC_EOP == getch()) {
      put(STK_INSYNC);
      put(result);
    } else {
      error++;
      put(STK_NOSYNC);
    }
    return;
  }
  put(STK_FAILED);
}

static void read_page() {
  uint8_t result = STK_FAILED;
  unsigned int length = 256 * getch();
  length += getch();
  char memtype = getch();
  if (CRC_EOP != getch()) {
    error++;
    put(STK_NOSYNC);
    return;
  }
  put(STK_INSYNC);
  if (memtype == 'F') result = flash_read_page(length);
  if (memtype == 'E') result = eeprom_read_page(length);
  put(result);
}

static void read_signature() {
  if (CRC_EOP != getch()) {
    error++;
    put(STK_NOSYNC);
    return;
  }
  put(STK_INSYNC);
  put(spi_transaction(0x30, 0x00, 0x00, 0x00));
  put(spi_transaction(0x30, 0x00, 0x01, 0x00));
  put(spi_transaction(0x30, 0x00, 0x02, 0x00));
  put(STK_OK);
}

// ---------------------------------------------------------------------------
// Hlavní dispečer STK500
// ---------------------------------------------------------------------------
static void avrisp() {
  uint8_t ch = getch();
  switch (ch) {
    case '0':  // signon
      error = 0;
      empty_reply();
      break;
    case '1':
      if (getch() == CRC_EOP) {
        put(STK_INSYNC);
        SERIAL.print("AVR ISP");
        put(STK_OK);
      } else {
        error++;
        put(STK_NOSYNC);
      }
      break;
    case 'A':
      get_version(getch());
      break;
    case 'B':
      fill(20);
      set_parameters();
      empty_reply();
      break;
    case 'E':  // extended parameters - ignorujeme
      fill(5);
      empty_reply();
      break;
    case 'P':
      if (!pmode) start_pmode();
      empty_reply();
      break;
    case 'U':  // nastavení adresy
      here = getch();
      here += 256 * getch();
      empty_reply();
      break;
    case 0x60:  // STK_PROG_FLASH
      getch();
      getch();
      empty_reply();
      break;
    case 0x61:  // STK_PROG_DATA
      getch();
      empty_reply();
      break;
    case 0x64:  // STK_PROG_PAGE
      program_page();
      break;
    case 0x74:  // STK_READ_PAGE
      read_page();
      break;
    case 'V':
      universal();
      break;
    case 'Q':
      error = 0;
      end_pmode();
      empty_reply();
      break;
    case 0x75:  // STK_READ_SIGN
      read_signature();
      break;
    case CRC_EOP:
      error++;
      put(STK_NOSYNC);
      break;
    default:
      error++;
      if (CRC_EOP == getch()) put(STK_UNKNOWN);
      else                    put(STK_NOSYNC);
  }
}

// ---------------------------------------------------------------------------
void setup() {
  SERIAL.begin(BAUDRATE);

  pinMode(ISP_PIN_SLOW, INPUT_PULLUP);

  pinMode(LED_PMODE, OUTPUT);
  pulse(LED_PMODE, 2);
#ifdef LED_ERR
  pinMode(LED_ERR, OUTPUT);
  pulse(LED_ERR, 2);
#endif
#ifdef LED_HB
  pinMode(LED_HB, OUTPUT);
  pulse(LED_HB, 2);
#endif
}

void loop() {
  led(LED_PMODE, pmode);
#ifdef LED_ERR
  led(LED_ERR, error);
#endif
  heartbeat();

  if (SERIAL.available()) {
    avrisp();
  }
}
