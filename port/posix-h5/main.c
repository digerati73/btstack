/*
 * Copyright (C) 2014 BlueKitchen GmbH
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the copyright holders nor the names of
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific prior written permission.
 * 4. Any redistribution, use, or modification is done solely for
 *    personal benefit and not for any commercial purpose or for
 *    monetary gain.
 *
 * THIS SOFTWARE IS PROVIDED BY BLUEKITCHEN GMBH AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL MATTHIAS
 * RINGWALD OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF
 * THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * Please inquire about commercial licensing options at 
 * contact@bluekitchen-gmbh.com
 *
 */

#define __BTSTACK_FILE__ "main.c"

// *****************************************************************************
//
// minimal setup for HCI code
//
// *****************************************************************************

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "btstack_config.h"

#include "btstack_debug.h"
#include "btstack_event.h"
#include "ble/le_device_db_tlv.h"
#include "classic/btstack_link_key_db_tlv.h"
#include "btstack_memory.h"
#include "btstack_run_loop.h"
#include "btstack_run_loop_posix.h"
#include "bluetooth_company_id.h"
#include "hci.h"
#include "hci_dump.h"
#include "btstack_stdin.h"
#include "btstack_tlv_posix.h"
#include "btstack_uart.h"

#include "btstack_chipset_csr.h"
#include "btstack_chipset_cc256x.h"
#include "btstack_chipset_em9301.h"
#include "btstack_chipset_stlc2500d.h"
#include "btstack_chipset_tc3566x.h"

int btstack_main(int argc, const char * argv[]);

#define TLV_DB_PATH_PREFIX "/tmp/btstack_"
#define TLV_DB_PATH_POSTFIX ".tlv"
static char tlv_db_path[100];
static const btstack_tlv_t * tlv_impl;
static btstack_tlv_posix_t   tlv_context;

static hci_transport_config_uart_t config = {
    HCI_TRANSPORT_CONFIG_UART,
    115200,
    0,  // main baudrate
    1,  // flow control
    NULL,
    BTSTACK_UART_PARITY_EVEN, // parity
};

static btstack_packet_callback_registration_t hci_event_callback_registration;
static void local_version_information_handler(uint8_t * packet);

static void sigint_handler(int param){
    UNUSED(param);

    printf("CTRL-C - SIGINT received, shutting down..\n");   
    log_info("sigint_handler: shutting down");

    // reset anyway
    btstack_stdin_reset();

    // power down
    hci_power_control(HCI_POWER_OFF);
    hci_close();
    log_info("Good bye, see you.\n");    
    exit(0);
}

static int led_state = 0;
void hal_led_toggle(void){
    led_state = 1 - led_state;
    printf("LED State %u\n", led_state);
}
static void use_fast_uart(void){
     printf("Using 921600 baud.\n");
     config.baudrate_main = 921600;
}

static void local_version_information_handler(uint8_t * packet){
    printf("Local version information:\n");
    uint16_t hci_version    = packet[6];
    uint16_t hci_revision   = little_endian_read_16(packet, 7);
    uint16_t lmp_version    = packet[9];
    uint16_t manufacturer   = little_endian_read_16(packet, 10);
    uint16_t lmp_subversion = little_endian_read_16(packet, 12);
    printf("- HCI Version  0x%04x\n", hci_version);
    printf("- HCI Revision 0x%04x\n", hci_revision);
    printf("- LMP Version  0x%04x\n", lmp_version);
    printf("- LMP Revision 0x%04x\n", lmp_subversion);
    printf("- Manufacturer 0x%04x\n", manufacturer);
    switch (manufacturer){
        case BLUETOOTH_COMPANY_ID_CAMBRIDGE_SILICON_RADIO:
            printf("Cambridge Silicon Radio - CSR chipset, Build ID: %u.\n", hci_revision);
            use_fast_uart();
            hci_set_chipset(btstack_chipset_csr_instance());
            break;
        case BLUETOOTH_COMPANY_ID_TEXAS_INSTRUMENTS_INC: 
            printf("Texas Instruments - CC256x compatible chipset.\n");
            use_fast_uart();
            hci_set_chipset(btstack_chipset_cc256x_instance());
            break;
        case BLUETOOTH_COMPANY_ID_BROADCOM_CORPORATION:   
            printf("Broadcom chipset. Not supported by posix-h5 port, please use port/posix-h5-bcm\n");
            exit(10);
            break;
        case BLUETOOTH_COMPANY_ID_ST_MICROELECTRONICS:   
            printf("ST Microelectronics - using STLC2500d driver.\n");
            use_fast_uart();
            hci_set_chipset(btstack_chipset_stlc2500d_instance());
            break;
        case BLUETOOTH_COMPANY_ID_EM_MICROELECTRONIC_MARIN_SA:
            printf("EM Microelectronics - using EM9301 driver.\n");
            hci_set_chipset(btstack_chipset_em9301_instance());
            break;
        default:
            printf("Unknown manufacturer / manufacturer not supported yet.\n");
            break;
    }
}

static void packet_handler (uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    bd_addr_t addr;
    if (packet_type != HCI_EVENT_PACKET) return;
    switch (hci_event_packet_get_type(packet)){
        case BTSTACK_EVENT_STATE:
            if (btstack_event_state_get_state(packet) != HCI_STATE_WORKING) break;
            gap_local_bd_addr(addr);
            printf("BTstack up and running at %s\n",  bd_addr_to_str(addr));
            // setup TLV
            strcpy(tlv_db_path, TLV_DB_PATH_PREFIX);
            strcat(tlv_db_path, bd_addr_to_str(addr));
            strcat(tlv_db_path, TLV_DB_PATH_POSTFIX);
            tlv_impl = btstack_tlv_posix_init_instance(&tlv_context, tlv_db_path);
            btstack_tlv_set_instance(tlv_impl, &tlv_context);
#ifdef ENABLE_CLASSIC
            hci_set_link_key_db(btstack_link_key_db_tlv_get_instance(tlv_impl, &tlv_context));
#endif    
#ifdef ENABLE_BLE
            le_device_db_tlv_configure(tlv_impl, &tlv_context);
#endif
            break;
        case HCI_EVENT_COMMAND_COMPLETE:
            if (HCI_EVENT_IS_COMMAND_COMPLETE(packet, hci_read_local_name)){
                if (hci_event_command_complete_get_return_parameters(packet)[0]) break;
                // terminate, name 248 chars
                packet[6+248] = 0;
                printf("Local name: %s\n", &packet[6]);
            }        
            if (HCI_EVENT_IS_COMMAND_COMPLETE(packet, hci_read_local_version_information)){
                local_version_information_handler(packet);
            }
            break;
        default:
            break;
    }
}

int main(int argc, const char * argv[]){

	/// GET STARTED with BTstack ///
	btstack_memory_init();
    btstack_run_loop_init(btstack_run_loop_posix_get_instance());
	    
    // use logger: format HCI_DUMP_PACKETLOGGER, HCI_DUMP_BLUEZ or HCI_DUMP_STDOUT
    const char * pklg_path = "/tmp/hci_dump.pklg";
    hci_dump_open(pklg_path, HCI_DUMP_PACKETLOGGER);
    printf("Packet Log: %s\n", pklg_path);

    // pick serial port
    config.device_name = "/dev/tty.usbserial-A900K2WS"; // DFROBOT
    // config.device_name = "/dev/tty.usbserial-A50285BI"; // BOOST-CC2564MODA New
    // config.device_name = "/dev/tty.usbserial-A9OVNX5P"; // RedBear IoT pHAT breakout board
    // config.device_name = "/dev/tty.usbserial-A900K0VK"; // CSR8811 breakout board

    // accept path from command line
    if (argc >= 3 && strcmp(argv[1], "-u") == 0){
        config.device_name = argv[2];
        argc -= 2;
        memmove(&argv[1], &argv[3], (argc-1) * sizeof(char *));
    }
    printf("H5 device: %s\n", config.device_name);

    // init HCI
    const btstack_uart_t * uart_driver = (const btstack_uart_t *) btstack_uart_posix_instance();
    const hci_transport_t * transport = hci_transport_h5_instance(uart_driver);
	hci_init(transport, (void*) &config);

    // set BD_ADDR for CSR without Flash/unique address
    // bd_addr_t own_address = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
    // btstack_chipset_csr_set_bd_addr(own_address);

    // enable auto-sleep mode
    // hci_transport_h5_set_auto_sleep(300);

    // register for HCI events
    hci_event_callback_registration.callback = &packet_handler;
    hci_add_event_handler(&hci_event_callback_registration);

    // handle CTRL-c
    signal(SIGINT, sigint_handler);

    // setup app
    btstack_main(argc, argv);

    // go
    btstack_run_loop_execute();    

    return 0;
}
