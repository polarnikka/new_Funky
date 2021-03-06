//--------------------------------------------------------------------------------------
// Ultra low power test for the Funkyv2; Sends an incrementing value and the VCC readout every 10 seconds
// harizanov.com
// GNU GPL V3
//--------------------------------------------------------------------------------------

 /* 
   I run this sketch with the following Atmega32u4 fuses
   low_fuses=0x7f
   high_fuses=0xd8
   extended_fuses=0xcd
   meaning:
   external crystal 8Mhz, start-up 16K CK+65ms; 
   Divide clock by 8 internally; [CKDIV8=0]  (We will start at 1Mhz since BOD level is 2.2V)
   Boot Reset vector Enabled (default address=$0000); [BOOTRST=0]
   Boot flsh size=2048K words
   Serial program downloading (SPI) enabled; [SPIEN=0]
   BOD=2.2V
*/

#include <avr/power.h>
#include <avr/sleep.h>

#include <JeeLib.h> // https://github.com/jcw/jeelib
#include "pins_arduino.h"

#define LEDpin 1

#define RETRY_PERIOD 1    // How soon to retry (in seconds) if ACK didn't come in
#define RETRY_LIMIT 5     // Maximum number of times to retry
#define ACK_TIME 15       // Number of milliseconds to wait for an ack

ISR(WDT_vect) { Sleepy::watchdogEvent(); } // interrupt handler for JeeLabs Sleepy power saving

#include <EEPROM.h>

// ID of the settings block
#define CONFIG_VERSION "mjh"
#define CONFIG_START 32

struct StoreStruct {
  // This is for mere detection if they are your settings
  char version[4];
  byte freq, network, myNodeID, ACK, sendp;
} storage = {
  CONFIG_VERSION,
  // The default values
  RF12_868MHZ, 210, 27, false, 5
};

static byte value, stack[20], top;


static byte usb;  // Are we powered via the USB? If so, do not disable it

//###############################################################
//Data Structure to be sent
//###############################################################

 typedef struct {
  	  int temp;	// Temp variable
  	  int supplyV;	// Supply voltage
 } Payload;

 Payload temptx;


void setup() {   
  // Because of the fuses, we are running @ 1Mhz now.  

  pinMode(A5,OUTPUT);  //Set RFM12B power control pin (REV 1)
  digitalWrite(A5,LOW); //Start the RFM12B
    
  pinMode(LEDpin,OUTPUT);
  digitalWrite(LEDpin,HIGH); 

  loadConfig();

  USBCON = USBCON | B00010000; 

  delay(1000);  // Wait at least between 150ms and 1000ms (necessary); Slower host like Raspberry Pi needs more time
 
  if (UDINT & B00000001){
      // USB Disconnected; We are running on battery so we must save power
      usb=0;
      powersave();
    //  clock_prescale_set(clock_div_2);   //Run at 4Mhz so we can talk to the RFM12B over SPI
  }
  else {
      // USB is connected 
      usb=1;
      //clock_prescale_set(clock_div_1);   //Make sure we run @ 8Mhz; not running on battery so go full speed
      for(int i=0;i<10;i++){
          digitalWrite(LEDpin,LOW); 
          delay(50);
          digitalWrite(LEDpin,HIGH); 
          delay(50);
      }

      Serial.begin(57600);  // Pretty much useless on USB CDC, in fact this procedure is blank. Included here so peope don't wonder where is Serial.begin
      showString(PSTR("\n[Funky v2]\n"));   
      showHelp();

      // Wait for configuration for 10 seconds, then timeout and start the sketch
      unsigned long start=millis();
    
      while((millis()-start)<10000) {
      if (Serial.available())
        {
          handleInput(Serial.read());
          start=millis();
        }
      }

      showString(PSTR("\nStarting sketch.."));   
      Serial.flush();  
    }
 
  digitalWrite(LEDpin,LOW);  
 
  rf12_initialize(storage.myNodeID,storage.freq,storage.network); // Initialize RFM12 
  // Adjust low battery voltage to 2.2V
  rf12_control(0xC000);
  
 /* 
 // Transmission power experimenting, see http://harizanov.com/2013/07/reducing-rfm12b-transmission-power/
 
 byte txPower=7;
 rf12_control(0x9850 | (txPower > 7 ? 7 : txPower)); // !mp,90kHz,MAX OUT               //last byte=power level: 0=highest, 7=lowest
 */
  
  rf12_sleep(0);                          // Put the RFM12 to sleep


  power_spi_disable();   

  //if(!usb) { Sleepy::loseSomeTime(10000); }         // Allow some time for power source to recover    


}

void loop() {
  
  digitalWrite(LEDpin,HIGH);  
  power_adc_enable();
  readVcc(); // discard first reading
  temptx.supplyV = readVcc(); // Get supply voltage  
  power_adc_disable();
  digitalWrite(LEDpin,LOW);  
  
  if (temptx.supplyV > 2400) {// Only send if enough "juice" is available i.e supply V >2.4V
    temptx.temp++;
    rfwrite(); // Send data via RF 
  }

  for(int j = 0; j < 1; j++) {    // Sleep for j minutes
    if(usb==0) 
      Sleepy::loseSomeTime(storage.sendp*1000); //JeeLabs power save function: enter low power mode for x seconds (valid range 16-65000 ms)
    else 
      delay(storage.sendp*1000);    
  }
}

//--------------------------------------------------------------------------------------------------
// Send payload data via RF
//--------------------------------------------------------------------------------------------------
static void rfwrite(){
      power_spi_enable();
    
      if(storage.ACK) {
         for (byte i = 0; i <= RETRY_LIMIT; ++i) {  // tx and wait for ack up to RETRY_LIMIT times
           rf12_sleep(-1);              // Wake up RF module
           while (!rf12_canSend())
              rf12_recvDone();
           rf12_sendStart(RF12_HDR_ACK, &temptx, sizeof temptx); 
           rf12_sendWait(2);           // Wait for RF to finish sending while in standby mode
           byte acked = waitForAck();  // Wait for ACK
           rf12_sleep(0);              // Put RF module to sleep
           if (acked) {       
             power_spi_disable();        
             return; 
           }       // Return if ACK received
       
        if(!usb) Sleepy::loseSomeTime(RETRY_PERIOD*1000); // If no ack received wait and try again
        else delay(RETRY_PERIOD*1000);       
       }
     }
     else {
  
      rf12_sleep(-1);              // Wake up RF module
      while (!rf12_canSend())
        rf12_recvDone();
      rf12_sendStart(0, &temptx, sizeof temptx); 
      rf12_sendWait(2);           // Wait for RF to finish sending while in standby mode
      rf12_sleep(0);              // Put RF module to sleep 
      power_spi_disable();      
     }
}



  static byte waitForAck() {
   MilliTimer ackTimer;
   while (!ackTimer.poll(ACK_TIME)) {
     if (rf12_recvDone() && rf12_crc == 0 &&
        rf12_hdr == (RF12_HDR_DST | RF12_HDR_CTL | storage.myNodeID))
        return 1;
     }
   return 0;
  }

  
//--------------------------------------------------------------------------------------------------
// Read current supply voltage
//--------------------------------------------------------------------------------------------------
 long readVcc() {
   long result;
   // Read 1.1V reference against Vcc
//   if(usb==0) clock_prescale_set(clock_div_1);   //Make sure we run @ 8Mhz
   ADCSRA |= bit(ADEN); 
   ADMUX = _BV(REFS0) | _BV(MUX4) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);  // For ATmega32u4
   Sleepy::loseSomeTime(2);
   ADCSRA |= _BV(ADSC); // Convert
   while (bit_is_set(ADCSRA,ADSC));
   result = ADCL;
   result |= ADCH<<8;
   result = 1126400L / result; // Back-calculate Vcc in mV
   ADCSRA &= ~ bit(ADEN); 
  // if(usb==0) clock_prescale_set(clock_div_2);     
   return result;
} 
//########################################################################################################################


void powersave() {
  ADCSRA =0;
  power_adc_disable();
  power_usart0_disable();
  //power_spi_disable();  /do that a bit later, after we power RFM12b down
  power_twi_disable();
  power_timer0_disable();
  power_timer1_disable();
  power_timer3_disable();
  PRR1 |= (uint8_t)(1 << 4);  //PRTIM4
  power_usart1_disable();
  
  // Switch to RC Clock 
  UDINT  &= ~(1 << SUSPI); // UDINT.SUSPI = 0; Usb_ack_suspend
  USBCON |= ( 1 <<FRZCLK); // USBCON.FRZCLK = 1; Usb_freeze_clock
  PLLCSR &= ~(1 << PLLE); // PLLCSR.PLLE = 0; Disable_pll

  CLKSEL0 |= (1 << RCE); // CLKSEL0.RCE = 1; Enable_RC_clock()
  while ( (CLKSTA & (1 << RCON)) == 0){}	// while (CLKSTA.RCON != 1);  while (!RC_clock_ready())
  CLKSEL0 &= ~(1 << CLKS);  // CLKSEL0.CLKS = 0; Select_RC_clock()
  CLKSEL0 &= ~(1 << EXTE);  // CLKSEL0.EXTE = 0; Disable_external_clock
   
   // Datasheet says that to power off the USB interface we have to: 
   //      Detach USB interface 
   //      Disable USB interface 
   //      Disable PLL 
   //      Disable USB pad regulator 

   // Disable the USB interface 
   USBCON &= ~(1 << USBE); 
    
   // Disable the VBUS transition enable bit 
   USBCON &= ~(1 << VBUSTE); 
    
   // Disable the VUSB pad 
   USBCON &= ~(1 << OTGPADE); 
    
   // Freeze the USB clock 
   USBCON &= ~(1 << FRZCLK); 
    
   // Disable USB pad regulator 
   UHWCON &= ~(1 << UVREGE); 
    
   // Clear the IVBUS Transition Interrupt flag 
   USBINT &= ~(1 << VBUSTI); 
    
   // Physically detact USB (by disconnecting internal pull-ups on D+ and D-) 
   UDCON |= (1 << DETACH); 
   
   power_usb_disable();  // Keep it here, after the USB power down

}

void loadConfig() {
  // To make sure there are settings, and they are ours
  // If nothing is found it will use the default settings.
  if (EEPROM.read(CONFIG_START + 0) == CONFIG_VERSION[0] &&
      EEPROM.read(CONFIG_START + 1) == CONFIG_VERSION[1] &&
      EEPROM.read(CONFIG_START + 2) == CONFIG_VERSION[2])
    for (unsigned int t=0; t<sizeof(storage); t++)
      *((char*)&storage + t) = EEPROM.read(CONFIG_START + t);
}

void saveConfig() {
  for (unsigned int t=0; t<sizeof(storage); t++)
    EEPROM.write(CONFIG_START + t, *((char*)&storage + t));
}

static void handleInput (char c) {
    if ('0' <= c && c <= '9')
        value = 10 * value + c - '0';
    else if (c == ',') {
        if (top < sizeof stack)
            stack[top++] = value;
        value = 0;
    } else if ('a' <= c && c <='z') {
        showString(PSTR("> "));
        Serial.print((int) value);
        Serial.println(c);
        switch (c) {
            default:
                showHelp();
                break;

             case 'i': // set node id
                  storage.myNodeID = value;
                  saveConfig();
                break;             
            case 'b': // set band: 4 = 433, 8 = 868, 9 = 915
                  value = value == 8 ? RF12_868MHZ :
                          value == 9 ? RF12_915MHZ : RF12_433MHZ;
                  storage.freq =value;
                  saveConfig();
                break;             
            case 'g': // set network group
                  storage.network = value;
                  saveConfig();
                break;
            case 'p': // set sending period
                  storage.sendp = value;
                  saveConfig();
                break;
            case 'a': // set ACK
                  if(value < 2){  // only 1 and 0 allowed
                    storage.ACK = value;
                    saveConfig();
                  }
                break;
                
                
                
        }
        value = top = 0;
        memset(stack, 0, sizeof stack);
    } else if (c > ' ')
        showHelp();

        rf12_initialize(storage.myNodeID,storage.freq,storage.network); // Initialize RFM12 
    
}


char helpText1[] PROGMEM = 
    "\n"
    "Available commands:" "\n"
    "  <nn> i     - set node ID (standard node ids are 1..26)" "\n"
    "  <n> b      - set MHz band (4 = 433, 8 = 868, 9 = 915)" "\n"
    "  <nnn> g    - set network group (default = 210)" "\n"
    "  <n> a      - set ACK flag (1 = request ACK, 0 = do not requst ACK - default)" "\n"
    "  <nnn> p    - set period for sending in seconds ( default = 20 seconds)" "\n"   
    "\n\n This configuration menu will timeout after 30 seconds of inactivity and sketch will start" "\n"       
    "\n"
;

static void showString (PGM_P s) {
    for (;;) {
        char c = pgm_read_byte(s++);
        if (c == 0)
            break;
        if (c == '\n')
            Serial.print('\r');
        Serial.print(c);
    }
}

static void showHelp() {
    showString(helpText1);
    showString(PSTR("\nCurrent configuration:\n"));

    showString(PSTR("NodeID: "));
    Serial.print(storage.myNodeID,DEC);
    showString(PSTR(", Group: "));
    Serial.print(storage.network,DEC);
    showString(PSTR(", Band: "));
    static word bands[4] = { 315, 433, 868, 915 };
    word band = storage.freq;
    Serial.print(bands[band],DEC);
    showString(PSTR(" MHz"));
    showString(PSTR(", ACKs: "));
    Serial.print(storage.ACK,DEC);
    showString(PSTR(", Sending every "));
    Serial.print(storage.sendp,DEC);
    showString(PSTR(" seconds\n"));
}


