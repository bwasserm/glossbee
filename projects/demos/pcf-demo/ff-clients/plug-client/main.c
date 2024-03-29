/******************************************************************************
*  Nano-RK, a real-time operating system for sensor networks.
*  Copyright (C) 2007, Real-Time and Multimedia Lab, Carnegie Mellon University
*  All rights reserved.
*
*  This is the Open Source Version of Nano-RK included as part of a Dual
*  Licensing Model. If you are unsure which license to use please refer to:
*  http://www.nanork.org/nano-RK/wiki/Licensing
*
*  This program is free software: you can redistribute it and/or modify
*  it under the terms of the GNU General Public License as published by
*  the Free Software Foundation, version 2.0 of the License.
*
*  This program is distributed in the hope that it will be useful,
*  but WITHOUT ANY WARRANTY; without even the implied warranty of
*  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*  GNU General Public License for more details.
*
*  You should have received a copy of the GNU General Public License
*  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*
*******************************************************************************/

#include <nrk.h>
#include <include.h>
#include <ulib.h>
#include <stdio.h>
#include <avr/sleep.h>
#include <hal.h>
#include <pcf_tdma.h>
#include <nrk_error.h>
#include <nrk_timer.h>
#include <nrk_ext_int.h>
#include <power_driver.h>
#include <nrk_eeprom.h>
#include <pkt.h>

// if DISABLE_BUTTON is defined then the plug meter ignores button toggle except the startup callibration check
// #define DISABLE_BUTTON	

// if SET_MAC is 0, then read MAC from EEPROM
// otherwise use the coded value
#define SET_MAC  	0x0
//#define SET_MAC  	0x1
#define RADIO_CHANNEL   26	

PKT_T	tx_pkt;
PKT_T	rx_pkt;

tdma_info tx_tdma_fd;
tdma_info rx_tdma_fd;

uint8_t rx_buf[TDMA_MAX_PKT_SIZE];
uint8_t tx_buf[TDMA_MAX_PKT_SIZE];

uint32_t mac_address;
uint16_t cal_done;
uint8_t send_ack;

nrk_task_type RX_TASK;
NRK_STK rx_task_stack[NRK_APP_STACKSIZE];
void rx_task (void);


nrk_task_type TX_TASK;
NRK_STK tx_task_stack[NRK_APP_STACKSIZE];
void tx_task (void);

void nrk_create_taskset ();

int8_t tdma_error(uint16_t);
uint8_t aes_key[] = {0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee, 0xff};

nrk_time_t button_last_press;
nrk_time_t button_cur_press;
nrk_time_t button_tmp_press;

#ifdef TWO_PIN_LED
uint8_t led_state; //0 off, 1 green, 2 red
#endif

int main ()
{
  uint16_t div;

  nrk_int_disable();
  // Configure relay port directions
  DDRE |= 0x10;
  socket_0_enable();
  // Configure led port directions
  DDRE |= 0x0c;
  DDRD |= 0x00;
  PORTD |= 0xff;
  DDRF = 0;

  socket_0_active=nrk_eeprom_read_byte(EEPROM_STATE_ADDR);
  if(socket_0_active==1)
  {
  socket_0_enable();
  plug_led_green_set();
  }else
  {
  socket_0_disable();
  plug_led_green_clr();
  }
  // If PUD value set, then we expect it wasn't a clean reboot (unexpected restart).  
  // Try to force a proper watchdog reboot
  if((MCUCR&0x10)!=0 )
  {
	//nrk_watchdog_enable();
  	nrk_int_disable();
	MCUSR &= ~(1<<WDRF);
	WDTCSR |= (1<<WDCE) | (1<<WDE);
	WDTCSR = (1<<WDE) | (1<<WDP2) | (1<<WDP0);

	// Disable interrupts to stop pending timers etc
	while(1);
  }

  nrk_setup_uart (UART_BAUDRATE_115K2);

  MCUCR |= BM(PUD);

  nrk_init ();
  nrk_time_set (0, 0);

  tdma_set_error_callback(&tdma_error);
  tdma_task_config();

  nrk_create_taskset ();
  nrk_start ();

  return 0;
}

// This function gets called in a loop if sync is lost.
// It is passed a counter indicating how long it has gone since the last synchronization.
int8_t tdma_error(uint16_t cons_error_cnt)
{

// TDMA error handler just controls the LED in this case.
// Button is still handled by interrupt
if(tdma_sync_ok()==0) plug_led_red_set();
else plug_led_red_clr();
// Return NRK_OK to force the error counter to be reset
return NRK_ERROR;
}


// This is an interrupts routine that handles the button press.
// It has a 300ms debounce
void button_handler()
{
int8_t v;

// Make sure button is depressed for at least 50 us
nrk_spin_wait_us(50);
v=nrk_gpio_get(NRK_PORTD_0);
if(v!=0) return;
nrk_time_get(&button_cur_press);
nrk_time_sub(&button_tmp_press, button_cur_press, button_last_press );
if(button_tmp_press.secs>=1 || button_tmp_press.nano_secs>=(300*NANOS_PER_MS))
{

// Reboot the node...

socket_0_disable();
power_socket_disable(0);
nrk_int_disable();
while(1);

	if(socket_0_active==1)
	{
		plug_led_green_clr();
		power_socket_disable(0);
	}
	else
	{
		plug_led_green_set();
		power_socket_enable(0);
	}
button_last_press.secs=button_cur_press.secs;
button_last_press.nano_secs=button_cur_press.nano_secs;
}

}


void rx_task ()
{
  nrk_time_t t;
  uint16_t cnt;
  int8_t v;
  uint8_t len, i;
uint8_t chan;


  cnt = 0;
  nrk_kprintf (PSTR ("Nano-RK Version "));
  printf ("%d\r\n", NRK_VERSION);


  nrk_kprintf( PSTR( "RX Task PID=" )); printf ("%u\r\n", nrk_get_pid ());
  t.secs = 5;
  t.nano_secs = 0;

  while (cal_done==0)
    nrk_wait_until_next_period ();


  chan = RADIO_CHANNEL;
  if (SET_MAC == 0x00) {

    v = read_eeprom_mac_address (&mac_address);
    if (v == NRK_OK) {
      v = read_eeprom_channel (&chan);
      v = read_eeprom_aes_key(aes_key);
    }
    else {
      while (1) {
        nrk_kprintf (PSTR
                     ("* ERROR reading MAC address, run eeprom-set utility\r\n"));
        nrk_wait_until_next_period ();
      }
    }
  }
  else
    mac_address = SET_MAC;

  printf ("MAC ADDR: %x\r\n", mac_address & 0xffff);
  printf ("chan = %d\r\n", chan);
  len=0;
  for(i=0; i<16; i++ ) { len+=aes_key[i]; }
  printf ("AES checksum = %d\r\n", len);



  tdma_init (TDMA_CLIENT, chan, (mac_address));

  tdma_aes_setkey(aes_key);
  tdma_aes_enable();

  while (!tdma_started ())
    nrk_wait_until_next_period ();

  v = tdma_tx_slot_add (mac_address&0xff);

  if (v != NRK_OK)
    nrk_kprintf (PSTR ("Could not add slot!\r\n"));

   // setup a software watch dog timer
   t.secs=30;
   t.nano_secs=0;
   nrk_sw_wdt_init(0, &t, NULL);
   nrk_sw_wdt_start(0);
  while (1) {
    // Update watchdog timer
    nrk_sw_wdt_update(0);
    v = tdma_recv (&rx_tdma_fd, &rx_buf, &len, TDMA_BLOCKING);
    if (v == NRK_OK) {
     // printf ("src: %u\r\nrssi: %d\r\n", rx_tdma_fd.src, rx_tdma_fd.rssi);
     // printf ("slot: %u\r\n", rx_tdma_fd.slot);
     // printf ("cycle len: %u\r\n", rx_tdma_fd.cycle_size);
      v=buf_to_pkt(&rx_buf, &rx_pkt);
      if(v==NRK_OK)
	{
      	if(((rx_pkt.dst_mac&0xff) == (mac_address&0xff)) || ((rx_pkt.dst_mac&0xff)==0xff)) 
     	 {
		if(rx_pkt.type==PING)
		{
	   	  send_ack=1;
                  nrk_led_clr(0);
                  nrk_led_clr(1);
        	if(rx_pkt.payload[0]==PING_1)
                {
                  nrk_led_set(0);
                  nrk_wait_until_next_period();
                  nrk_wait_until_next_period();
                  nrk_wait_until_next_period();
                  nrk_led_clr(0);
                }
        	if(rx_pkt.payload[0]==PING_2)
                {
                  nrk_led_set(1);
                  nrk_wait_until_next_period();
                  nrk_wait_until_next_period();
                  nrk_wait_until_next_period();
                  nrk_led_clr(1);
                }
        	if(rx_pkt.payload[0]==PING_PERSIST)
                {
                  nrk_led_set(0);
                }


		}

		if(rx_pkt.type==APP)
		{
		  // payload 1: Key
		  if(rx_pkt.payload[1]==2)
		    {
	  	      	send_ack=1;
			// payload 2: Outlet Number 
			// payload 3: On/Off
			if(rx_pkt.payload[3]==0) {
			power_socket_disable(rx_pkt.payload[2]);	
			plug_led_green_clr();
			//printf( "Disable %d\r\n", rx_pkt.payload[2] );
		    }
		if(rx_pkt.payload[3]==1) {
			power_socket_enable(rx_pkt.payload[2]);	
			//printf( "Enable %d\r\n", rx_pkt.payload[2] );
			plug_led_green_set();
			}


			}
		}

      	   }

	}
      /*      printf ("len: %u\r\npayload: ", len);
      for (i = 0; i < len; i++)
        printf ("%d ", rx_buf[i]);
      printf ("\r\n");

      if(rx_buf[0]==(mac_address&0xff))
      {
	if(rx_buf[2]==0) {
		power_socket_disable(rx_buf[1]);	
		printf( "Disable %d\r\n", rx_buf[1] );
	}
	if(rx_buf[2]==1) {
		power_socket_enable(rx_buf[1]);	
		printf( "Enable %d\r\n", rx_buf[1] );
	}
      }
      */

    }

    tdma_rx_pkt_release();
    //  nrk_wait_until_next_period();
  }

}

uint8_t ctr_cnt[4];

void tx_task ()
{
  uint8_t j, i, val, cnt;
  int8_t len;
  int8_t v;
  nrk_sig_t tx_done_signal;
  nrk_sig_mask_t ret;


  send_ack=0;
  cal_done=0;
  printf ("tx_task PID=%d\r\n", nrk_get_pid ());

  // Wait until the tx_task starts up bmac
  // This should be called by all tasks using bmac that


  power_init ();

#ifndef DISABLE_BUTTON
  nrk_gpio_direction(NRK_BUTTON,NRK_PIN_INPUT );
  nrk_ext_int_configure(NRK_EXT_INT_0, NRK_FALLING_EDGE, &button_handler );
  nrk_ext_int_enable(NRK_EXT_INT_0);
#endif

v_center=((uint16_t)nrk_eeprom_read_byte(EEPROM_CAL_V_MSB_ADDR))<<8 | (uint16_t)nrk_eeprom_read_byte(EEPROM_CAL_V_LSB_ADDR);
c_center=((uint16_t)nrk_eeprom_read_byte(EEPROM_CAL_C1_MSB_ADDR))<<8 | (uint16_t)nrk_eeprom_read_byte(EEPROM_CAL_C1_LSB_ADDR);
c2_center=((uint16_t)nrk_eeprom_read_byte(EEPROM_CAL_C2_MSB_ADDR))<<8 | (uint16_t)nrk_eeprom_read_byte(EEPROM_CAL_C2_LSB_ADDR);


if(((PIND & 0x1) == 0) || (v_center==0xffff) || (v_center==0x0) )
{
	// Get v_center and c_centers enough to grab calibration values
	v_center=512;
	c_center=512;
	c2_center=512;
	power_socket_enable(0);
	socket_0_disable();

	plug_led_green_clr();
	plug_led_red_clr();
	for(i=0; i<3; i++ ) {
	plug_led_green_set();
	nrk_wait_until_next_period();
	plug_led_green_clr();
	nrk_wait_until_next_period();
	}
	plug_led_green_clr();
	plug_led_red_clr();
	for(i=0; i<5; i++ )
		nrk_wait_until_next_period();

	v_center=(v_p2p_high+v_p2p_low)/2;
	c_center=(c_p2p_high+c_p2p_low)/2;
	c2_center=(c_p2p_high2+c_p2p_low2)/2;
    	nrk_eeprom_write_byte(EEPROM_CAL_V_MSB_ADDR, (uint8_t)(v_center>>8)); 
	nrk_eeprom_write_byte(EEPROM_CAL_V_LSB_ADDR, (uint8_t)v_center&0xff);
   	nrk_eeprom_write_byte(EEPROM_CAL_C1_MSB_ADDR, (uint8_t)(c_center>>8)); 
	nrk_eeprom_write_byte(EEPROM_CAL_C1_LSB_ADDR, (uint8_t)c_center&0xff);
    	nrk_eeprom_write_byte(EEPROM_CAL_C2_MSB_ADDR, (uint8_t)(c2_center>>8)); 
	nrk_eeprom_write_byte(EEPROM_CAL_C2_LSB_ADDR, (uint8_t)c2_center&0xff);
	nrk_eeprom_write_byte(EEPROM_ENERGY1_0_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY1_1_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY1_2_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY1_3_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY1_4_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY1_5_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY1_6_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY1_7_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY2_0_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY2_1_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY2_2_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY2_3_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY2_4_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY2_5_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY2_6_ADDR, 0);
	nrk_eeprom_write_byte(EEPROM_ENERGY2_7_ADDR, 0);
	//plug_led_green_set();
	//socket_0_enable();
	//power_socket_enable(0);
	power_socket_disable(0);
	socket_0_disable();
}
cummulative_energy.byte[0]=nrk_eeprom_read_byte(EEPROM_ENERGY1_0_ADDR);
cummulative_energy.byte[1]=nrk_eeprom_read_byte(EEPROM_ENERGY1_1_ADDR);
cummulative_energy.byte[2]=nrk_eeprom_read_byte(EEPROM_ENERGY1_2_ADDR);
cummulative_energy.byte[3]=nrk_eeprom_read_byte(EEPROM_ENERGY1_3_ADDR);
cummulative_energy.byte[4]=nrk_eeprom_read_byte(EEPROM_ENERGY1_4_ADDR);
cummulative_energy.byte[5]=nrk_eeprom_read_byte(EEPROM_ENERGY1_5_ADDR);
cummulative_energy.byte[6]=nrk_eeprom_read_byte(EEPROM_ENERGY1_6_ADDR);
cummulative_energy.byte[7]=nrk_eeprom_read_byte(EEPROM_ENERGY1_7_ADDR);
cummulative_energy2.byte[0]=nrk_eeprom_read_byte(EEPROM_ENERGY2_0_ADDR);
cummulative_energy2.byte[1]=nrk_eeprom_read_byte(EEPROM_ENERGY2_1_ADDR);
cummulative_energy2.byte[2]=nrk_eeprom_read_byte(EEPROM_ENERGY2_2_ADDR);
cummulative_energy2.byte[3]=nrk_eeprom_read_byte(EEPROM_ENERGY2_3_ADDR);
cummulative_energy2.byte[4]=nrk_eeprom_read_byte(EEPROM_ENERGY2_4_ADDR);
cummulative_energy2.byte[5]=nrk_eeprom_read_byte(EEPROM_ENERGY2_5_ADDR);
cummulative_energy2.byte[6]=nrk_eeprom_read_byte(EEPROM_ENERGY2_6_ADDR);
cummulative_energy2.byte[7]=nrk_eeprom_read_byte(EEPROM_ENERGY2_7_ADDR);


cal_done=1;
  while (!tdma_started ())
    nrk_wait_until_next_period ();
//nrk_kprintf( PSTR("after outlet on\r\n"));

  // Sample of using Reservations on TX packets
  // This example allows 2 packets to be sent every 5 seconds
  // r_period.secs=5;
  // r_period.nano_secs=0;
  // v=bmac_tx_reserve_set( &r_period, 2 );
  // if(v==NRK_ERROR) nrk_kprintf( PSTR("Error setting b-mac tx reservation (is NRK_MAX_RESERVES defined?)\r\n" ));





  while (1) {


    nrk_sw_wdt_update(0);  
    // For blocking transmits, use the following function call.
    // For this there is no need to register  
    tx_pkt.payload[0] = 1;  // ELEMENTS
    tx_pkt.payload[1] = 1;  // Key
    tx_pkt.payload[2] = (total_secs >> 24) & 0xff;
    tx_pkt.payload[3] = (total_secs >> 16) & 0xff;
    tx_pkt.payload[4] = (total_secs >> 8) & 0xff;
    tx_pkt.payload[5] = (total_secs) & 0xff;
    tx_pkt.payload[6] = freq & 0xff;
    tx_pkt.payload[7] = (rms_voltage >> 8) & 0xff;
    tx_pkt.payload[8] = rms_voltage & 0xff;
    tx_pkt.payload[9] = (rms_current >> 8) & 0xff;
    tx_pkt.payload[10] = rms_current & 0xff;
    tx_pkt.payload[11] = (true_power >> 16) & 0xff;
    tx_pkt.payload[12] = (true_power >> 8) & 0xff;
    tx_pkt.payload[13] = (true_power) & 0xff;
    tx_pkt.payload[14] = tmp_energy.byte[5];
    tx_pkt.payload[15] = tmp_energy.byte[4];
    tx_pkt.payload[16] = tmp_energy.byte[3];
    tx_pkt.payload[17] = tmp_energy.byte[2];
    tx_pkt.payload[18] = tmp_energy.byte[1];
    tx_pkt.payload[19] = tmp_energy.byte[0];
    tx_pkt.payload[20] = socket_0_active;
    tx_pkt.payload[21] = (rms_current2 >> 8) & 0xff;
    tx_pkt.payload[22] = rms_current2 & 0xff;
    tx_pkt.payload[23] = (true_power2 >> 16) & 0xff;
    tx_pkt.payload[24] = (true_power2 >> 8) & 0xff;
    tx_pkt.payload[25] = (true_power2) & 0xff;
    tx_pkt.payload[26] = tmp_energy2.byte[5];
    tx_pkt.payload[27] = tmp_energy2.byte[4];
    tx_pkt.payload[28] = tmp_energy2.byte[3];
    tx_pkt.payload[29] = tmp_energy2.byte[2];
    tx_pkt.payload[30] = tmp_energy2.byte[1];
    tx_pkt.payload[31] = tmp_energy2.byte[0];
    tx_pkt.payload[32] = socket_1_active;
    tx_pkt.payload[33] = l_v_p2p_low>>8;
    tx_pkt.payload[34] = l_v_p2p_low & 0xff;
    tx_pkt.payload[35] = l_v_p2p_high>>8;
    tx_pkt.payload[36] = l_v_p2p_high& 0xff;
    tx_pkt.payload[37] = c_p2p_low >>8;
    tx_pkt.payload[38] = c_p2p_low & 0xff;
    tx_pkt.payload[39] = c_p2p_high >>8;
    tx_pkt.payload[40] = c_p2p_high & 0xff;
    tx_pkt.payload[41] = c_p2p_low2 >>8;
    tx_pkt.payload[42] = c_p2p_low2 & 0xff;
    tx_pkt.payload[43] = c_p2p_high2 >>8;
    tx_pkt.payload[44] = c_p2p_high2 & 0xff;
    tx_pkt.payload[45] =  v_center >>8;
    tx_pkt.payload[46] =  v_center & 0xff;
    tx_pkt.payload[47] =  c_center >>8;
    tx_pkt.payload[48] =  c_center & 0xff;
    tx_pkt.payload[49] =  c2_center >>8;
    tx_pkt.payload[50] =  c2_center & 0xff;
    tx_pkt.payload_len=51;

    tx_pkt.src_mac=mac_address;
    tx_pkt.dst_mac=0;
    tx_pkt.type=APP;

	// if there is a pending ACK, drop the application packet
        if(send_ack==1)
        {
        tx_pkt.type=ACK;
        tx_pkt.payload_len=0;
        send_ack=0;
        }


    len=pkt_to_buf(&tx_pkt,&tx_buf );
    if(len>0)
    {
    v = tdma_send (&tx_tdma_fd, &tx_buf, len, TDMA_BLOCKING);
    if (v == NRK_OK) {
      //nrk_kprintf (PSTR ("App Packet Sent\n"));
    }
    } else nrk_wait_until_next_period();

    // Turn off red led if TDMA is running...
    if(tdma_sync_ok()==1) plug_led_red_clr();

  }

}

void nrk_create_taskset ()
{
 
  
  
  nrk_task_set_entry_function( &TX_TASK, tx_task);
  nrk_task_set_stk( &TX_TASK, tx_task_stack, NRK_APP_STACKSIZE);
  TX_TASK.prio = 2;
  TX_TASK.FirstActivation = TRUE;
  TX_TASK.Type = BASIC_TASK;
  TX_TASK.SchType = PREEMPTIVE;
  TX_TASK.period.secs = 1;
  TX_TASK.period.nano_secs = 0;
  TX_TASK.cpu_reserve.secs = 0;
  TX_TASK.cpu_reserve.nano_secs = 0;
  TX_TASK.offset.secs = 0;
  TX_TASK.offset.nano_secs = 0;
  nrk_activate_task (&TX_TASK);


  nrk_task_set_entry_function( &RX_TASK, rx_task);
  nrk_task_set_stk( &RX_TASK, rx_task_stack, NRK_APP_STACKSIZE);
  RX_TASK.prio = 2;
  RX_TASK.FirstActivation = TRUE;
  RX_TASK.Type = BASIC_TASK;
  RX_TASK.SchType = PREEMPTIVE;
  RX_TASK.period.secs = 1;
  RX_TASK.period.nano_secs = 0;
  RX_TASK.cpu_reserve.secs = 0;
  RX_TASK.cpu_reserve.nano_secs = 0;
  RX_TASK.offset.secs = 0;
  RX_TASK.offset.nano_secs = 0;
  nrk_activate_task (&RX_TASK);



  nrk_task_set_entry_function( &event_detector, event_detector_task);
  nrk_task_set_stk( &event_detector, event_detector_stack, EVENT_DETECTOR_STACKSIZE);
  event_detector.prio = 1;
  event_detector.FirstActivation = TRUE;
  event_detector.Type = BASIC_TASK;
  event_detector.SchType = PREEMPTIVE;
  event_detector.period.secs = 1;
  event_detector.period.nano_secs = 0*NANOS_PER_MS;
  event_detector.cpu_reserve.secs = 0;
  event_detector.cpu_reserve.nano_secs = 500*NANOS_PER_MS;
  event_detector.offset.secs = 0;
  event_detector.offset.nano_secs= 0;
  nrk_activate_task (&event_detector);


}
