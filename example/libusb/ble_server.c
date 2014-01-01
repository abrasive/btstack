/*
 * Copyright (C) 2011-2013 by Matthias Ringwald
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
 * 4. This software may not be used in a commercial product
 *    without an explicit license granted by the copyright holder. 
 *
 * THIS SOFTWARE IS PROVIDED BY MATTHIAS RINGWALD AND CONTRIBUTORS
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
 */

//*****************************************************************************
//
// att device demo
//
//*****************************************************************************

// TODO: seperate BR/EDR from LE ACL buffers
// ..

// NOTE: Supports only a single connection

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"

#include <btstack/run_loop.h>
#include "debug.h"
#include "btstack_memory.h"
#include "hci.h"
#include "hci_dump.h"

#include "l2cap.h"

#include "att.h"

#include "rijndael.h"

// Bluetooth Spec definitions
typedef enum {
    SM_CODE_PAIRING_REQUEST = 0X01,
    SM_CODE_PAIRING_RESPONSE,
    SM_CODE_PAIRING_CONFIRM,
    SM_CODE_PAIRING_RANDOM,
    SM_CODE_PAIRING_FAILED,
    SM_CODE_ENCRYPTION_INFORMATION,
    SM_CODE_MASTER_IDENTIFICATION,
    SM_CODE_IDENTITY_INFORMATION,
    SM_CODE_IDENTITY_ADDRESS_INFORMATION,
    SM_CODE_SIGNING_INFORMATION,
    SM_CODE_SECURITY_REQUEST
} SECURITY_MANAGER_COMMANDS;

// Authentication requirement flags
#define SM_AUTHREQ_NO_BONDING 0x00
#define SM_AUTHREQ_BONDING 0x01
#define SM_AUTHREQ_MITM_PROTECTION 0x04

// Key distribution flags used by spec
#define SM_KEYDIST_ENC_KEY 0X01
#define SM_KEYDIST_ID_KEY  0x02
#define SM_KEYDIST_SIGN    0x04

// Key distribution flags used internally
#define SM_KEYDIST_FLAG_ENCRYPTION_INFORMATION       0x01
#define SM_KEYDIST_FLAG_MASTER_IDENTIFICATION        0x02
#define SM_KEYDIST_FLAG_IDENTITY_INFORMATION         0x04
#define SM_KEYDIST_FLAG_IDENTITY_ADDRESS_INFORMATION 0x08
#define SM_KEYDIST_FLAG_SIGNING_IDENTIFICATION       0x10

// STK Generation Methods
#define SM_STK_GENERATION_METHOD_JUST_WORKS 0x01
#define SM_STK_GENERATION_METHOD_OOB        0x02
#define SM_STK_GENERATION_METHOD_PASSKEY    0x04

// Pairing Failed Reasons
#define SM_REASON_RESERVED                     0x00
#define SM_REASON_PASSKEYT_ENTRY_FAILED        0x01
#define SM_REASON_OOB_NOT_AVAILABLE            0x02
#define SM_REASON_AUTHENTHICATION_REQUIREMENTS 0x03
#define SM_REASON_CONFIRM_VALUE_FAILED         0x04
#define SM_REASON_PAIRING_NOT_SUPPORTED        0x05
#define SM_REASON_ENCRYPTION_KEY_SIZE          0x06
#define SM_REASON_COMMAND_NOT_SUPPORTED        0x07
#define SM_REASON_UNSPECIFIED_REASON           0x08
#define SM_REASON_REPEATED_ATTEMPTS            0x09
// also, invalid parameters
// and reserved

// IO Capability Values
typedef enum {
    IO_CAPABILITY_DISPLAY_ONLY = 0,
    IO_CAPABILITY_DISPLAY_YES_NO,
    IO_CAPABILITY_KEYBOARD_ONLY,
    IO_CAPABILITY_NO_INPUT_NO_OUTPUT,
    IO_CAPABILITY_KEYBOARD_DISPLAY, // not used by secure simple pairing
    IO_CAPABILITY_UNKNOWN = 0xff
} io_capability_t;


typedef enum {
    GAP_RANDOM_ADDRESS_TYPE_OFF = 0,
    GAP_RANDOM_ADDRESS_NON_RESOLVABLE,
    GAP_RANDOM_ADDRESS_RESOLVABLE,
} gap_random_address_type_t;
//
// types used by client
//

typedef struct sm_event {
    uint8_t   type;   // see <btstack/hci_cmds.h> SM_...
    uint8_t   addr_type;
    bd_addr_t address;
    uint32_t  passkey;  // only used for SM_PASSKEY_DISPLAY_NUMBER 
} sm_event_t;

//
// internal types and globals
//

typedef uint8_t key_t[16];

typedef enum {

    SM_STATE_IDLE,

    SM_STATE_SEND_SECURITY_REQUEST,
    SM_STATE_SEND_LTK_REQUESTED_NEGATIVE_REPLY,

    // Phase 1: Pairing Feature Exchange

    SM_STATE_PH1_SEND_PAIRING_RESPONSE,
    SM_STATE_PH1_W4_PAIRING_CONFIRM,
    SM_STATE_PH1_W4_USER_RESPONSE,

    SM_STATE_SEND_PAIRING_FAILED,
    SM_STATE_SEND_PAIRING_RANDOM,

    // Phase 2: Authenticating and Encrypting

    // get random number for TK if we show it 
    SM_STATE_PH2_GET_RANDOM_TK,
    SM_STATE_PH2_W4_RANDOM_TK,

    // calculate confirm values for local and remote connection
    SM_STATE_PH2_C1_GET_RANDOM_A,
    SM_STATE_PH2_C1_W4_RANDOM_A,
    SM_STATE_PH2_C1_GET_RANDOM_B,
    SM_STATE_PH2_C1_W4_RANDOM_B,
    SM_STATE_PH2_C1_GET_ENC_A,
    SM_STATE_PH2_C1_W4_ENC_A,
    SM_STATE_PH2_C1_GET_ENC_B,
    SM_STATE_PH2_C1_W4_ENC_B,
    SM_STATE_PH2_C1_SEND_PAIRING_CONFIRM,
    SM_STATE_PH2_W4_PAIRING_RANDOM,
    SM_STATE_PH2_C1_GET_ENC_C,
    SM_STATE_PH2_C1_W4_ENC_C,
    SM_STATE_PH2_C1_GET_ENC_D,
    SM_STATE_PH2_C1_W4_ENC_D,

    // calc STK
    SM_STATE_PH2_CALC_STK,
    SM_STATE_PH2_W4_STK,
    SM_STATE_PH2_SEND_STK,
    SM_STATE_PH2_W4_LTK_REQUEST,
    SM_STATE_PH2_W4_CONNECTION_ENCRYPTED,

    // Phase 3: Transport Specific Key Distribution
    
    // calculate DHK, Y, EDIV, and LTK
    SM_STATE_PH3_GET_RANDOM,
    SM_STATE_PH3_W4_RANDOM,
    SM_STATE_PH3_GET_DIV,
    SM_STATE_PH3_W4_DIV,
    SM_STATE_PH3_Y_GET_ENC,
    SM_STATE_PH3_Y_W4_ENC,
    SM_STATE_PH3_LTK_GET_ENC,
    SM_STATE_PH3_LTK_W4_ENC,

    //
    SM_STATE_DISTRIBUTE_KEYS,

    // re establish previously distribued LTK
    SM_STATE_PH4_Y_GET_ENC,
    SM_STATE_PH4_Y_W4_ENC,
    SM_STATE_PH4_LTK_GET_ENC,
    SM_STATE_PH4_LTK_W4_ENC,
    SM_STATE_PH4_SEND_LTK,

    SM_STATE_TIMEOUT, // no other security messages are exchanged

} security_manager_state_t;

typedef enum {
    DKG_W4_WORKING,
    DKG_CALC_IRK,
    DKG_W4_IRK,
    DKG_CALC_DHK,
    DKG_W4_DHK,
    DKG_READY
} derived_key_generation_t;

typedef enum {
    RAU_IDLE,
    RAU_GET_RANDOM,
    RAU_W4_RANDOM,
    RAU_GET_ENC,
    RAU_W4_ENC,
    RAU_SET_ADDRESS,
} random_address_update_t;

typedef enum {
    CMAC_IDLE,
    CMAC_CALC_SUBKEYS,
    CMAC_W4_SUBKEYS,
    CMAC_CALC_MI,
    CMAC_W4_MI,
    CMAC_CALC_MLAST,
    CMAC_W4_MLAST
} cmac_state_t;

typedef enum {
    JUST_WORKS,
    PK_RESP_INPUT,  // Initiator displays PK, initiator inputs PK 
    PK_INIT_INPUT,  // Responder displays PK, responder inputs PK
    OK_BOTH_INPUT,  // Only input on both, both input PK
    OOB             // OOB available on both sides
} stk_generation_method_t;

typedef enum {
    SM_USER_RESPONSE_IDLE,
    SM_USER_RESPONSE_PENDING,
    SM_USER_RESPONSE_CONFIRM,
    SM_USER_RESPONSE_PASSKEY,
    SM_USER_RESPONSE_DECLINE
} sm_user_response_t;

//
// GLOBAL DATA
//

// Security Manager Master Keys, please use sm_set_er(er) and sm_set_ir(ir) with your own 128 bit random values
static key_t sm_persistent_er;
static key_t sm_persistent_ir;

// derived from sm_persistent_ir
static key_t sm_persistent_dhk;
static key_t sm_persistent_irk;

// derived from sm_persistent_er
// ..

static uint8_t sm_accepted_stk_generation_methods;
static uint8_t sm_max_encryption_key_size;
static uint8_t sm_min_encryption_key_size;

static uint8_t sm_encryption_key_size;
static uint8_t sm_s_auth_req = 0;
static uint8_t sm_s_io_capabilities = IO_CAPABILITY_UNKNOWN;
static uint8_t sm_s_request_security = 0;



// 
static derived_key_generation_t dkg_state = DKG_W4_WORKING;

// random address update
static random_address_update_t rau_state = RAU_IDLE;
static bd_addr_t sm_random_address;

// resolvable private address lookup

//
static uint8_t   sm_s_addr_type;
static bd_addr_t sm_s_address;

// PER INSTANCE DATA

static security_manager_state_t sm_state_responding = SM_STATE_IDLE;
static uint16_t sm_response_handle = 0;
static uint8_t  sm_pairing_failed_reason = 0;

// SM timeout
static timer_source_t sm_timeout;

// data to send to aes128 crypto engine, see sm_aes128_set_key and sm_aes128_set_plaintext
static key_t   sm_aes128_key;
static key_t   sm_aes128_plaintext;
static uint8_t sm_aes128_active;

// generation method and temporary key for STK - STK is stored in sm_s_ltk
static stk_generation_method_t sm_stk_generation_method;
static key_t sm_tk;

// user response
static uint8_t   sm_user_response;

// defines which keys will be send  after connection is encrypted
static int sm_key_distribution_send_set;
static int sm_key_distribution_received_set;

//
// Volume 3, Part H, Chapter 24
// "Security shall be initiated by the Security Manager in the device in the master role.
// The device in the slave role shall be the responding device."
// -> master := initiator, slave := responder
//

static uint8_t   sm_m_io_capabilities;
static uint8_t   sm_m_have_oob_data;
static uint8_t   sm_m_auth_req;
static uint8_t   sm_m_max_encryption_key_size;
static uint8_t   sm_m_key_distribution;
static uint8_t   sm_m_preq[7];
static key_t     sm_m_random;
static key_t     sm_m_confirm;

static key_t     sm_s_random;
static key_t     sm_s_confirm;
static uint8_t   sm_s_pres[7];

// key distribution, slave sends
static key_t     sm_s_ltk;
static uint16_t  sm_s_y;
static uint16_t  sm_s_div;
static uint16_t  sm_s_ediv;
static uint8_t   sm_s_rand[8];
static key_t     sm_s_csrk;

// key distribution, received from master
static key_t     sm_m_ltk;
static uint16_t  sm_m_ediv;
static uint8_t   sm_m_rand[8];
static uint8_t   sm_m_addr_type;
static bd_addr_t sm_m_address;
static key_t     sm_m_csrk;
static key_t     sm_m_irk;

// CMAC calculation
static cmac_state_t sm_cmac_state;
static key_t        sm_cmac_k;
static uint16_t     sm_cmac_message_len;
static uint8_t *    sm_cmac_message;
static key_t        sm_cmac_m_last;
static key_t        sm_cmac_x;
static uint8_t      sm_cmac_block_current;
static uint8_t      sm_cmac_block_count;

// CMAC Test Data
uint8_t m16[] = { 0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a};
uint8_t m64[] = { 0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
                  0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
                  0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
                  0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
uint8_t m40[] = { 0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
                  0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
                  0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11};

// @returns 1 if oob data is available
// stores oob data in provided 16 byte buffer if not null
static int (*sm_get_oob_data)(uint8_t addres_type, bd_addr_t * addr, uint8_t * oob_data) = NULL;

// used to notify applicationss that user interaction is neccessary, see sm_notify_t below
static btstack_packet_handler_t sm_client_packet_handler = NULL;

// horizontal: initiator capabilities
// vertial:    responder capabilities
static const stk_generation_method_t stk_generation_method[5][5] = {
    { JUST_WORKS,      JUST_WORKS,       PK_INIT_INPUT,   JUST_WORKS,    PK_INIT_INPUT },
    { JUST_WORKS,      JUST_WORKS,       PK_INIT_INPUT,   JUST_WORKS,    PK_INIT_INPUT },
    { PK_RESP_INPUT,   PK_RESP_INPUT,    OK_BOTH_INPUT,   JUST_WORKS,    PK_RESP_INPUT },
    { JUST_WORKS,      JUST_WORKS,       JUST_WORKS,      JUST_WORKS,    JUST_WORKS    },
    { PK_RESP_INPUT,   PK_RESP_INPUT,    PK_INIT_INPUT,   JUST_WORKS,    PK_RESP_INPUT },
};

// ATT Server

static att_connection_t att_connection;
static uint16_t         att_addr_type;
static bd_addr_t        att_address;

typedef enum {
    ATT_SERVER_IDLE,
    ATT_SERVER_REQUEST_RECEIVED,
    ATT_SERVER_W4_SIGNED_WRITE_VALIDATION,
} att_server_state_t;

static att_server_state_t att_server_state;
static uint16_t         att_request_handle = 0;
static uint16_t         att_request_size   = 0;
static uint8_t          att_request_buffer[28];

// SECURITY MANAGER (SM) MATERIALIZES HERE
static void sm_run();

int sm_cmac_ready();
static void sm_cmac_handle_encryption_result(key_t data);
static void sm_cmac_handle_aes_engine_ready();
static void sm_cmac_start(key_t k, uint16_t message_len, uint8_t * message);

static inline void swapX(uint8_t *src, uint8_t *dst, int len){
    int i;
    for (i = 0; i < len; i++)
        dst[len - 1 - i] = src[i];
}
static inline void swap24(uint8_t src[3], uint8_t dst[3]){
    swapX(src, dst, 3);
}
static inline void swap56(uint8_t src[7], uint8_t dst[7]){
    swapX(src, dst, 7);
}
static inline void swap64(uint8_t src[8], uint8_t dst[8]){
    swapX(src, dst, 8);
}
static inline void swap128(uint8_t src[16], uint8_t dst[16]){
    swapX(src, dst, 16);
}

// @returns 1 if all bytes are 0
static int sm_is_null_random(uint8_t random[8]){
    int i;
    for (i=0; i < 8 ; i++){
        if (random[i]) return 0;
    }
    return 1;
}

static void sm_reset_tk(){
    int i;
    for (i=0;i<16;i++){
        sm_tk[i] = 0;
    }
}

// "For example, if a 128-bit encryption key is 0x123456789ABCDEF0123456789ABCDEF0
// and it is reduced to 7 octets (56 bits), then the resulting key is 0x0000000000000000003456789ABCDEF0.""
static void sm_truncate_key(key_t key, int max_encryption_size){
    int i;
    for (i = max_encryption_size ; i < 16 ; i++){
        key[15-i] = 0;
    }
}

static void print_key(const char * name, key_t key){
    printf("%-6s ", name);
    hexdump(key, 16);
}

static void print_hex16(const char * name, uint16_t value){
    printf("%-6s 0x%04x\n", name, value);
}

// Central Device db interface

void central_device_db_init();

// @returns true, if successful
int central_device_db_add(int addr_type, bd_addr_t addr, key_t irk, key_t csrk);

// @returns number of device in db
int central_device_db_count(void);

// get device information: addr type and address
void central_device_db_info(int index, int * addr_type, bd_addr_t addr, key_t csrk);

// get signature key
void central_device_db_csrk(int index, key_t csrk);

// query last used/seen signing counter
uint32_t central_device_db_counter_get(int index);

// update signing counter
void central_device_db_counter_set(int index, uint32_t counter);

// free device
void central_device_db_remove(int index);

// Central Device db implemenation using static memory
typedef struct central_device_memory_db {
    int addr_type;
    bd_addr_t addr;
    key_t csrk;
    key_t irk;
    uint32_t signing_counter;
} central_device_memory_db_t;

#define CENTRAL_DEVICE_MEMORY_SIZE 4
static central_device_memory_db_t central_devices[CENTRAL_DEVICE_MEMORY_SIZE];
static int central_devices_count;

void central_device_db_init(){
    central_devices_count = 0;
}

// @returns number of device in db
int central_device_db_count(void){
    return central_devices_count;
}

// free device - TODO not implemented
void central_device_db_remove(int index){
}

int central_device_db_add(int addr_type, bd_addr_t addr, key_t csrk, key_t irk){
    if (central_devices_count >= CENTRAL_DEVICE_MEMORY_SIZE) return 0;
    central_devices[central_devices_count].addr_type = addr_type;
    memcpy(central_devices[central_devices_count].addr, addr, 6);
    memcpy(central_devices[central_devices_count].csrk, csrk, 16);
    memcpy(central_devices[central_devices_count].irk, irk, 16);
    central_devices[central_devices_count].signing_counter = 0; 
    central_devices_count++;
    return 1;
}


// get device information: addr type and address
void central_device_db_info(int index, int * addr_type, bd_addr_t addr, key_t irk){
    if (addr_type) *addr_type = central_devices[index].addr_type;
    if (addr) memcpy(addr, central_devices[index].addr, 6);
    if (irk) memcpy(irk, central_devices[index].irk, 16);
}

// get signature key
void central_device_db_csrk(int index, key_t csrk){
    if (csrk) memcpy(csrk, central_devices[index].csrk, 16);
}


// query last used/seen signing counter
uint32_t central_device_db_counter_get(int index){
    return central_devices[index].signing_counter;
}

// update signing counter
void central_device_db_counter_set(int index, uint32_t counter){
    central_devices[index].signing_counter = counter;
}


// SMP Timeout implementation

// Upon transmission of the Pairing Request command or reception of the Pairing Request command,
// the Security Manager Timer shall be reset and started.
//
// The Security Manager Timer shall be reset when an L2CAP SMP command is queued for transmission.
//
// If the Security Manager Timer reaches 30 seconds, the procedure shall be considered to have failed,
// and the local higher layer shall be notified. No further SMP commands shall be sent over the L2CAP
// Security Manager Channel. A new SM procedure shall only be performed when a new physical link has been
// established.

static void sm_timeout_handler(timer_source_t * timer){
    printf("SM timeout");
    sm_state_responding = SM_STATE_TIMEOUT;
}
static void sm_timeout_start(){
    run_loop_set_timer_handler(&sm_timeout, sm_timeout_handler);
    run_loop_set_timer(&sm_timeout, 30000); // 30 seconds sm timeout
    run_loop_add_timer(&sm_timeout);
}
static void sm_timeout_stop(){
    run_loop_remove_timer(&sm_timeout);
}
static void sm_timeout_reset(){
    sm_timeout_stop();
    sm_timeout_start();    
}

// end of sm timeout

// GAP Random Address updates
static gap_random_address_type_t gap_random_adress_type;
static timer_source_t gap_random_address_update_timer; 
static uint32_t gap_random_adress_update_period;

static void gap_random_address_update_handler(timer_source_t * timer){
    printf("GAP Random Address Update due\n");
    run_loop_set_timer(&gap_random_address_update_timer, gap_random_adress_update_period);
    run_loop_add_timer(&gap_random_address_update_timer);
    if (rau_state != RAU_IDLE) return;
    rau_state = RAU_GET_RANDOM;
    sm_run();
}

static void gap_random_address_update_start(){
    run_loop_set_timer_handler(&gap_random_address_update_timer, gap_random_address_update_handler);
    run_loop_set_timer(&gap_random_address_update_timer, gap_random_adress_update_period);
    run_loop_add_timer(&gap_random_address_update_timer);
}

static void gap_random_address_update_stop(){
    run_loop_remove_timer(&gap_random_address_update_timer);
}

static inline void sm_aes128_set_key(key_t key){
    memcpy(sm_aes128_key, key, 16);
} 

static inline void sm_aes128_set_plaintext(key_t plaintext){
    memcpy(sm_aes128_plaintext, plaintext, 16);
} 

// asserts: sm_aes128_active == 0, hci_can_send_command == 1
static void sm_aes128_start(key_t key, key_t plaintext){
    sm_aes128_active = 1;
    key_t key_flipped, plaintext_flipped;
    swap128(key, key_flipped);
    swap128(plaintext, plaintext_flipped);
    hci_send_cmd(&hci_le_encrypt, key_flipped, plaintext_flipped);
}

static void sm_ah_r_prime(uint8_t r[3], key_t d1_prime){
    // r'= padding || r
    memset(d1_prime, 0, 16);
    memcpy(&d1_prime[13], r, 3);
}

static void sm_d1_d_prime(uint16_t d, uint16_t r, key_t d1_prime){
    // d'= padding || r || d
    memset(d1_prime, 0, 16);
    net_store_16(d1_prime, 12, r);
    net_store_16(d1_prime, 14, d);
}

static void sm_dm_r_prime(uint8_t r[8], key_t r_prime){
    // r’ = padding || r
    memset(r_prime, 0, 16);
    memcpy(&r_prime[8], r, 8);
}


// calculate arguments for first AES128 operation in C1 function
static void sm_c1_t1(key_t r, uint8_t preq[7], uint8_t pres[7], uint8_t iat, uint8_t rat, key_t t1){

    // p1 = pres || preq || rat’ || iat’
    // "The octet of iat’ becomes the least significant octet of p1 and the most signifi-
    // cant octet of pres becomes the most significant octet of p1.
    // For example, if the 8-bit iat’ is 0x01, the 8-bit rat’ is 0x00, the 56-bit preq
    // is 0x07071000000101 and the 56 bit pres is 0x05000800000302 then
    // p1 is 0x05000800000302070710000001010001."
    
    key_t p1;
    swap56(pres, &p1[0]);
    swap56(preq, &p1[7]);
    p1[14] = rat;
    p1[15] = iat;
    print_key("p1", p1);
    print_key("r", r);
    
    // t1 = r xor p1
    int i;
    for (i=0;i<16;i++){
        t1[i] = r[i] ^ p1[i];
    }
    print_key("t1", t1);
}

// calculate arguments for second AES128 operation in C1 function
static void sm_c1_t3(key_t t2, bd_addr_t ia, bd_addr_t ra, key_t t3){
     // p2 = padding || ia || ra
    // "The least significant octet of ra becomes the least significant octet of p2 and
    // the most significant octet of padding becomes the most significant octet of p2.
    // For example, if 48-bit ia is 0xA1A2A3A4A5A6 and the 48-bit ra is
    // 0xB1B2B3B4B5B6 then p2 is 0x00000000A1A2A3A4A5A6B1B2B3B4B5B6.
    
    key_t p2;
    memset(p2, 0, 16);
    memcpy(&p2[4],  ia, 6);
    memcpy(&p2[10], ra, 6);
    print_key("p2", p2);

    // c1 = e(k, t2_xor_p2)
    int i;
    for (i=0;i<16;i++){
        t3[i] = t2[i] ^ p2[i];
    }
    print_key("t3", t3);
}

static void sm_s1_r_prime(key_t r1, key_t r2, key_t r_prime){
    print_key("r1", r1);
    print_key("r2", r2);
    memcpy(&r_prime[8], &r2[8], 8);
    memcpy(&r_prime[0], &r1[8], 8);
}

static void sm_notify_client(uint8_t type, uint8_t addr_type, bd_addr_t address, uint32_t passkey){

    sm_event_t event;
    event.type = type;
    event.addr_type = addr_type;
    BD_ADDR_COPY(event.address, address);
    event.passkey = passkey;

    // dummy implementation
    printf("sm_notify_client: event 0x%02x, addres_type %u, address (), num '%06u'", event.type, event.addr_type, event.passkey);

    if (!sm_client_packet_handler) return;
    sm_client_packet_handler(HCI_EVENT_PACKET, 0, (uint8_t*) &event, sizeof(sm_event_t));
}

// decide on stk generation based on
// - pairing request
// - io capabilities
// - OOB data availability
static void sm_tk_setup(){

    // default: just works
    sm_stk_generation_method = JUST_WORKS;
    sm_reset_tk();

    // If both devices have out of band authentication data, then the Authentication
    // Requirements Flags shall be ignored when selecting the pairing method and the
    // Out of Band pairing method shall be used.
    if (sm_m_have_oob_data && (*sm_get_oob_data)(att_addr_type, &att_address, sm_tk)){
        sm_stk_generation_method = OOB;
        return;
    }

    // If both devices have not set the MITM option in the Authentication Requirements
    // Flags, then the IO capabilities shall be ignored and the Just Works association
    // model shall be used. 
    if ( ((sm_m_auth_req & SM_AUTHREQ_MITM_PROTECTION) == 0x00) && ((sm_s_auth_req & SM_AUTHREQ_MITM_PROTECTION) == 0)){
        return;
    }

    // Also use just works if unknown io capabilites
    if ((sm_m_io_capabilities > IO_CAPABILITY_KEYBOARD_DISPLAY) || (sm_m_io_capabilities > IO_CAPABILITY_KEYBOARD_DISPLAY)){
        return;
    }

    // Otherwise the IO capabilities of the devices shall be used to determine the
    // pairing method as defined in Table 2.4.
    sm_stk_generation_method = stk_generation_method[sm_m_io_capabilities][sm_s_io_capabilities];
}


static void sm_setup_key_distribution(uint8_t key_set){

    // TODO: handle initiator case here

    // distribute keys as requested by initiator
    sm_key_distribution_send_set = 0;
    sm_key_distribution_received_set = 0;
    
    if (key_set & SM_KEYDIST_ENC_KEY){
        sm_key_distribution_send_set |= SM_KEYDIST_FLAG_ENCRYPTION_INFORMATION;
        sm_key_distribution_send_set |= SM_KEYDIST_FLAG_MASTER_IDENTIFICATION;        
    }
    if (key_set & SM_KEYDIST_ID_KEY){
        sm_key_distribution_send_set |= SM_KEYDIST_FLAG_IDENTITY_INFORMATION;
        sm_key_distribution_send_set |= SM_KEYDIST_FLAG_IDENTITY_ADDRESS_INFORMATION;        
    }
    if (key_set & SM_KEYDIST_SIGN){
        sm_key_distribution_send_set |= SM_KEYDIST_FLAG_SIGNING_IDENTIFICATION;
    }
}

// CMAC Implementation using AES128 engine
static void sm_shift_left_by_one_bit_inplace(int len, uint8_t * data){
    int i;
    int carry = 0;
    for (i=len-1; i >= 0 ; i--){
        int new_carry = data[i] >> 7;
        data[i] = data[i] << 1 | carry;
        carry = new_carry;
    }
}

static int sm_cmac_last_block_complete(){
    if (sm_cmac_message_len == 0) return 0;
    return (sm_cmac_message_len & 0x0f) == 0;
}

static void sm_cmac_start(key_t k, uint16_t message_len, uint8_t * message){
    memcpy(sm_cmac_k, k, 16);
    sm_cmac_message_len = message_len;
    sm_cmac_message = message;
    sm_cmac_block_current = 0;
    memset(sm_cmac_x, 0, 16);

    // step 2: n := ceil(len/const_Bsize);
    sm_cmac_block_count = (message_len + 15) / 16;

    // step 3: ..
    if (sm_cmac_block_count==0){
        sm_cmac_block_count = 1;
    }

    // first, we need to compute l for k1, k2, and m_last
    sm_cmac_state = CMAC_CALC_SUBKEYS;
}

int sm_cmac_ready(){
    return sm_cmac_state != CMAC_IDLE;
}

static void sm_cmac_handle_aes_engine_ready(){
    switch (sm_cmac_state){
        case CMAC_CALC_SUBKEYS:
            {
            key_t const_zero;
            memset(const_zero, 0, 16);
            sm_aes128_start(sm_cmac_k, const_zero);
            sm_cmac_state++;
            break;
            }
        case CMAC_CALC_MI: {
            printf("CMAC_CALC_MI\n");
            int j;
            key_t y;
            for (j=0;j<16;j++){
                y[j] = sm_cmac_x[j] ^ sm_cmac_message[sm_cmac_block_current*16 + j];
            }
            sm_cmac_block_current++;
            sm_aes128_start(sm_cmac_k, y);
            sm_cmac_state++;
            break;
        }
        case CMAC_CALC_MLAST: {
            printf("CMAC_CALC_MLAST\n");
            int i;
            key_t y;
            for (i=0;i<16;i++){
                y[i] = sm_cmac_x[i] ^ sm_cmac_m_last[i]; 
            }
            print_key("Y", y);
            sm_cmac_block_current++;
            sm_aes128_start(sm_cmac_k, y);
            sm_cmac_state++;
            break;
        }
        default:
            printf("sm_cmac_handle_aes_engine_ready called in state %u\n", sm_cmac_state);
            break;
    }
}

static void sm_cmac_handle_encryption_result(key_t data){
    switch (sm_cmac_state){
        case CMAC_W4_SUBKEYS: {
            key_t k1;
            memcpy(k1, data, 16);
            sm_shift_left_by_one_bit_inplace(16, k1);
            if (data[0] & 0x80){
                k1[15] ^= 0x87;
            }
            key_t k2;
            memcpy(k2, k1, 16);
            sm_shift_left_by_one_bit_inplace(16, k2);
            if (k1[0] & 0x80){
                k2[15] ^= 0x87;
            } 

            print_key("k", sm_cmac_k);
            print_key("k1", k1);
            print_key("k2", k2);

            // step 4: set m_last
            if (sm_cmac_last_block_complete()){
                printf("sm_cmac_last_block_complete = 1\n");
                int i;
                for (i=0;i<16;i++){
                    sm_cmac_m_last[i] = sm_cmac_message[sm_cmac_message_len - 16 + i] ^ k1[i];
                }
            } else {
                printf("sm_cmac_last_block_complete = 0\n");
                int valid_octets_in_last_block = sm_cmac_message_len & 0x0f;
                int i;
                for (i=0;i<16;i++){
                    if (i < valid_octets_in_last_block){
                        sm_cmac_m_last[i] = sm_cmac_message[(sm_cmac_message_len & 0xfff0) + i] ^ k2[i];
                        continue;
                    }
                    if (i == valid_octets_in_last_block){
                        sm_cmac_m_last[i] = 0x80 ^ k2[i];
                        continue;
                    }
                    sm_cmac_m_last[i] = k2[i];
                }
            }
            print_key("ML", sm_cmac_m_last);


            // next
            sm_cmac_state = sm_cmac_block_current < sm_cmac_block_count - 1 ? CMAC_CALC_MI : CMAC_CALC_MLAST;  
            printf("next %u\n", sm_cmac_state);
            break;
        }
        case CMAC_W4_MI:
            memcpy(sm_cmac_x, data, 16);
            sm_cmac_state = sm_cmac_block_current < sm_cmac_block_count - 1 ? CMAC_CALC_MI : CMAC_CALC_MLAST;  
            break;
        case CMAC_W4_MLAST:
            // done
            print_key("T", data);
            break;
        default:
            printf("sm_cmac_handle_encryption_result called in state %u\n", sm_cmac_state);
            break;
    }
}

static void sm_run(void){

    // assert that we can send either one
    if (!hci_can_send_packet_now(HCI_COMMAND_DATA_PACKET)) return;
    if (!hci_can_send_packet_now(HCI_ACL_DATA_PACKET)) return;

    // distributed key generation
    switch (dkg_state){
        case DKG_CALC_IRK:
            // already busy?
            if (sm_aes128_active) break;
            {
            // IRK = d1(IR, 1, 0)
            key_t d1_prime;
            sm_d1_d_prime(1, 0, d1_prime);  // plaintext
            sm_aes128_start(sm_persistent_ir, d1_prime);
            dkg_state++;
            }
        case DKG_CALC_DHK:
            // already busy?
            if (sm_aes128_active) break;
            {
            // DHK = d1(IR, 3, 0)
            key_t d1_prime;
            sm_d1_d_prime(3, 0, d1_prime);  // plaintext
            sm_aes128_start(sm_persistent_ir, d1_prime);
            dkg_state++;
            }
            return;
        default:
            break;  
    }

    // random address updates
    switch (rau_state){
        case RAU_GET_RANDOM:
            hci_send_cmd(&hci_le_rand);
            rau_state++;
            return;
        case RAU_GET_ENC:
            // already busy?
            if (sm_aes128_active) break;
            {
            key_t r_prime;
            sm_ah_r_prime(sm_random_address, r_prime);
            sm_aes128_start(sm_persistent_irk, r_prime);
            rau_state++;
            return;
            }
        case RAU_SET_ADDRESS:
            printf("New random address: ");
            print_bd_addr(sm_random_address);
            printf("\n");
            hci_send_cmd(&hci_le_set_random_address, sm_random_address);
            rau_state = RAU_IDLE;
            return;
        default:
            break;
    }

    // cmac
    switch (sm_cmac_state){
        case CMAC_CALC_SUBKEYS:
        case CMAC_CALC_MI:
        case CMAC_CALC_MLAST:
            // already busy?
            if (sm_aes128_active) break;
            sm_cmac_handle_aes_engine_ready();
            return;
        default:
            break;
    }

    // responding state
    switch (sm_state_responding){

        case SM_STATE_SEND_SECURITY_REQUEST: {
            uint8_t buffer[2];
            buffer[0] = SM_CODE_SECURITY_REQUEST;
            buffer[1] = SM_AUTHREQ_BONDING;
            l2cap_send_connectionless(sm_response_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
            sm_state_responding = SM_STATE_IDLE;            
            return;
        }

        case SM_STATE_PH1_SEND_PAIRING_RESPONSE: {

            uint8_t buffer[7];

            memcpy(buffer, sm_m_preq, 7);        
            buffer[0] = SM_CODE_PAIRING_RESPONSE;
            buffer[1] = sm_s_io_capabilities;
            buffer[2] = sm_stk_generation_method == OOB ? 1 : 0;
            buffer[3] = sm_s_auth_req;
            buffer[4] = sm_max_encryption_key_size;

            memcpy(sm_s_pres, buffer, 7);

            // for validate

            l2cap_send_connectionless(sm_response_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
            sm_timeout_reset();

            // notify client for: JUST WORKS confirm, PASSKEY display or input
            sm_user_response = SM_USER_RESPONSE_IDLE;
            switch (sm_stk_generation_method){
                case PK_RESP_INPUT:
                    sm_user_response = SM_USER_RESPONSE_PENDING;
                    sm_notify_client(SM_PASSKEY_INPUT_NUMBER, sm_m_addr_type, sm_m_address, 0); 
                    break;
                case PK_INIT_INPUT:
                    sm_notify_client(SM_PASSKEY_DISPLAY_NUMBER, sm_m_addr_type, sm_m_address, READ_NET_32(sm_tk, 12)); 
                    break;
                case JUST_WORKS:
                    switch (sm_s_io_capabilities){
                        case IO_CAPABILITY_KEYBOARD_DISPLAY:
                        case IO_CAPABILITY_DISPLAY_YES_NO:
                            sm_user_response = SM_USER_RESPONSE_PENDING;
                            sm_notify_client(SM_JUST_WORKS_REQUEST, sm_m_addr_type, sm_m_address, READ_NET_32(sm_tk, 12));
                            break;
                        default:
                            // cannot ask user
                            break;  

                    }
                    break;
                default:
                    break;
            }

            sm_state_responding = SM_STATE_PH1_W4_PAIRING_CONFIRM;
            return;
        }

        case SM_STATE_SEND_LTK_REQUESTED_NEGATIVE_REPLY:
            hci_send_cmd(&hci_le_long_term_key_negative_reply, sm_response_handle);
            sm_state_responding = SM_STATE_IDLE;
            return;

        case SM_STATE_SEND_PAIRING_FAILED: {
            uint8_t buffer[2];
            buffer[0] = SM_CODE_PAIRING_FAILED;
            buffer[1] = sm_pairing_failed_reason;
            l2cap_send_connectionless(sm_response_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
            sm_timeout_stop();
            sm_state_responding = SM_STATE_IDLE;
            break;
        }

        case SM_STATE_SEND_PAIRING_RANDOM: {
            uint8_t buffer[17];
            buffer[0] = SM_CODE_PAIRING_RANDOM;
            swap128(sm_s_random, &buffer[1]);
            l2cap_send_connectionless(sm_response_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
            sm_timeout_reset();
            sm_state_responding = SM_STATE_PH2_W4_LTK_REQUEST;
            break;
        }

        case SM_STATE_PH2_GET_RANDOM_TK:
        case SM_STATE_PH2_C1_GET_RANDOM_A:
        case SM_STATE_PH2_C1_GET_RANDOM_B:
        case SM_STATE_PH3_GET_RANDOM:
        case SM_STATE_PH3_GET_DIV:
            hci_send_cmd(&hci_le_rand);
            sm_state_responding++;
            return;
        case SM_STATE_PH2_C1_GET_ENC_A:
        case SM_STATE_PH2_C1_GET_ENC_B:
        case SM_STATE_PH2_C1_GET_ENC_C:
        case SM_STATE_PH2_C1_GET_ENC_D:
        case SM_STATE_PH2_CALC_STK:
        case SM_STATE_PH3_Y_GET_ENC:
        case SM_STATE_PH3_LTK_GET_ENC:
        case SM_STATE_PH4_Y_GET_ENC:
        case SM_STATE_PH4_LTK_GET_ENC:
            // already busy?
            if (sm_aes128_active) break;
            sm_aes128_start(sm_aes128_key, sm_aes128_plaintext);
            sm_state_responding++;
            return;
        case SM_STATE_PH2_C1_SEND_PAIRING_CONFIRM: {
            uint8_t buffer[17];
            buffer[0] = SM_CODE_PAIRING_CONFIRM;
            swap128(sm_s_confirm, &buffer[1]);
            l2cap_send_connectionless(sm_response_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
            sm_timeout_reset();
            sm_state_responding = SM_STATE_PH2_W4_PAIRING_RANDOM;
            return;
        }
        case SM_STATE_PH2_SEND_STK: {
            key_t stk_flipped;
            swap128(sm_s_ltk, stk_flipped);
            hci_send_cmd(&hci_le_long_term_key_request_reply, sm_response_handle, stk_flipped);
            sm_state_responding = SM_STATE_PH2_W4_CONNECTION_ENCRYPTED;
            return;
        }
        case SM_STATE_PH4_SEND_LTK: {
            key_t ltk_flipped;
            swap128(sm_s_ltk, ltk_flipped);
            hci_send_cmd(&hci_le_long_term_key_request_reply, sm_response_handle, ltk_flipped);
            sm_state_responding = SM_STATE_IDLE;
            return;
        }

        case SM_STATE_DISTRIBUTE_KEYS:
            if (sm_key_distribution_send_set &   SM_KEYDIST_FLAG_ENCRYPTION_INFORMATION){
                sm_key_distribution_send_set &= ~SM_KEYDIST_FLAG_ENCRYPTION_INFORMATION;
                uint8_t buffer[17];
                buffer[0] = SM_CODE_ENCRYPTION_INFORMATION;
                swap128(sm_s_ltk, &buffer[1]);
                l2cap_send_connectionless(sm_response_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                sm_timeout_reset();
                return;
            }
            if (sm_key_distribution_send_set &   SM_KEYDIST_FLAG_MASTER_IDENTIFICATION){
                sm_key_distribution_send_set &= ~SM_KEYDIST_FLAG_MASTER_IDENTIFICATION;
                uint8_t buffer[11];
                buffer[0] = SM_CODE_MASTER_IDENTIFICATION;
                bt_store_16(buffer, 1, sm_s_ediv);
                swap64(sm_s_rand, &buffer[3]);
                l2cap_send_connectionless(sm_response_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                sm_timeout_reset();
                return;
            }
            if (sm_key_distribution_send_set &   SM_KEYDIST_FLAG_IDENTITY_INFORMATION){
                sm_key_distribution_send_set &= ~SM_KEYDIST_FLAG_IDENTITY_INFORMATION;
                uint8_t buffer[17];
                buffer[0] = SM_CODE_IDENTITY_INFORMATION;
                swap128(sm_persistent_irk, &buffer[1]);
                l2cap_send_connectionless(sm_response_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                sm_timeout_reset();
                return;
            }
            if (sm_key_distribution_send_set &   SM_KEYDIST_FLAG_IDENTITY_ADDRESS_INFORMATION){
                sm_key_distribution_send_set &= ~SM_KEYDIST_FLAG_IDENTITY_ADDRESS_INFORMATION;
                uint8_t buffer[8];
                buffer[0] = SM_CODE_IDENTITY_ADDRESS_INFORMATION;
                buffer[1] = sm_s_addr_type;
                bt_flip_addr(&buffer[2], sm_s_address);
                l2cap_send_connectionless(sm_response_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                sm_timeout_reset();
                return;
            }
            if (sm_key_distribution_send_set &   SM_KEYDIST_FLAG_SIGNING_IDENTIFICATION){
                sm_key_distribution_send_set &= ~SM_KEYDIST_FLAG_SIGNING_IDENTIFICATION;
                uint8_t buffer[17];
                buffer[0] = SM_CODE_SIGNING_INFORMATION;
                swap128(sm_s_csrk, &buffer[1]);
                l2cap_send_connectionless(sm_response_handle, L2CAP_CID_SECURITY_MANAGER_PROTOCOL, (uint8_t*) buffer, sizeof(buffer));
                sm_timeout_reset();
                return;
            }
            sm_timeout_stop();
            sm_state_responding = SM_STATE_IDLE; 
            break;

        default:
            break;
    }
}

static void sm_pdu_received_in_wrong_state(){
    sm_pairing_failed_reason = SM_REASON_UNSPECIFIED_REASON;
    sm_state_responding = SM_STATE_SEND_PAIRING_FAILED;
}

static void sm_packet_handler(uint8_t packet_type, uint16_t handle, uint8_t *packet, uint16_t size){

    if (packet_type != SM_DATA_PACKET) return;

    if (handle != sm_response_handle){
        printf("sm_packet_handler: packet from handle %u, but expecting from %u\n", handle, sm_response_handle);
        return;
    }

    // printf("sm_packet_handler, request %0x\n", packet[0]);

    // a sm timeout requries a new physical connection
    if (sm_state_responding == SM_STATE_TIMEOUT) return;

    switch (packet[0]){
        case SM_CODE_PAIRING_REQUEST: {

            if (sm_state_responding != SM_STATE_IDLE) {
                sm_pdu_received_in_wrong_state();
                break;;
            }

            // store key distribtion request
            sm_m_io_capabilities = packet[1];
            sm_m_have_oob_data = packet[2];
            sm_m_auth_req = packet[3];
            sm_m_max_encryption_key_size = packet[4];

            // assert max encryption size above our minimum
            if (sm_m_max_encryption_key_size < sm_min_encryption_key_size){
                sm_pairing_failed_reason = SM_REASON_ENCRYPTION_KEY_SIZE;
                sm_state_responding = SM_STATE_SEND_PAIRING_FAILED;
                break;
            }

            // min{}
            sm_encryption_key_size = sm_max_encryption_key_size;
            if (sm_m_max_encryption_key_size < sm_max_encryption_key_size){
                sm_encryption_key_size = sm_m_max_encryption_key_size;
            }

            // setup key distribution
            sm_m_key_distribution = packet[5];
            sm_setup_key_distribution(packet[6]);

            // for validate
            memcpy(sm_m_preq, packet, 7);

            // start SM timeout
            sm_timeout_start();

            // decide on STK generation method
            sm_tk_setup();

            // check if STK generation method is acceptable by client
            int ok = 0;
            switch (sm_stk_generation_method){
                case JUST_WORKS:
                    ok = (sm_accepted_stk_generation_methods & SM_STK_GENERATION_METHOD_JUST_WORKS) != 0;
                    break;
                case PK_RESP_INPUT:
                case PK_INIT_INPUT:
                case OK_BOTH_INPUT:
                    ok = (sm_accepted_stk_generation_methods & SM_STK_GENERATION_METHOD_PASSKEY) != 0;
                    break;
                case OOB:
                    ok = (sm_accepted_stk_generation_methods & SM_STK_GENERATION_METHOD_OOB) != 0;
                    break;
            }
            if (!ok){
                sm_pairing_failed_reason = SM_REASON_AUTHENTHICATION_REQUIREMENTS;
                sm_state_responding = SM_STATE_SEND_PAIRING_FAILED;
                break;
            }

            // generate random number first, if we need to show passkey
            if (sm_stk_generation_method == PK_INIT_INPUT){
                sm_state_responding = SM_STATE_PH2_GET_RANDOM_TK;
                break;
            }

            sm_state_responding = SM_STATE_PH1_SEND_PAIRING_RESPONSE;
            break;
        }

        case  SM_CODE_PAIRING_CONFIRM:

            if (sm_state_responding != SM_STATE_PH1_W4_PAIRING_CONFIRM) {
                sm_pdu_received_in_wrong_state();
                break;
            }

            // received confirm value
            swap128(&packet[1], sm_m_confirm);

            // notify client to hide shown passkey
            if (sm_stk_generation_method == PK_INIT_INPUT){
                sm_notify_client(SM_PASSKEY_DISPLAY_CANCEL, sm_m_addr_type, sm_m_address, 0);
            }

            // handle user cancel pairing?
            if (sm_user_response == SM_USER_RESPONSE_DECLINE){
                sm_pairing_failed_reason = SM_REASON_PASSKEYT_ENTRY_FAILED;
                sm_state_responding = SM_STATE_SEND_PAIRING_FAILED;
                break;
            }

            // wait for user action?
            if (sm_user_response == SM_USER_RESPONSE_PENDING){
                sm_state_responding = SM_STATE_PH1_W4_USER_RESPONSE;
                break;
            }

            // calculate and send s_confirm
            sm_state_responding = SM_STATE_PH2_C1_GET_RANDOM_A;
            break;


        case SM_CODE_PAIRING_RANDOM:

            if (sm_state_responding != SM_STATE_PH2_W4_PAIRING_RANDOM) {
                sm_pdu_received_in_wrong_state();
                break;
            }

            // received random value
            swap128(&packet[1], sm_m_random);

            // use aes128 engine
            // calculate m_confirm using aes128 engine - step 1
            sm_aes128_set_key(sm_tk);
            sm_c1_t1(sm_m_random, sm_m_preq, sm_s_pres, sm_m_addr_type, sm_s_addr_type, sm_aes128_plaintext);
            sm_state_responding = SM_STATE_PH2_C1_GET_ENC_C;
            break;

        case SM_CODE_ENCRYPTION_INFORMATION:
            if ((sm_state_responding != SM_STATE_DISTRIBUTE_KEYS) || ((sm_m_key_distribution & SM_KEYDIST_ENC_KEY) == 0)){
                sm_pdu_received_in_wrong_state();
                break;
            }
            sm_key_distribution_received_set |= SM_KEYDIST_FLAG_ENCRYPTION_INFORMATION;
            swap128(&packet[1], sm_m_ltk);
            break;

        case SM_CODE_MASTER_IDENTIFICATION:
            if ((sm_state_responding != SM_STATE_DISTRIBUTE_KEYS) || ((sm_m_key_distribution & SM_KEYDIST_ENC_KEY) == 0)){
                sm_pdu_received_in_wrong_state();
                break;
            }
            sm_key_distribution_received_set |= SM_KEYDIST_FLAG_MASTER_IDENTIFICATION;
            sm_m_ediv = READ_BT_16(packet, 1);
            swap64(&packet[3], sm_m_rand);
            break;

        case SM_CODE_IDENTITY_INFORMATION:
            if ((sm_state_responding != SM_STATE_DISTRIBUTE_KEYS) || ((sm_m_key_distribution & SM_KEYDIST_ID_KEY) == 0)){
                sm_pdu_received_in_wrong_state();
                break;
            }
            sm_key_distribution_received_set |= SM_KEYDIST_FLAG_IDENTITY_INFORMATION;
            swap128(&packet[1], sm_m_irk);
            break;

        case SM_CODE_IDENTITY_ADDRESS_INFORMATION:
            if ((sm_state_responding != SM_STATE_DISTRIBUTE_KEYS) || ((sm_m_key_distribution & SM_KEYDIST_ID_KEY) == 0)){
                sm_pdu_received_in_wrong_state();
                break;
            }
            sm_key_distribution_received_set |= SM_KEYDIST_FLAG_IDENTITY_ADDRESS_INFORMATION;
            sm_m_addr_type = packet[1];
            BD_ADDR_COPY(sm_m_address, &packet[2]); 
            break;

        case SM_CODE_SIGNING_INFORMATION:
            if ((sm_state_responding != SM_STATE_DISTRIBUTE_KEYS) || ((sm_m_key_distribution & SM_KEYDIST_SIGN) == 0)){
                sm_pdu_received_in_wrong_state();
                break;
            }
            sm_key_distribution_received_set |= SM_KEYDIST_FLAG_SIGNING_IDENTIFICATION;
            swap128(&packet[1], sm_m_csrk);

            // store, if: it's a public address, or, we got an IRK
            if (sm_m_addr_type == 0 || (sm_key_distribution_received_set & SM_KEYDIST_FLAG_IDENTITY_INFORMATION)) {
                central_device_db_add(sm_m_addr_type, sm_m_address, sm_m_irk, sm_m_csrk);
                break;
            } 
            break;
    }

    // try to send preparared packet
    sm_run();
}

void sm_set_er(key_t er){
    memcpy(sm_persistent_er, er, 16);
}

void sm_set_ir(key_t ir){
    memcpy(sm_persistent_ir, ir, 16);
    // sm_dhk(sm_persistent_ir, sm_persistent_dhk);
    // sm_irk(sm_persistent_ir, sm_persistent_irk);
}

void sm_init(){
    // set some (BTstack default) ER and IR
    int i;
    key_t er;
    key_t ir;
    for (i=0;i<16;i++){
        er[i] = 0x30 + i;
        ir[i] = 0x90 + i;
    }
    sm_set_er(er);
    sm_set_ir(ir);
    sm_state_responding = SM_STATE_IDLE;
    // defaults
    sm_accepted_stk_generation_methods = SM_STK_GENERATION_METHOD_JUST_WORKS
                                       | SM_STK_GENERATION_METHOD_OOB
                                       | SM_STK_GENERATION_METHOD_PASSKEY;
    sm_max_encryption_key_size = 16;
    sm_min_encryption_key_size = 7;
    sm_aes128_active = 0;

    gap_random_adress_update_period = 15 * 60 * 1000;
}

// END OF SM

// enable LE, setup ADV data
static void att_run(void);
static void packet_handler (void * connection, uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){
    uint8_t adv_data[] = { 02, 01, 05,   03, 02, 0xf0, 0xff }; 

    sm_run();

    switch (packet_type) {
            
		case HCI_EVENT_PACKET:
			switch (packet[0]) {
				
                case BTSTACK_EVENT_STATE:
					// bt stack activated, get started
					if (packet[2] == HCI_STATE_WORKING) {
                        printf("HCI Working!\n");
                        dkg_state = DKG_CALC_IRK;
					}
					break;
                
                case DAEMON_EVENT_HCI_PACKET_SENT:
                    att_run();
                    break;
                    
                case HCI_EVENT_LE_META:
                    switch (packet[2]) {
                        case HCI_SUBEVENT_LE_CONNECTION_COMPLETE:
                            // only single connection for peripheral
                            if (sm_response_handle){
                                printf("Already connected, ignoring incoming connection\n");
                                return;
                            }

                            sm_response_handle = READ_BT_16(packet, 4);
                            sm_m_addr_type = packet[7];
                            bt_flip_addr(sm_m_address, &packet[8]);
                            sm_reset_tk();
                            // TODO support private addresses
                            sm_s_addr_type = 0;
                            BD_ADDR_COPY(sm_s_address, hci_local_bd_addr());
                            printf("Incoming connection, own address ");
                            print_bd_addr(sm_s_address);

                            // reset connection MTU
                            att_connection.mtu = 23;

                            // request security
                            if (sm_s_request_security){
                                sm_state_responding = SM_STATE_SEND_SECURITY_REQUEST;
                            }
                            break;

                        case HCI_SUBEVENT_LE_LONG_TERM_KEY_REQUEST:
                            log_info("LTK Request: state %u", sm_state_responding);
                            if (sm_state_responding == SM_STATE_PH2_W4_LTK_REQUEST){
                                // calculate STK
                                log_info("LTK Request: calculating STK");
                                sm_aes128_set_key(sm_tk);
                                sm_s1_r_prime(sm_s_random, sm_m_random, sm_aes128_plaintext);
                                sm_state_responding = SM_STATE_PH2_CALC_STK;
                                break;
                            }

                            // re-establish previously used LTK using Rand and EDIV
                            swap64(&packet[5], sm_s_rand);
                            sm_s_ediv = READ_BT_16(packet, 13);

                            // assume that we don't have a LTK for ediv == 0 and random == null
                            if (sm_s_ediv == 0 && sm_is_null_random(sm_s_rand)){
                                printf("LTK Request: ediv & random are empty\n");
                                sm_state_responding = SM_STATE_SEND_LTK_REQUESTED_NEGATIVE_REPLY;
                                break;
                            }

                            // re-establish used key encryption size
                            if (sm_max_encryption_key_size == sm_min_encryption_key_size){
                                sm_encryption_key_size = sm_max_encryption_key_size;
                            } else {
                                // no db for encryption size hack: encryption size is stored in lowest nibble of sm_s_rand
                                sm_encryption_key_size = (sm_s_rand[7] & 0x0f) + 1;
                            }

                            log_info("LTK Request: recalculating with ediv 0x%04x", sm_s_ediv);

                            // dhk = d1(IR, 3, 0) - enc
                            // y   = dm(dhk, rand) - enc
                            // div = y xor ediv
                            // ltk = d1(ER, div, 0) - enc

                            // Y = dm(DHK, Rand)
                            sm_aes128_set_key(sm_persistent_dhk);
                            sm_dm_r_prime(sm_s_rand, sm_aes128_plaintext);
                            sm_state_responding = SM_STATE_PH4_Y_GET_ENC;

                            // sm_s_div = sm_div(sm_persistent_dhk, sm_s_rand, sm_s_ediv);
                            // sm_s_ltk(sm_persistent_er, sm_s_div, sm_s_ltk);
                            break;

                        default:
                            break;
                    }
                    break;

                case HCI_EVENT_ENCRYPTION_CHANGE: 
                    log_info("Connection encrypted");
                    if (sm_state_responding == SM_STATE_PH2_W4_CONNECTION_ENCRYPTED) {
                        sm_state_responding = SM_STATE_PH3_GET_RANDOM;
                    }
                    break;

                case HCI_EVENT_DISCONNECTION_COMPLETE:
                    // restart advertising if we have been connected before
                    // -> avoid sending advertise enable a second time before command complete was received 
                    if (att_request_handle) {
                        hci_send_cmd(&hci_le_set_advertise_enable, 1);
                    }

                    att_server_state = ATT_SERVER_IDLE;

                    sm_response_handle = 0;
                    sm_state_responding = SM_STATE_IDLE;
                    break;
                    
				case HCI_EVENT_COMMAND_COMPLETE:
				    if (COMMAND_COMPLETE_EVENT(packet, hci_le_set_advertising_parameters)){
                        // only needed for BLE Peripheral
                        hci_send_cmd(&hci_le_set_advertising_data, sizeof(adv_data), adv_data);
                        break;
					}
				    if (COMMAND_COMPLETE_EVENT(packet, hci_le_set_advertising_data)){
                        // only needed for BLE Peripheral
					   hci_send_cmd(&hci_le_set_scan_response_data, 10, adv_data);
					   break;
					}
				    if (COMMAND_COMPLETE_EVENT(packet, hci_le_set_scan_response_data)){
                        // only needed for BLE Peripheral
					   hci_send_cmd(&hci_le_set_advertise_enable, 1);
					   break;
					}
                    if (COMMAND_COMPLETE_EVENT(packet, hci_le_encrypt)){
                        sm_aes128_active = 0;
                        switch (dkg_state){
                            case DKG_W4_IRK:
                                swap128(&packet[6], sm_persistent_irk);
                                print_key("irk", sm_persistent_irk);
                                dkg_state++;
                                break;
                            case DKG_W4_DHK:
                                swap128(&packet[6], sm_persistent_dhk);
                                print_key("dhk", sm_persistent_dhk);
                                dkg_state ++;

                                // SM INIT FINISHED, start application code - TODO untangle that
                                printf("SM Init completed\n");
                                hci_send_cmd(&hci_le_set_advertising_data, sizeof(adv_data), adv_data);

// {
//                                 key_t k = { 0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c };
//                                 sm_cmac_start(k, sizeof(m64), m64);
// }

                                break;
                            default:
                                break;
                        }
                        switch (rau_state){
                            case RAU_W4_ENC:
                                swap24(&packet[19], &sm_random_address[3]);
                                rau_state++;
                                break;
                            default:
                                break;
                        }
                        switch (sm_cmac_state){
                            case CMAC_W4_SUBKEYS:
                            case CMAC_W4_MI:
                            case CMAC_W4_MLAST:
                                {
                                key_t t;
                                swap128(&packet[6], t);
                                sm_cmac_handle_encryption_result(t);
                                }
                                break;
                            default:
                                break;
                        }
                        switch (sm_state_responding){
                            case SM_STATE_PH2_C1_W4_ENC_A:
                            case SM_STATE_PH2_C1_W4_ENC_C:
                                {
                                sm_aes128_set_key(sm_tk);
                                key_t t2;
                                swap128(&packet[6], t2);
                                sm_c1_t3(t2, sm_m_address, sm_s_address, sm_aes128_plaintext);
                                }
                                sm_state_responding++;
                                break;
                            case SM_STATE_PH2_C1_W4_ENC_B:
                                swap128(&packet[6], sm_s_confirm);
                                print_key("c1!", sm_s_confirm);
                                sm_state_responding++;
                                break;
                            case SM_STATE_PH2_C1_W4_ENC_D:
                                {
                                key_t m_confirm_test;
                                swap128(&packet[6], m_confirm_test);
                                print_key("c1!", m_confirm_test);
                                if (memcmp(sm_m_confirm, m_confirm_test, 16) == 0){
                                    // send s_random
                                    sm_state_responding = SM_STATE_SEND_PAIRING_RANDOM;
                                    break;
                                }
                                sm_pairing_failed_reason = SM_REASON_CONFIRM_VALUE_FAILED;
                                sm_state_responding = SM_STATE_SEND_PAIRING_FAILED;
                                }
                                break;
                            case SM_STATE_PH2_W4_STK:
                                swap128(&packet[6], sm_s_ltk);
                                sm_truncate_key(sm_s_ltk, sm_encryption_key_size);
                                print_key("stk", sm_s_ltk);
                                sm_state_responding = SM_STATE_PH2_SEND_STK;
                                break;
                            case SM_STATE_PH3_Y_W4_ENC:{
                                key_t y128;
                                swap128(&packet[6], y128);
                                sm_s_y = READ_NET_16(y128, 14);
                                print_hex16("y", sm_s_y);
                                // PH3B3 - calculate EDIV
                                sm_s_ediv = sm_s_y ^ sm_s_div;
                                print_hex16("ediv", sm_s_ediv);
                                // PH3B4 - calculate LTK         - enc
                                // LTK = d1(ER, DIV, 0))
                                sm_aes128_set_key(sm_persistent_er);
                                sm_d1_d_prime(sm_s_div, 0, sm_aes128_plaintext);
                                sm_state_responding = SM_STATE_PH3_LTK_GET_ENC;
                                break;
                            }
                            case SM_STATE_PH4_Y_W4_ENC:{
                                key_t y128;
                                swap128(&packet[6], y128);
                                sm_s_y = READ_NET_16(y128, 14);
                                print_hex16("y", sm_s_y);
                                // PH3B3 - calculate DIV
                                sm_s_div = sm_s_y ^ sm_s_ediv;
                                print_hex16("ediv", sm_s_ediv);
                                // PH3B4 - calculate LTK         - enc
                                // LTK = d1(ER, DIV, 0))
                                sm_aes128_set_key(sm_persistent_er);
                                sm_d1_d_prime(sm_s_div, 0, sm_aes128_plaintext);
                                sm_state_responding = SM_STATE_PH4_LTK_GET_ENC;
                                break;
                            }
                            case SM_STATE_PH3_LTK_W4_ENC:
                                swap128(&packet[6], sm_s_ltk);
                                print_key("ltk", sm_s_ltk);
                                // distribute keys
                                sm_state_responding = SM_STATE_DISTRIBUTE_KEYS;
                                break;                                
                            case SM_STATE_PH4_LTK_W4_ENC:
                                swap128(&packet[6], sm_s_ltk);
                                sm_truncate_key(sm_s_ltk, sm_encryption_key_size);
                                print_key("ltk", sm_s_ltk);
                                sm_state_responding = SM_STATE_PH4_SEND_LTK;
                                break;                                
                            default:
                                break;
                        }
                    }
                    if (COMMAND_COMPLETE_EVENT(packet, hci_le_rand)){
                        switch (rau_state){
                            case RAU_W4_RANDOM:
                                // non-resolvable vs. resolvable
                                switch (gap_random_adress_type){
                                    case GAP_RANDOM_ADDRESS_RESOLVABLE:
                                        // resolvable: use random as prand and calc address hash
                                        // "The two most significant bits of prand shall be equal to ‘0’ and ‘1"
                                        memcpy(sm_random_address, &packet[6], 3);
                                        sm_random_address[0] &= 0x3f;
                                        sm_random_address[0] |= 0x40;
                                        rau_state = RAU_GET_ENC;
                                        break;
                                    case GAP_RANDOM_ADDRESS_NON_RESOLVABLE:
                                    default:
                                        // "The two most significant bits of the address shall be equal to ‘0’""
                                        memcpy(sm_random_address, &packet[6], 6);
                                        sm_random_address[0] &= 0x3f; 
                                        rau_state = RAU_SET_ADDRESS;
                                        break;
                                }
                                break;
                            default:
                                break;
                        }
                        switch (sm_state_responding){
                            case SM_STATE_PH2_W4_RANDOM_TK:
                            {
                                // map random to 0-999999 without speding much cycles on a modulus operation
                                uint32_t tk = * (uint32_t*) &packet[6]; // random endianess
                                tk = tk & 0xfffff;  // 1048575
                                if (tk >= 999999){
                                    tk = tk - 999999;
                                } 
                                sm_reset_tk();
                                net_store_32(sm_tk, 12, tk);
                                // continue with phase 1
                                sm_state_responding = SM_STATE_PH1_SEND_PAIRING_RESPONSE;
                                break;
                            }
                            case SM_STATE_PH2_C1_W4_RANDOM_A:

                                memcpy(&sm_s_random[0], &packet[6], 8); // random endinaness
                                sm_state_responding = SM_STATE_PH2_C1_GET_RANDOM_B;
                                break;
                            case SM_STATE_PH2_C1_W4_RANDOM_B:
                                memcpy(&sm_s_random[8], &packet[6], 8); // random endinaness

                                // calculate s_confirm manually
                                // sm_c1(sm_tk, sm_s_random, sm_m_preq, sm_s_pres, sm_m_addr_type, sm_s_addr_type, sm_m_address, sm_s_address, sm_s_confirm);

                                // calculate s_confirm using aes128 engine - step 1
                                sm_aes128_set_key(sm_tk);
                                sm_c1_t1(sm_s_random, sm_m_preq, sm_s_pres, sm_m_addr_type, sm_s_addr_type, sm_aes128_plaintext);
                                sm_state_responding = SM_STATE_PH2_C1_GET_ENC_A;
                                break;

                            case SM_STATE_PH3_W4_RANDOM:
                                swap64(&packet[6], sm_s_rand);
                                // no db for encryption size hack: encryption size is stored in lowest nibble of sm_s_rand
                                sm_s_rand[7] =  (sm_s_rand[7] & 0xf0) + (sm_encryption_key_size - 1);
                                sm_state_responding = SM_STATE_PH3_GET_DIV;
                                break;
                            case SM_STATE_PH3_W4_DIV:
                                // use 16 bit from random value as div
                                sm_s_div = READ_NET_16(packet, 6);
                                print_hex16("div", sm_s_div);

                                // PLAN
                                // PH3B1 - calculate DHK from IR - enc
                                // PH3B2 - calculate Y from      - enc
                                // PH3B3 - calculate EDIV
                                // PH3B4 - calculate LTK         - enc

                                // skip PH3B1 - we got DHK during startup
                                // PH3B2 - calculate Y from      - enc
                                // Y = dm(DHK, Rand)
                                sm_aes128_set_key(sm_persistent_dhk);
                                sm_dm_r_prime(sm_s_rand, sm_aes128_plaintext);
                                sm_state_responding = SM_STATE_PH3_Y_GET_ENC;

                                // // calculate EDIV and LTK
                                // sm_s_ediv = sm_ediv(sm_persistent_dhk, sm_s_rand, sm_s_div);
                                // sm_s_ltk(sm_persistent_er, sm_s_div, sm_s_ltk);
                                // print_key("ltk", sm_s_ltk);
                                // print_hex16("ediv", sm_s_ediv);
                                // // distribute keys
                                // sm_distribute_keys();
                                // // done
                                // sm_state_responding = SM_STATE_IDLE;
                                break;

                            default:
                                break;
                        }
                        break;
                    }
			}
	}

    sm_run();
}

// GAP LE API
void gap_random_address_set_mode(gap_random_address_type_t random_address_type){
    gap_random_address_update_stop();
    gap_random_adress_type = random_address_type;
    if (random_address_type == GAP_RANDOM_ADDRESS_TYPE_OFF) return;
    gap_random_address_update_start();
}

void gap_random_address_set_update_period(int period_ms){
    gap_random_adress_update_period = period_ms;
    if (gap_random_adress_type == GAP_RANDOM_ADDRESS_TYPE_OFF) return;
    gap_random_address_update_stop();
    gap_random_address_update_start();
}

// Security Manager Client API
void sm_register_oob_data_callback( int (*get_oob_data_callback)(uint8_t addres_type, bd_addr_t * addr, uint8_t * oob_data)){
    sm_get_oob_data = get_oob_data_callback;
}

void sm_register_packet_handler(btstack_packet_handler_t handler){
    sm_client_packet_handler = handler;    
}

void sm_set_accepted_stk_generation_methods(uint8_t accepted_stk_generation_methods){
    sm_accepted_stk_generation_methods = accepted_stk_generation_methods;
}

void sm_set_max_encrypted_key_size(uint8_t size) {
    sm_max_encryption_key_size = size;
}

void sm_set_min_encrypted_key_size(uint8_t size) {
    sm_min_encryption_key_size = size;
}

void sm_set_authentication_requirements(uint8_t auth_req){
    sm_s_auth_req = auth_req;
}

void sm_set_io_capabilities(io_capability_t io_capability){
    sm_s_io_capabilities = io_capability;
}

void sm_set_request_security(int enable){
    sm_s_request_security = enable;
}

int sm_get_connection(uint8_t addr_type, bd_addr_t address){
    // TODO compare to current connection
    return 1;
}

void sm_bonding_decline(uint8_t addr_type, bd_addr_t address){
    if (!sm_get_connection(addr_type, address)) return; // wrong connection
    sm_user_response = SM_USER_RESPONSE_DECLINE;

    if (sm_state_responding == SM_STATE_PH1_W4_USER_RESPONSE){
        sm_pairing_failed_reason = SM_REASON_PASSKEYT_ENTRY_FAILED;
        sm_state_responding = SM_STATE_SEND_PAIRING_FAILED;
    }
    sm_run();
}

void sm_just_works_confirm(uint8_t addr_type, bd_addr_t address){
    if (!sm_get_connection(addr_type, address)) return; // wrong connection
    sm_user_response = SM_USER_RESPONSE_CONFIRM;
    if (sm_state_responding == SM_STATE_PH1_W4_USER_RESPONSE){
        sm_state_responding = SM_STATE_PH2_C1_GET_RANDOM_A;
    }
    sm_run();
}

void sm_passkey_input(uint8_t addr_type, bd_addr_t address, uint32_t passkey){
    if (!sm_get_connection(addr_type, address)) return; // wrong connection
    sm_reset_tk();
    net_store_32(sm_tk, 12, passkey);
    sm_user_response = SM_USER_RESPONSE_PASSKEY;
    if (sm_state_responding == SM_STATE_PH1_W4_USER_RESPONSE){
        sm_state_responding = SM_STATE_PH2_C1_GET_RANDOM_A;
    }
    sm_run();
}

// test profile
#include "profile.h"

static void att_run(void){
    switch (att_server_state){
        case ATT_SERVER_IDLE:
            return;
        case ATT_SERVER_REQUEST_RECEIVED:
            if (att_request_buffer[0] == ATT_SIGNED_WRITE_COMAND){
                printf("ATT_SIGNED_WRITE_COMAND not implemented yet\n");
                if (!sm_cmac_ready()) return;                                
                return;
            } 

            // any other request
            if (!hci_can_send_packet_now(HCI_ACL_DATA_PACKET)) return;

            uint8_t  att_response_buffer[28];
            uint16_t att_response_size = att_handle_request(&att_connection, att_request_buffer, att_request_size, att_response_buffer);
            att_server_state = ATT_SERVER_IDLE;
            if (att_response_size == 0) return;

            l2cap_send_connectionless(att_request_handle, L2CAP_CID_ATTRIBUTE_PROTOCOL, att_response_buffer, att_response_size);
            break;
        case ATT_SERVER_W4_SIGNED_WRITE_VALIDATION:
            // signed write doesn't have a response
            att_handle_request(&att_connection, att_request_buffer, att_request_size, NULL);
            att_server_state = ATT_SERVER_IDLE;
            break;
    }
}

static void att_packet_handler(uint8_t packet_type, uint16_t handle, uint8_t *packet, uint16_t size){
    if (packet_type != ATT_DATA_PACKET) return;

    // chcke size
    if (size > sizeof(att_request_buffer)) return;

    // last request still in processing?
    if (att_server_state != ATT_SERVER_IDLE) return;

    // store request
    att_server_state = ATT_SERVER_REQUEST_RECEIVED;
    att_request_size = size;
    att_request_handle = handle;
    memcpy(att_request_buffer, packet, size);

    att_run();
}

// write requests
static void att_write_callback(uint16_t handle, uint16_t transaction_mode, uint16_t offset, uint8_t *buffer, uint16_t buffer_size, signature_t * signature){
    printf("WRITE Callback, handle %04x\n", handle);
    switch(handle){
        case 0x000b:
            buffer[buffer_size]=0;
            printf("New text: %s\n", buffer);
            break;
        case 0x000d:
            printf("New value: %u\n", buffer[0]);
            break;
    }
}

void setup(void){
    /// GET STARTED with BTstack ///
    btstack_memory_init();
    run_loop_init(RUN_LOOP_POSIX);
        
    // use logger: format HCI_DUMP_PACKETLOGGER, HCI_DUMP_BLUEZ or HCI_DUMP_STDOUT
    hci_dump_open("/tmp/hci_dump.pklg", HCI_DUMP_PACKETLOGGER);

    // init HCI
    hci_transport_t    * transport = hci_transport_usb_instance();
    hci_uart_config_t  * config    = NULL;
    bt_control_t       * control   = NULL;
    remote_device_db_t * remote_db = (remote_device_db_t *) &remote_device_db_memory;
        
    hci_init(transport, config, control, remote_db);

    // set up l2cap_le
    l2cap_init();
    l2cap_register_fixed_channel(att_packet_handler, L2CAP_CID_ATTRIBUTE_PROTOCOL);
    l2cap_register_fixed_channel(sm_packet_handler, L2CAP_CID_SECURITY_MANAGER_PROTOCOL);
    l2cap_register_packet_handler(packet_handler);
    
    // set up ATT
    att_server_state = ATT_SERVER_IDLE;
    att_set_db(profile_data);
    att_set_write_callback(att_write_callback);
    att_dump_attributes();
    att_connection.mtu = 27;

    // setup SM
    sm_init();
    sm_set_io_capabilities(IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
    sm_set_authentication_requirements( SM_AUTHREQ_BONDING );
    sm_set_request_security(1);
}

int main(void)
{
    setup();
    // gap_random_address_set_update_period(5000);
    gap_random_address_set_mode(GAP_RANDOM_ADDRESS_RESOLVABLE);

    // turn on!
    hci_power_control(HCI_POWER_ON);
    
    // go!
    run_loop_execute(); 
    
    // happy compiler!
    return 0;
}
