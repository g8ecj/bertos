//---------------------------------------------------------------------------
// Copyright (C) 2013 Robin Gilks
//
//
//  WxRx.c   -   This program collects weather data from RF sensors and sends it to a PC via USB/serial link
//
//  History:   1.0 - First release. 
//
//    This program is free software; you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation; either version 2 of the License.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program; if not, write to the Free Software
//    Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

// include files

#include <cfg/debug.h>

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

#include <avr/eeprom.h>

#include <drv/timer.h>
#include <drv/ser.h>


// Comment out for a normal build
// Uncomment for a debug build
//#define DEBUG

#define INPUT_CAPTURE_IS_RISING_EDGE()    ((TCCR1B & _BV(ICES1)) != 0)
#define INPUT_CAPTURE_IS_FALLING_EDGE()   ((TCCR1B & _BV(ICES1)) == 0)
#define SET_INPUT_CAPTURE_RISING_EDGE()   (TCCR1B |=  _BV(ICES1))
#define SET_INPUT_CAPTURE_FALLING_EDGE()  (TCCR1B &= ~_BV(ICES1))
#define GREEN_TESTLED_ON()          ((PORTD &= ~(1<<PORTB6)))
#define GREEN_TESTLED_OFF()         ((PORTD |=  (1<<PORTB6)))
#define RED_TESTLED_ON()            ((PORTD &= ~(1<<PORTD7)))
#define RED_TESTLED_OFF()           ((PORTD |=  (1<<PORTD7)))


/* serial port communication (via USB) */
#define BAUD_RATE 115200

#define PACKET_SIZE 11   /* number of nibbles in packet (after inital byte) */
#define PACKET_START 0x09	/* byte to match for start of packet */

// 0.6 ms high is a one
#define MIN_ONE 135		// minimum length of '1'
#define MAX_ONE 165		// maximum length of '1'
// 1.2 ms high is a zero
#define MIN_ZERO 270		// minimum length of '0'
#define MAX_ZERO 330		// maximum length of '0'
// 1.2 ms between bits
#define MIN_WAIT 270		// minimum interval since end of last bit
#define MAX_WAIT 330		// maximum interval since end of last bit


unsigned int CapturedTime;
unsigned int PreviousCapturedTime;
unsigned int CapturedPeriod;
unsigned int PreviousCapturedPeriod;
unsigned int SinceLastBit;
unsigned int LastBitTime;
unsigned int BitCount;
float tempC;              /* temperature in deg C */
float speedW;             /* wind speed in km/h */
float windC;              /* wind chill */
float dp;                 /* dewpoint (deg C) */
uint8_t dirn;             /* wind direction in 22.5degree steps */
uint8_t rh = 50;          /* relative humidity */
uint8_t DataPacket[PACKET_SIZE];	  /* actively loading packet */
uint8_t FinishedPacket[PACKET_SIZE]; /* fully read packet */
uint8_t PacketBitCounter;
bool ReadingPacket;
bool PacketDone;

uint8_t seconds = 0, minutes = 0, hours = 0;
int16_t rain_mins[60];
int16_t rain_hours[24];
int16_t EEMEM eeRain;
int16_t gRain;
int16_t rain1h;           /* rain in last hour */
int16_t rain24h;          /* rain in last day */

#define RAINCONVERT(r) ((float)r * 0.51826)

const unsigned char DIRN[16][4] = {
  " N ",
  "NNE",
  " NE",
  "ENE",
  " E ",
  "ESE",
  " SE",
  "SSE",
  " S ",
  "SSW",
  " SW",
  "WSW",
  " W ",
  "WNW",
  " NW",
  "NNW"
};


uint8_t CapturedPeriodWasHigh;
uint8_t PreviousCapturedPeriodWasHigh;
uint8_t mask;		    /* temporary mask uint8_t */
uint8_t CompByte;		    /* byte containing the last 8 bits read */



// does nothing now
ISR( TIMER1_OVF_vect )
{
  //increment the 32 bit timestamp counter (see overflow notes above)
  //overflow is allowed as this timestamp is most likely to be used as a delta from the previous timestamp,
  //so if it's used externally in the same 32 bit unsigned type it will come out ok.
 GREEN_TESTLED_OFF();
}

ISR( TIMER1_CAPT_vect )
{
  // Immediately grab the current capture time in case it triggers again and
  // overwrites ICR1 with an unexpected new value
  CapturedTime = ICR1;

  // GREEN test led on (flicker for debug)
  GREEN_TESTLED_ON();
  if( INPUT_CAPTURE_IS_RISING_EDGE() )
  {
    SET_INPUT_CAPTURE_FALLING_EDGE();      //previous period was low and just transitioned high
    CapturedPeriodWasHigh = false;    //uiICP_CapturedPeriod about to be stored will be a low period
  } else {
    SET_INPUT_CAPTURE_RISING_EDGE();       //previous period was high and transitioned low
    CapturedPeriodWasHigh = true;     //uiICP_CapturedPeriod about to be stored will be a high period
  }

  CapturedPeriod = (CapturedTime - PreviousCapturedTime);

  if ((CapturedPeriod > MIN_ONE) && (CapturedPeriodWasHigh == true)) { // possible bit
    /* time from end of last bit to beginning of this one */
    SinceLastBit = (PreviousCapturedTime - LastBitTime);

    if ((CapturedPeriod < MAX_ONE) && (SinceLastBit > MIN_WAIT)) {
      if (SinceLastBit > MAX_WAIT) { // too long since last bit read
        if ((SinceLastBit > (2*MIN_WAIT+MIN_ONE)) && (SinceLastBit < (2*MAX_WAIT+MAX_ONE))) { /* missed a one */
#ifdef DEBUG
          kprintf("missed one\n");
#endif
        } else {
          if ((SinceLastBit > (2*MIN_WAIT+MIN_ZERO)) && (SinceLastBit < (2*MAX_WAIT+MAX_ZERO))) { /* missed a zero */
#ifdef DEBUG
            kprintf("missed zero\n");
#endif
          }
        }
        RED_TESTLED_OFF();
        if (ReadingPacket) {
#ifdef DEBUG
          kprintf("dropped packet. bits read: %d\n", PacketBitCounter);
#endif
          ReadingPacket=0;
          PacketBitCounter=0;
        }
        CompByte=0xFF;			  /* reset comparison byte */
      } else { /* call it a one */
        if (ReadingPacket) {	/* record the bit as a one */
//          kprintf("1");
          mask = (1 << (3 - (PacketBitCounter & 0x03)));
          DataPacket[(PacketBitCounter >> 2)] |= mask;
          PacketBitCounter++;
        } else {		  /* still looking for valid packet data */
          if (CompByte != 0xFF) {	/* don't bother recording if no zeros recently */
            CompByte = ((CompByte << 1) | 0x01); /* push one on the end */
          }
        }
        LastBitTime = CapturedTime;
      }
    } else {			/* Check whether it's a zero */
      if ((CapturedPeriod > MIN_ZERO) && (CapturedPeriod < MAX_ZERO)) {
        if (ReadingPacket) {	/* record the bit as a zero */
//         kprintf("0");
          mask = (1 << (3 - (PacketBitCounter & 0x03)));
          DataPacket[(PacketBitCounter >> 2)] &= ~mask;
          PacketBitCounter++;
        } else {		      /* still looking for valid packet data */
         CompByte = (CompByte << 1); /* push zero on the end */
/* 	  if ((CompByte & 0xF0) != 0xf0) { */
/* 	    kprintf("%2x "CompByte); */
/* 	  } */
        }
        LastBitTime = CapturedTime;
      }
    }
  }

  if (ReadingPacket) {
    if (PacketBitCounter == (4*PACKET_SIZE)) { /* done reading packet */
      memcpy(&FinishedPacket,&DataPacket,PACKET_SIZE);
      RED_TESTLED_OFF();
      PacketDone = 1;
      ReadingPacket = 0;
      PacketBitCounter = 0;
    }
  } else {
    /* Check whether we have the start of a data packet */
    if (CompByte == PACKET_START) {
//      kprintf("Got packet start!");
      CompByte=0xFF;		/* reset comparison byte */
      RED_TESTLED_ON();
      /* set a flag and start recording data */
      ReadingPacket = 1;
    }
  }

  //save the current capture data as previous so it can be used for period calculation again next time around
  PreviousCapturedTime           = CapturedTime;
  PreviousCapturedPeriod         = CapturedPeriod;
  PreviousCapturedPeriodWasHigh   = CapturedPeriodWasHigh;
 
  //GREEN test led off (flicker for debug)
  GREEN_TESTLED_OFF();
}




static float dewpoint(float T, float h) {
  float td;
  // Simplified dewpoint formula from Lawrence (2005), doi:10.1175/BAMS-86-2-225
  td = T - (100-h)*pow(((T+273.15)/300),2)/5 - 0.00135*pow(h-84,2) + 0.35;
  return td;
}

static float windchill(float temp, float wind)
{
  float wind_chill;
  float wind2 = pow(wind, 0.16);

  wind_chill = 13.12 + (0.6215 * temp) - (11.37 * wind2) + (0.3965 * temp * wind2);
  wind_chill = (wind <= 4.8) ? temp : wind_chill;
  wind_chill = (temp > 10) ? temp : wind_chill;
  return wind_chill;

}

static void update_rain(void)
{
  rain_mins[minutes] = gRain;
  rain_hours[hours] = gRain;
  rain1h = gRain - rain_mins[(minutes+1) % 60];
  rain24h = gRain - rain_hours[(hours+1) % 24];

}

// a mechanism to look after time!!
static void ticker (void)
{
  static ticks_t LastTicks = 0;
  int32_t diff;


  // find out how far off the exact number of ticks we are
  diff = timer_clock () - LastTicks - ms_to_ticks (1000);
  if (diff < 0)
    return;

  // add in the number of ticks we drifted by
  LastTicks = timer_clock () - diff;

  seconds++;            // increment second
  if (seconds > 59)
  {
    update_rain();       // recalculate 1 and 24 hour rain totals every minute
    seconds = 0;

    minutes++;
    if (minutes > 59)
    {
      minutes = 0;

      hours++;
      if (hours > 23)
        hours = 0;
    }
  }
}




static void init(void) 
{
  uint8_t j;

  kdbg_init();
  kprintf( "%s", "La Crosse weather station simulator\n" );

  timer_init();

  // get the last rainfall value
  eeprom_read_block ((void *) &gRain, (const void *) &eeRain, sizeof (gRain));
  for (j = 0; j < 60; j++)
    rain_mins[j] = gRain;
  for (j = 0; j < 24; j++)
    rain_hours[j] = gRain;
  update_rain();
  cli();

  for (j = 0; j < PACKET_SIZE; j++)
    DataPacket[j] = 0;

  DDRB = 0x2F;   // B00101111
  DDRB  &= ~(1<<DDB0);    //PBO(ICP1) input
  PORTB &= ~(1<<PORTB0);  //ensure pullup resistor is also disabled

  //PORTD6 and PORTD7, GREEN and RED test LED setup
  DDRD  |=  0B11000000;      //(1<<PORTD6);   //DDRD  |=  (1<<PORTD7); (example of B prefix)
  GREEN_TESTLED_OFF();      //GREEN test led off
//  RED_TESTLED_ON();         //RED test led on
  // Set up timer1 for RF signal detection
  TCCR1A = 0B00000000;   //Normal mode of operation, TOP = 0xFFFF, TOV1 Flag Set on MAX
  TCCR1B = ( _BV(ICNC1) | _BV(CS11) | _BV(CS10) );
  SET_INPUT_CAPTURE_RISING_EDGE();
  //Timer1 Input Capture Interrupt Enable, Overflow Interrupt Enable  
  TIMSK1 = ( _BV(ICIE1) | _BV(TOIE1) );
  sei();
  /* Enable all the interrupts */
  IRQ_ENABLE;

}



// parse a raw data string
static void ParsePacket(uint8_t *Packet) {

  uint8_t chksum, j;
  int16_t rain;
  static uint8_t collectedData = 0;

  #ifdef DEBUG
  kprintf("RAW: ");
  for (j=0; j<PACKET_SIZE; j++) {
    kprintf("%01x", Packet[j]);
  }	
  kprintf("\n");
  #endif

  chksum = PACKET_START;
  for (j=0; j<PACKET_SIZE-1; j++) {
    chksum += Packet[j];
  }

  if ((chksum&0xf) == (Packet[PACKET_SIZE-1]&0xf)) { /* checksum pass */
    /* make sure that most significant digits repeat inverted */
    if (((Packet[5]&0xf)==(~Packet[8]&0xf)) && ((Packet[6]&0xf)==(~Packet[9]&0xf))) {
      switch (Packet[0] & 0x3) {
      case 0:              /* temperature packet */
         tempC=((float)(Packet[5]*100 + Packet[6]*10 + Packet[7]) - 300)/10;
         dp=dewpoint(tempC,rh);
         collectedData |= 1;
         break;
      case 1:            /* humidity packet */
         rh=(Packet[5]*10 + Packet[6]);
         collectedData |= 2;
         break;
      case 2:        /* rain */
         rain=Packet[5]*256 + Packet[6]*16 + Packet[7];
         // if rain has changed then record it
         if (rain != gRain)
         {
           gRain = rain;
           eeprom_write_block ((const void *) &gRain, (void *) &eeRain, sizeof (gRain));
           update_rain();
         }
         collectedData |= 4;
         break;
      case 3:        /* wind */
         speedW=(Packet[5]*16 + Packet[6]) * 0.36;
         dirn=Packet[7] & 0xf;
         windC=windchill(tempC, speedW);
         collectedData |= 8;
         break;
      }
      if (collectedData == 15)
      {
         kprintf("To:%2.1f WC:%2.1f DP:%2.1f Rtot:%4.1f R1h:%2.1f R24h:%3.1f RHo:%d WS:%3.1f DIR0:%3.1f DIR1:%s\n", tempC, windC, dp, RAINCONVERT(gRain), RAINCONVERT(rain1h), RAINCONVERT(rain24h), rh, speedW, (float)dirn * 22.5, (const char*)DIRN[(int)dirn]);
         collectedData = 0;
      }
    } else {
      #ifdef DEBUG
      kprintf("Fail secondary data check\n");
      #endif
    }
  } else {                  /* checksum fail */
    #ifdef DEBUG
    kprintf("chksum = %2x, data chksum %2x\n", chksum & 0x0f, Packet[PACKET_SIZE-1]);
    #endif
  }
}


// in the main loop, just hang around waiting to see whether the interrupt routine has gathered a full packet yet
int main (void)
{
  init();

  while (1)
  {
    ticker();
    timer_delay(2);                  // wait for a short time
    if (PacketDone) {	     // have a bit string that's ended
      ParsePacket(FinishedPacket);
      PacketDone=0;
    }
  }
}
