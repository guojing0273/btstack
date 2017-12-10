/*
 * Copyright (C) 2017 BlueKitchen GmbH
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

#define __BTSTACK_FILE__ "provisioning_device.c"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ble/mesh/pb_adv.h"
#include "classic/rfcomm.h" // for crc8
#include "btstack.h"

#define PROVISIONING_PROTOCOL_TIMEOUT_MS 60000

// remote ecc
static uint8_t remote_ec_q[64];
static uint8_t dhkey[32];

// mesh k1 - might get moved to btstack_crypto and all vars go into btstack_crypto_mesh_k1_t struct
static uint8_t         mesh_k1_temp[16];
static void (*         mesh_k1_callback)(void * arg);
static void *          mesh_k1_arg;
static const uint8_t * mesh_k1_p;
static uint16_t        mesh_k1_p_len;
static uint8_t *       mesh_k1_result;

static void mesh_k1_temp_calculated(void * arg){
    btstack_crypto_aes128_cmac_t * request = (btstack_crypto_aes128_cmac_t*) arg;
    btstack_crypto_aes128_cmac_message(request, mesh_k1_temp, mesh_k1_p_len, mesh_k1_p, mesh_k1_result, mesh_k1_callback, mesh_k1_arg);
}

static void mesh_k1(btstack_crypto_aes128_cmac_t * request, const uint8_t * n, uint16_t n_len, const uint8_t * salt,
    const uint8_t * p, const uint16_t p_len, uint8_t * result, void (* callback)(void * arg), void * callback_arg){
    mesh_k1_callback = callback;
    mesh_k1_arg      = callback_arg;
    mesh_k1_p        = p;
    mesh_k1_p_len    = p_len;
    mesh_k1_result   = result;
    btstack_crypto_aes128_cmac_message(request, salt, n_len, n, mesh_k1_temp, mesh_k1_temp_calculated, request);
}

// Provisioning Bearer Control

#define MESH_PROV_INVITE            0x00
#define MESH_PROV_CAPABILITIES      0x01
#define MESH_PROV_START             0x02
#define MESH_PROV_PUB_KEY           0x03
#define MESH_PROV_INPUT_COMPLETE    0x04
#define MESH_PROV_CONFIRM           0x05
#define MESH_PROV_RANDOM            0x06
#define MESH_PROV_DATA              0x07
#define MESH_PROV_COMPLETE          0x08
#define MESH_PROV_FAILED            0x09

#define MESH_OUTPUT_OOB_BLINK       0x01
#define MESH_OUTPUT_OOB_BEEP        0x02
#define MESH_OUTPUT_OOB_VIBRATE     0x04
#define MESH_OUTPUT_OOB_NUMBER      0x08
#define MESH_OUTPUT_OOB_STRING      0x10

#define MESH_INPUT_OOB_PUSH         0x01
#define MESH_INPUT_OOB_TWIST        0x02
#define MESH_INPUT_OOB_NUMBER       0x04
#define MESH_INPUT_OOB_STRING       0x08


static uint8_t  prov_buffer_out[100];   // TODO: how large are prov messages?
// ConfirmationInputs = ProvisioningInvitePDUValue || ProvisioningCapabilitiesPDUValue || ProvisioningStartPDUValue || PublicKeyProvisioner || PublicKeyDevice
static uint8_t  prov_confirmation_inputs[1 + 11 + 5 + 64 + 64];
static uint8_t  prov_authentication_action;

static uint8_t  prov_ec_q[64];

#ifdef ENABLE_ATTENTION_TIMER
static btstack_timer_source_t       prov_attention_timer;
#endif

static btstack_timer_source_t       prov_protocol_timer;

static btstack_crypto_aes128_cmac_t prov_cmac_request;
static btstack_crypto_random_t      prov_random_request;
static btstack_crypto_ecc_p256_t    prov_ecc_p256_request;
static btstack_crypto_ccm_t         prov_ccm_request;

static void provisiong_timer_handler(btstack_timer_source_t * ts){
    UNUSED(ts);
    printf("provisiong_timer_handler!\n");
    // TODO: use actual pb_adv_cid
    pb_adv_close_link(1, 1);
}

static void provisiong_timer_start(void){
    btstack_run_loop_remove_timer(&prov_protocol_timer);
    btstack_run_loop_set_timer_handler(&prov_protocol_timer, &provisiong_timer_handler);
    btstack_run_loop_set_timer(&prov_protocol_timer, PROVISIONING_PROTOCOL_TIMEOUT_MS);
    btstack_run_loop_add_timer(&prov_protocol_timer);
}

static void provisioning_timer_stop(void){
    btstack_run_loop_remove_timer(&prov_protocol_timer);
}

static void provisioning_attention_timer_timeout(btstack_timer_source_t * ts){
    UNUSED(ts);
#ifdef ENABLE_ATTENTION_TIMER
    // TODO: check if provisioning complete, stop attention
#endif
}

static void provisioning_handle_invite(uint8_t *packet, uint16_t size){

    if (size != 1) return;

    // store for confirmation inputs: len 1
    memcpy(&prov_confirmation_inputs[0], packet, 1);

    // handle invite message
#ifdef ENABLE_ATTENTION_TIMER
    uint32_t attention_timer_timeout_ms = packet[0] * 1000;
    btstack_run_loop_set_timer_handler(&prov_attention_timer, &provisioning_attention_timer_timeout);
    btstack_run_loop_set_timer(&prov_attention_timer, attention_timer_timeout_ms);
    btstack_run_loop_add_timer(&prov_attention_timer);
#endif

    // setup response 
    prov_buffer_out[0] = MESH_PROV_CAPABILITIES;

    // TOOD: get actual number
    /* Number of Elements supported */
    prov_buffer_out[1] = 1;

    /* Supported algorithms - FIPS P-256 Eliptic Curve */
    big_endian_store_16(prov_buffer_out, 2, 1);

    /* Public Key Type - Public Key OOB information available */
    prov_buffer_out[4] = 0;

    /* Static OOB Type - Static OOB information available */
    prov_buffer_out[5] = 1; 

    /* Output OOB Size - max of 8 */
    prov_buffer_out[6] = 8; 

    /* Output OOB Action */
    big_endian_store_16(prov_buffer_out, 7, MESH_OUTPUT_OOB_NUMBER); //  | MESH_OUTPUT_OOB_STRING);

    /* Input OOB Size - max of 8*/
    prov_buffer_out[9] = 8; 

    /* Input OOB Action */
    big_endian_store_16(prov_buffer_out, 10, MESH_INPUT_OOB_STRING | MESH_OUTPUT_OOB_NUMBER);

    // store for confirmation inputs: len 11
    memcpy(&prov_confirmation_inputs[1], &prov_buffer_out[1], 11);

    // send
    provisiong_timer_start();
    pb_adv_send_pdu(prov_buffer_out, 12);
}

static void provisioning_handle_start(uint8_t * packet, uint16_t size){

    if (size != 5) return;

    // store for confirmation inputs: len 5
    memcpy(&prov_confirmation_inputs[12], packet, 5);

    // output authentication action
    prov_authentication_action = packet[3];
}

static void provisioning_handle_public_key_dhkey(void * arg){
    UNUSED(arg);

    printf("DHKEY: ");
    printf_hexdump(dhkey, sizeof(dhkey));

    // setup response 
    prov_buffer_out[0] = MESH_PROV_PUB_KEY;
    memcpy(&prov_buffer_out[1], prov_ec_q, 64);

    // store for confirmation inputs: len 64
    memcpy(&prov_confirmation_inputs[81], &prov_buffer_out[1], 64);

    // send
    provisiong_timer_start();
    pb_adv_send_pdu(prov_buffer_out, 65);
}

static void provisioning_handle_public_key(uint8_t *packet, uint16_t size){

    if (size != sizeof(remote_ec_q)) return;

    // store for confirmation inputs: len 64
    memcpy(&prov_confirmation_inputs[17], packet, 64);

    // store remote q
    memcpy(remote_ec_q, packet, sizeof(remote_ec_q));

    // calculate DHKey
    btstack_crypto_ecc_p256_calculate_dhkey(&prov_ecc_p256_request, remote_ec_q, dhkey, provisioning_handle_public_key_dhkey, NULL);
}

// ConfirmationDevice
static uint8_t confirmation_device[16];
// ConfirmationSalt
static uint8_t confirmation_salt[16];
// ConfirmationKey
static uint8_t confirmation_key[16];
// RandomDevice
static uint8_t random_device[16];
// ProvisioningSalt
static uint8_t provisioning_salt[16];
// AuthValue
static uint8_t auth_value[16];
// SessionKey
static uint8_t session_key[16];
// SessionNonce
static uint8_t session_nonce[16];
// EncProvisioningData
static uint8_t enc_provisioning_data[25];
// ProvisioningData
static uint8_t provisioning_data[25];
// DeviceKey
static uint8_t device_key[16];
// NetKey
static uint8_t  net_key[16];
// NetKeyIndex
static uint16_t net_key_index;

static uint8_t  flags;

static uint32_t iv_index;
static uint16_t unicast_address;

static void provisioning_handle_confirmation_device_calculated(void * arg){

    UNUSED(arg);

    printf("ConfirmationDevice: ");
    printf_hexdump(confirmation_device, sizeof(confirmation_device));

    // setup response 
    prov_buffer_out[0] = MESH_PROV_CONFIRM;
    memcpy(&prov_buffer_out[1], confirmation_device, 16);

    // send
    provisiong_timer_start();
    pb_adv_send_pdu(prov_buffer_out, 17);
}

static void provisioning_handle_confirmation_random_device(void * arg){
    // re-use prov_confirmation_inputs buffer
    memcpy(&prov_confirmation_inputs[0],  random_device, 16);
    memcpy(&prov_confirmation_inputs[16], auth_value, 16);

    // calc confirmation device
    btstack_crypto_aes128_cmac_message(&prov_cmac_request, confirmation_key, 32, prov_confirmation_inputs, confirmation_device, &provisioning_handle_confirmation_device_calculated, NULL);
}

static void provisioning_handle_confirmation_random_auth(void * arg){

    // limit auth value to single digit
    auth_value[15] = auth_value[15] % 9 + 1;

    // output auth value
    printf("AuthAction: %02x\n", prov_authentication_action);
    printf("AuthValue:  '%u'\n", auth_value[15]);

    // generate random_device
    btstack_crypto_random_generate(&prov_random_request,random_device, 16, &provisioning_handle_confirmation_random_device, NULL);
}

static void provisioning_handle_confirmation_k1_calculated(void * arg){
    printf("ConfirmationKey:   ");
    printf_hexdump(confirmation_key, sizeof(confirmation_key));

    // auth_value
    memset(auth_value, 0, sizeof(auth_value));

    // generate single byte of random data to use for authentication
    btstack_crypto_random_generate(&prov_random_request, &auth_value[15], 1, &provisioning_handle_confirmation_random_auth, NULL);
}

static void provisioning_handle_confirmation_s1_calculated(void * arg){

    UNUSED(arg);

    // ConfirmationSalt
    printf("ConfirmationSalt:   ");
    printf_hexdump(confirmation_salt, sizeof(confirmation_salt));

    // ConfirmationKey
    mesh_k1(&prov_cmac_request, dhkey, sizeof(dhkey), confirmation_salt, (const uint8_t*) "prck", 4, confirmation_key, &provisioning_handle_confirmation_k1_calculated, NULL);
}

static void provisioning_handle_confirmation(uint8_t *packet, uint16_t size){

    UNUSED(size);
    UNUSED(packet);

    // CalculationInputs
    printf("ConfirmationInputs: ");
    printf_hexdump(prov_confirmation_inputs, sizeof(prov_confirmation_inputs));
    btstack_crypto_aes128_cmac_zero(&prov_cmac_request, sizeof(prov_confirmation_inputs), prov_confirmation_inputs, confirmation_salt, &provisioning_handle_confirmation_s1_calculated, NULL);
}

static void provisioning_send_random(void  *arg){

    UNUSED(arg);

    // setup response 
    prov_buffer_out[0] = MESH_PROV_RANDOM;
    memcpy(&prov_buffer_out[1],  random_device, 16);

    // send pdu
    provisiong_timer_start();
    pb_adv_send_pdu(prov_buffer_out, 17);
}

static void provisioning_handle_random_session_nonce_calculated(void * arg){
    UNUSED(arg);

    // The nonce shall be the 13 least significant octets == zero most significant octets
    memset(session_nonce, 0, 3);

    // SessionNonce
    printf("SessionNonce:   ");
    printf_hexdump(session_nonce, sizeof(session_nonce));

    // finally respond with our random
    provisioning_send_random(NULL);
}

static void provisioning_handle_random_session_key_calculated(void * arg){
    UNUSED(arg);

    // SessionKey
    printf("SessionKey:   ");
    printf_hexdump(session_key, sizeof(session_key));

    // SessionNonce
    mesh_k1(&prov_cmac_request, dhkey, sizeof(dhkey), provisioning_salt, (const uint8_t*) "prsn", 4, session_nonce, &provisioning_handle_random_session_nonce_calculated, NULL);
}

static void provisioning_handle_random_s1_calculated(void * arg){

    UNUSED(arg);
    
    // ProvisioningSalt
    printf("ProvisioningSalt:   ");
    printf_hexdump(provisioning_salt, sizeof(provisioning_salt));

    // SessionKey
    mesh_k1(&prov_cmac_request, dhkey, sizeof(dhkey), provisioning_salt, (const uint8_t*) "prsk", 4, session_key, &provisioning_handle_random_session_key_calculated, NULL);
}

static void provisioning_handle_random(uint8_t *packet, uint16_t size){

    UNUSED(size);
    UNUSED(packet);

    // TODO: validate Confirmation

    // calc ProvisioningSalt = s1(ConfirmationSalt || RandomProvisioner || RandomDevice)
    memcpy(prov_confirmation_inputs, confirmation_salt, 16);
    memcpy(prov_confirmation_inputs, packet, 16);
    memcpy(prov_confirmation_inputs, random_device, 16);
    btstack_crypto_aes128_cmac_zero(&prov_cmac_request, sizeof(prov_confirmation_inputs), prov_confirmation_inputs, provisioning_salt, &provisioning_handle_random_s1_calculated, NULL);
}

static void provisioning_handle_data_device_key(void * arg){
    // send response
    prov_buffer_out[0] = MESH_PROV_COMPLETE;
    provisioning_timer_stop();
    pb_adv_send_pdu(prov_buffer_out, 1);
}

static void provisioning_handle_data_ccm(void * arg){

    UNUSED(arg);

    // validate MIC?

    // sort provisoning data
    memcpy(net_key, provisioning_data, 16);
    net_key_index = big_endian_read_16(provisioning_data, 16);
    flags = provisioning_data[18];
    iv_index = big_endian_read_32(provisioning_data, 19);
    unicast_address = big_endian_read_16(provisioning_data, 23);

    // dump
    printf("NetKey: ");
    printf_hexdump(provisioning_data, 16);
    printf("NetKeyIndex: %04x\n", net_key_index);
    printf("Flags: %02x\n", flags);
    printf("IVIndex: %04x\n", iv_index);
    printf("UnicastAddress: %02x\n", unicast_address);

    // DeviceKey
    mesh_k1(&prov_cmac_request, dhkey, sizeof(dhkey), provisioning_salt, (const uint8_t*) "prdk", 4, device_key, &provisioning_handle_data_device_key, NULL);
}

static void provisioning_handle_data(uint8_t *packet, uint16_t size){

    UNUSED(size);

    memcpy(enc_provisioning_data, packet, 25);

    // decode response
    btstack_crypo_ccm_init(&prov_ccm_request, session_key, session_nonce, 25);
    btstack_crypto_ccm_decrypt_block(&prov_ccm_request, 25, enc_provisioning_data, provisioning_data, &provisioning_handle_data_ccm, NULL);
}

static void provisioning_handle_pdu(uint8_t packet_type, uint16_t channel, uint8_t *packet, uint16_t size){

    if (size < 1) return;

    switch (packet_type){
        case HCI_EVENT_PACKET:
            break;
        case PROVISIONING_DATA_PACKET:
            // dispatch msg
            switch (packet[0]){
                case MESH_PROV_INVITE:
                    printf("MESH_PROV_INVITE: ");
                    printf_hexdump(packet, size);
                    provisioning_handle_invite(&packet[1], size-1);
                    break;
                case MESH_PROV_START:
                    printf("MESH_PROV_START:  ");
                    printf_hexdump(&packet[1], size-1);
                    provisioning_handle_start(&packet[1], size-1);
                    break;
                case MESH_PROV_PUB_KEY:
                    printf("MESH_PROV_PUB_KEY: ");
                    printf_hexdump(&packet[1], size-1);
                    provisioning_handle_public_key(&packet[1], size-1);
                    break;
                case MESH_PROV_CONFIRM:
                    printf("MESH_PROV_CONFIRM: ");
                    printf_hexdump(&packet[1], size-1);
                    provisioning_handle_confirmation(&packet[1], size-1);
                    break;
                case MESH_PROV_RANDOM:
                    printf("MESH_PROV_RANDOM:  ");
                    printf_hexdump(&packet[1], size-1);
                    provisioning_handle_random(&packet[1], size-1);
                    break;
                case MESH_PROV_DATA:
                    printf("MESH_PROV_DATA:  ");
                    printf_hexdump(&packet[1], size-1);
                    provisioning_handle_data(&packet[1], size-1);
                    break;
                default:
                    printf("TODO: handle provisioning msg type %x\n", packet[0]);
                    printf_hexdump(&packet[1], size-1);
                    break;
            }            
            break;
        default:
            break;
    }
}

static void dump_data(uint8_t * buffer, uint16_t size){
    static int data_counter = 1;
    char var_name[80];
    sprintf(var_name, "test_data_%02u", data_counter);
    printf("uint8_t %s[] = { ", var_name);
    for (int i = 0; i < size ; i++){
        if ((i % 16) == 0) printf("\n    ");
        printf ("0x%02x, ", buffer[i]);
    }
    printf("};\n");
    data_counter++;
}

static void prov_key_generated(void * arg){
    UNUSED(arg);
    printf("ECC-P256: ");
    dump_data(prov_ec_q, sizeof(prov_ec_q));
}

void provisioning_device_init(const uint8_t * device_uuid){
    pb_adv_init(device_uuid);
    pb_adv_register_packet_handler(&provisioning_handle_pdu);

    // generaete public key
    btstack_crypto_ecc_p256_generate_key(&prov_ecc_p256_request, prov_ec_q, &prov_key_generated, NULL);
}