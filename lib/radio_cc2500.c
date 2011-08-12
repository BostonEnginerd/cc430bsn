/** @file radio_cc2500.c
*
* @brief CC2500 radio functions
*
* @author Alvaro Prieto
*/
#include "radio_cc2500.h"
#include "cc2500.h"
#include "uart.h"
#include "intrinsics.h"
#include <signal.h>
#include <string.h>

// Define positions in buffer for various fields
#define LENGTH_FIELD  (0)
#define ADDRESS_FIELD (1)
#define DATA_FIELD    (2)

static uint8_t dummy_callback( uint8_t*, uint8_t );
uint8_t receive_packet( uint8_t*, uint8_t* );

extern cc2500_settings_t cc2500_settings;

// Receive buffer
static uint8_t p_rx_buffer[CC2500_BUFFER_LENGTH];
static uint8_t p_tx_buffer[CC2500_BUFFER_LENGTH];

// Holds pointers to all callback functions for CCR registers (and overflow)
static uint8_t (*rx_callback)( uint8_t*, uint8_t ) = dummy_callback;

//
// Optimum PATABLE levels according to Table 31 on CC2500 datasheet
//
static const uint8_t power_table[] = { 
                              0x00, 0x50, 0x44, 0xC0, // -55, -30, -28, -26 dBm
                              0x84, 0x81, 0x46, 0x93, // -24, -22, -20, -18 dBm
                              0x55, 0x8D, 0xC6, 0x97, // -16, -14, -12, -10 dBm
                              0x6E, 0x7F, 0xA9, 0xBB, // -8,  -6,  -4,  -2  dBm
                              0xFE, 0xFF };           //  0,   1            dBm 

/*******************************************************************************
 * @fn     void setup_radio( uint8_t (*callback)(void) )
 * @brief  Initialize radio and register Rx Callback function
 * ****************************************************************************/
void setup_cc2500( uint8_t (*callback)(uint8_t*, uint8_t) )
{
  uint8_t tx_power = 0xFB;  // Maximum power

  // Set-up rx_callback function
  rx_callback = callback;
  
 initialize_radio();        // Reset radio
  
  __delay_cycles(100);      // Let radio settle (Won't configure unless you do?)

  write_rf_settings();      // Initialize settings                            
  write_burst_register(PATABLE, &tx_power, 1 ); // Set TX power
 
  strobe( SRX );            // Set radio to RX mode

}

/*******************************************************************************
 * @fn     cc2500_tx( uint8_t* p_buffer, uint8_t length )
 * @brief  Send raw message through radio
 * ****************************************************************************/
void cc2500_tx( uint8_t* p_buffer, uint8_t length )
{
  GDO0_PxIE &= ~GDO0_PIN;          // Disable interrupt
  
  write_burst_register( FIFO, p_buffer, length );
  
  strobe( STX );                   // Change to TX mode, start data transfer
  
                                   // Wait until GDO0 goes high (sync word txed) 
  while ( !( GDO0_PxIN & GDO0_PIN ) );

  // Transmitting

  while ( GDO0_PxIN & GDO0_PIN );   // Wait until GDO0 goes low (end of packet)

  // Only needed if radio is configured to return to IDLE after transmission
  // Check register MCSM1.TXOFF_MODE
  //strobe( SRX ); 
  
  GDO0_PxIFG &= ~GDO0_PIN;          // Clear flag
  GDO0_PxIE |= GDO0_PIN;            // Enable interrupt  
}

/*******************************************************************************
 * @fn     void cc2500_tx_packet( uint8_t* p_buffer, uint8_t length, 
 *                                                        uint8_t destination )
 * @brief  Send packet through radio. Takes care of adding length and 
 *         destination to packet.
 * ****************************************************************************/
void cc2500_tx_packet( uint8_t* p_buffer, uint8_t length, uint8_t destination )
{
  // Add one to packet length account for address byte
  p_tx_buffer[LENGTH_FIELD] = length + 1;
  
  // Insert destination address to buffer
  p_tx_buffer[ADDRESS_FIELD] = destination;
  
  // Copy message buffer into tx buffer. Add one to account for length byte
  memcpy( &p_tx_buffer[DATA_FIELD], p_buffer, length );
  
  // Add DATA_FIELD to account for packet length and address bytes
  cc2500_tx( p_tx_buffer, (length + DATA_FIELD) );
}

/*******************************************************************************
 * @fn     cc2500_set_address( uint8_t );
 * @brief  Set device address
 * ****************************************************************************/
void cc2500_set_address( uint8_t address )
{
  cc2500_settings.addr = address;
  write_register( &cc2500_settings.addr );
}

/*******************************************************************************
 * @fn     cc2500_set_channel( uint8_t );
 * @brief  Set device channel
 * ****************************************************************************/
void cc2500_set_channel( uint8_t channel )
{
  cc2500_settings.channr = channel;
  write_register( &cc2500_settings.channr );
}

/*******************************************************************************
 * @fn     cc2500_set_power( uint8_t );
 * @brief  Set device transmit power
 * ****************************************************************************/
void cc2500_set_power( uint8_t power )
{
  // Make sure not to read values outside the power table
  if ( power > sizeof(power_table) )
  {
    power = sizeof(power_table) - 1;
  }
  
  // Set TX power
  write_burst_register(PATABLE, (uint8_t *)&power_table[power], 1 ); 
}

/*******************************************************************************
 * @fn     void dummy_callback( void )
 * @brief  empty function works as default callback
 * ****************************************************************************/
static uint8_t dummy_callback( uint8_t* buffer, uint8_t length )
{
  __no_operation();

  return 0;
}

/*******************************************************************************
 * @fn     uint8_t receive_packet( uint8_t* p_buffer, uint8_t* length )
 * @brief  Receive packet from the radio using CC2500
 * ****************************************************************************/
uint8_t receive_packet( uint8_t* p_buffer, uint8_t* length )
{
  uint8_t status[2];
  uint8_t packet_length;
  
  // Make sure there are bytes to be read in the FIFO buffer
  if ( (read_status( RXBYTES ) & NUM_RXBYTES ) )
  {
    // Read the first byte which contains the packet length
    read_burst_register( FIFO, &packet_length, 1 );

    // Make sure the packet length is smaller than our buffer
    if ( packet_length <= *length )
    {
      // Read the rest of the packet
      read_burst_register( FIFO, p_buffer, packet_length );
      
      // Return packet size in length variable
      *length = packet_length;
      
      // Read two byte status 
      read_burst_register( FIFO, status, 2 );
      
      // Append status bytes to buffer 
      memcpy( &p_buffer[packet_length], status, 2 );
      
      // Return 1 when CRC matches, 0 otherwise
      return ( status[LQI_POS] & CRC_OK );
    }    
    else
    {
      // If the packet is larger than the buffer, flush the RX FIFO
      *length = packet_length;
      
      // Flush RX FIFO
      strobe( SFRX );
      
      return 0;
    }
    
  }
 
  return 0;  
}

/*******************************************************************************
 * @fn     void port2_isr( void )
 * @brief  UART ISR
 * ****************************************************************************/
wakeup interrupt ( PORT2_VECTOR ) port2_isr(void) // CHANGE
{
  uint8_t length = CC2500_BUFFER_LENGTH; 
  
  // Check to see if this interrupt was caused by the GDO0 pin from the CC2500
  if ( GDO0_PxIFG & GDO0_PIN )
  {
      if( receive_packet( p_rx_buffer, &length ) )
      {
        // Successful packet receive, now send data to callback function
        rx_callback( p_rx_buffer, length );
        
      }
      else
      {
        // A failed receive can occur due to bad CRC or (if address checking is
        // enabled) an address mismatch
        
        //uart_write("CRC NOK\r\n", 9);
      }
           
  }
  GDO0_PxIFG &= ~GDO0_PIN;  // Clear interrupt flag 
  
  // Only needed if radio is configured to return to IDLE after transmission
  // Check register MCSM1.TXOFF_MODE
  //strobe( SRX ); // enter receive mode again
}