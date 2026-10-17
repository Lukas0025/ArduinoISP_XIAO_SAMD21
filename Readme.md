# ArduinoISP pro Seeed XIAO SAMD21

Změní **Seeed XIAO SAMD21** na programátor AVR čipů (ATtiny, ATmega, …) přes SPI/ISP. Komunikuje s PC přes nativní USB (protokol STK500v1), takže funguje s `avrdude` i s Arduino IDE.

---

## 1. Zapojení pinů

### 1.1 Tabulka

| XIAO pin | MCU pin | Funkce | Směr | Připojit na cíli |
|---|---|---|---|---|
| **D8** | PA07 | SCK (HW SPI) | XIAO → cíl | SCK |
| **D9** | PA05 | MISO (HW SPI) | cíl → XIAO | MISO |
| **D10** | PA06 | MOSI (HW SPI) | XIAO → cíl | MOSI |
| **D4** | PA08 | RESET cíle | XIAO → cíl | RESET |
| **D2** | PA10 | Jumper „SLOW“ (volitelný) | vstup, pull-up | GND = pomalý režim |
| **3V3** | – | Napájení cíle | XIAO → cíl | VCC |
| **GND** | – | Zem | – | GND |

Piny D8/D9/D10 jsou pevně dané hardwarovou SPI sběrnicí XIAO. Piny D4 a D2 lze změnit v sekci *Konfigurace* na začátku sketche (`ISP_PIN_RESET`, `ISP_PIN_SLOW`).

### 1.2 Schéma

```
        Seeed XIAO SAMD21                 Cílový AVR
       ┌───────────────────┐
       │ D8  (SCK)  ───────┼────────────►  SCK
       │ D9  (MISO) ◄──────┼─────────────  MISO
       │ D10 (MOSI) ───────┼────────────►  MOSI
       │ D4  (RESET) ──────┼────────────►  RESET   (pull-up dělá XIAO / AVR sám)
       │ 3V3 ──────────────┼────────────►  VCC
       │ GND ──────────────┼────────────►  GND
       │                   │
       │ D2 ──[jumper]── GND   (volitelně: pomalý režim pro cíle na 128 kHz)
       │ USB-C ◄──────────────► PC
       └───────────────────┘
```

### 1.3 Zapojení na 6pinovou ISP/ICSP hlavičku (pohled shora na konektor)

```
   MISO  1 ● ● 2  VCC
   SCK   3 ● ● 4  MOSI
   RESET 5 ● ● 6  GND
```

| ISP pin | Signál | XIAO |
|---|---|---|
| 1 | MISO | D9 |
| 2 | VCC | 3V3 |
| 3 | SCK | D8 |
| 4 | MOSI | D10 |
| 5 | RESET | D4 |
| 6 | GND | GND |

### 1.4 Indikační LED (jsou přímo na XIAO, nic nepřipojuješ)

| LED | Význam |
|---|---|
| žlutá (`LED_BUILTIN`) | svítí = programovací režim; při zápisu krátce zhasíná |
| modrá RX | chyba protokolu (svítí, dokud ji nevymaže nový příkaz `0`) |
| modrá TX | heartbeat – bliká 1 Hz, program běží |

Při startu všechny tři LED třikrát bliknou.

---

## 2. ⚠️ Napájení a logické úrovně

**XIAO SAMD21 pracuje na 3,3 V a vstupy NEJSOU 5V tolerantní.**

- **Doporučeno:** cílový AVR napájej **3,3 V** (ATmega328P/ATtiny85 při 3,3 V s interním oscilátorem 1–8 MHz fungují bez problému).
- **Cíl na 5 V:** je nutný převodník úrovní, minimálně na lince **MISO** (cíl → XIAO), např. dělič 1 kΩ / 2 kΩ nebo převodník 74LVC/BSS138. Navíc AVR na 5 V může pulz RESET ve 3,3 V nevyhodnotit jako HIGH; spolehlivější je převodník na všech čtyřech signálech.
- **Napájení z pinu 3V3:** stačí jen pro malou zátěž (holý čip, pár LED). Větší desky napájej samostatně a **spoj GND**.
- **RESET a pull-up:** XIAO RESET jen stahuje do GND a při uvolnění ho pustí (open-drain styl). Vnější rezistor není nutný: AVR má na RESET vlastní interní pull-up (~30–60 kΩ) a XIAO ve výchozím nastavení zapíná ještě svůj (`ISP_RESET_RELEASE = INPUT_PULLUP`).
  - **Cíl na 3,3 V:** ponech `INPUT_PULLUP`.
  - **Cíl na 5 V:** nastav `#define ISP_RESET_RELEASE INPUT`. Pull-up XIAO jde do 3,3 V a spolu s pull-upem cíle by držel RESET kolem 4,2 V, tedy pod prahem 0,9·VCC, a cíl by zůstal v resetu.
  - Při dlouhých kabelech nebo ruchu stále pomůže vnější 10 kΩ na VCC cíle.
  - **Nedávej na RESET velký kondenzátor** (např. 100 nF z auto-resetu Arduino desek), ruší pulz.

---

## 4. Použití

### 4.1 avrdude

```bash
# test: přečtení signatury
avrdude -c stk500v1 -p m328p -P COM5 -b 19200

# nahrání firmwaru
avrdude -c stk500v1 -p m328p -P COM5 -b 19200 -U flash:w:firmware.hex:i

# čtení EEPROM
avrdude -c stk500v1 -p m328p -P COM5 -b 19200 -U eeprom:r:eeprom_dump.hex:i

# zápis EEPROM (+ automatická verifikace)
avrdude -c stk500v1 -p m328p -P COM5 -b 19200 -U eeprom:w:eeprom.hex:i

# fuses (příklad: ATmega328P, 8 MHz interní)
avrdude -c stk500v1 -p m328p -P COM5 -b 19200 -U lfuse:w:0xE2:m
```

`COM5` nahraď svým portem. Pro ATtiny85 použij `-p t85`.

### 4.2 Arduino IDE

*Tools → Programmer → **Arduino as ISP*** (ne „ArduinoISP.org“), pak *Tools → Burn Bootloader* nebo *Sketch → Upload Using Programmer*. Port vyber ten od XIAO.

### 4.3 Platformio

```
; PlatformIO Project Configuration File
;
;   Build options: build flags, source filter
;   Upload options: custom upload port, speed and extra flags
;   Library options: dependencies, extra library storages
;   Advanced options: extra scripting
;
; Please visit documentation for the other options and examples
; https://docs.platformio.org/page/projectconf.html

[env:attiny84]
platform = atmelavr
board = attiny84
framework = arduino
upload_protocol = stk500v1
upload_flags = 
	-P$UPLOAD_PORT
	-b19200
upload_port = /dev/ttyACM0
```

### 4.3 Rychlost SPI – jumper na D2

| Jumper D2–GND | Režim | SCK | Pro koho |
|---|---|---|---|
| **rozpojeno** (výchozí) | hardwarové SPI | ~100 kHz | cíl běží na ≥ 1 MHz (výchozí nastavení většiny čerstvých ATmega/ATtiny) |
| **spojeno s GND** | softwarové (bit-bang) SPI | ~21 kHz | cíl běží na 128 kHz (interní RC „128 kHz“ oscilátor, špatně nastavené fuses) |

Pravidlo AVR: `SCK < f_cpu / 4`. Hardwarové SPI na SAMD21 neumí méně než ~94 kHz, proto je pro pomalé cíle potřeba softwarový režim. Mód se vybírá při každém vstupu do programovacího režimu, jumper tedy můžeš přepnout i bez restartu XIAO (před spuštěním avrdude).

---

## 5. Co bylo změněno oproti původnímu kódu

Jedná se o port https://github.com/Lukas0025/ESP8266-as-ISP.git

### 5.1 Oprava čtení / zápisu EEPROM

**Příčina chyby:** původní kód předpokládal, že adresa z příkazu `U` je vždy *slovní* (word), a proto ji pro EEPROM násobil dvěma (`here * 2`). Zdrojové kódy avrdude (`stk500.c`, funkce `set_memchr_a_div`) ale ukazují, že pro EEPROM s programátorem `stk500v1` posílá **bajtovou adresu**. Výsledek: první blok (adresa 0) se četl správně, všechny další z dvojnásobné adresy, takže data byla posunutá a verifikace selhávala.

**Oprava:**
- adresa EEPROM se už nenásobí dvěma (výchozí `EEPROM_ADDR_IS_WORD 0`),
- po každém čtení/zápisu se ukazatel `here` posune, takže funguje i sekvenční čtení bez nového `U`,
- při příliš dlouhém bloku se data před chybou **vždy dočtou ze sériového portu** (dřív se rozsypala synchronizace protokolu),
- místo pevného `delay(45)` na každý bajt se čeká na ukončení zápisu příkazem *Poll RDY/BSY* (maximálně 45 ms). Zápis EEPROM je několikanásobně rychlejší,
- data se do PC odesílají po blocích (`SERIAL.write(buf, n)`), ne po jednom bajtu.

> **Pokud používáš avrdude ≥ 8.0 s programátorem `-c arduino_as_isp`**, ten posílá pro EEPROM *slovní* adresu. V tom případě nastav ve sketchi
> `#define EEPROM_ADDR_IS_WORD 1`. Pro `-c stk500v1` a Arduino IDE ponech `0`.

### 5.2 Úpravy pro SAMD21 / XIAO

| Původně (ESP8266) | Nyní |
|---|---|
| `RESET D4`, ESP piny | numerické piny XIAO, přejmenováno na `ISP_PIN_RESET` (kolize názvů s CMSIS makry) |
| RESET aktivně tlačen do HIGH (push-pull) | open-drain styl: LOW = reset, uvolnění přes `INPUT_PULLUP` / `INPUT` (nastavitelné `ISP_RESET_RELEASE`) |
| SPI vždy hardwarové (`#define USE_HARDWARE_SPI`) | HW SPI 100 kHz **+** SW SPI 21 kHz přepínaný jumperem na D2 (HW SPI na SAMD21 pod ~94 kHz nejde) |
| LED vypnuté (zakomentované) | využity vestavěné LED XIAO (aktivní v LOW) |
| heartbeat přes `analogWrite` (PWM) | prosté blikání 1 Hz (nezávislé na PWM) |
| `SERIAL.print((char)x)` | `SERIAL.write(uint8_t)` |
| flash: `delay(PTIME)` po každé stránce | čekání pomocí Poll RDY/BSY |
| `current_page()` s pevnými maskami | obecný výpočet z `pagesize` |
| `write_flash` bez kontroly délky | kontrola proti přetečení `buff[256]` |
| sériový port `Serial` | nativní USB CDC (`SERIAL_PORT_USBVIRTUAL`) |

---

## 6. Řešení problémů

| Příznak | Příčina / řešení |
|---|---|
| `Yikes! Invalid device signature` / signatura `0x000000` nebo `0xFFFFFF` | Špatné zapojení (prohoď MISO/MOSI), chybí společná GND, cíl nemá napájení, nebo cíl běží na příliš pomalé frekvenci → zkus **jumper D2–GND**. |
| `not in sync: resp=0x00` / `0x30` | Špatný COM port, port je obsazený jiným programem, nebo je XIAO v bootloaderu (pulzující LED) → nahraj sketch znovu. |
| EEPROM se čte, ale verifikace selhává od 2. bloku | Zkontroluj `EEPROM_ADDR_IS_WORD` – musí odpovídat tomu, co posílá tvůj programátor (viz 5.1). |
| Zápis flash/EEPROM občas selže | Zkus pomalý režim (jumper D2). Přidej 100 nF blokovací kondenzátor na VCC cíle, kabely krať (< 20 cm). |
| Rozsvítí se modrá RX LED | Chyba protokolu; smaže se při dalším spojení (příkaz `0`). |
| Cíl po programování neběží / zůstává v resetu | Po skončení avrdude uvolní RESET i SPI piny. Zkontroluj, že RESET má pull-up. U **5V cíle** nastav `ISP_RESET_RELEASE INPUT` (viz kapitola 2). |
| Při otevření portu na 1200 baud se XIAO „ztratí“ | 1200 baud je speciální „touch“ pro vstup do bootloaderu SAMD21. Používej 19200. |

---

## 7. Technické poznámky

- Parametr `-b 19200` u avrdude je u nativního USB formální, přenos běží plnou rychlostí USB.
- SAMD21 USB CDC **neprovádí auto-reset** při otevření portu (na rozdíl od Arduino Uno), takže se XIAO při spuštění avrdude nerestartuje.
- Flash je adresován po slovech (16 bitů), EEPROM po bajtech (viz 5.1).
- Sketch podporuje cíle s flash do 64 K slov (128 KB); rozšířená adresace (`ATmega2560`) není implementována.