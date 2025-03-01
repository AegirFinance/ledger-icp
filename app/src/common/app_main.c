/*******************************************************************************
*   (c) 2018, 2019 Zondax GmbH
*   (c) 2016 Ledger
*
*  Licensed under the Apache License, Version 2.0 (the "License");
*  you may not use this file except in compliance with the License.
*  You may obtain a copy of the License at
*
*      http://www.apache.org/licenses/LICENSE-2.0
*
*  Unless required by applicable law or agreed to in writing, software
*  distributed under the License is distributed on an "AS IS" BASIS,
*  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
*  See the License for the specific language governing permissions and
*  limitations under the License.
********************************************************************************/

#include "app_main.h"

#include <string.h>
#include <os_io_seproxyhal.h>
#include <os.h>
#include <ux.h>

#include "view.h"
#include "actions.h"
#include "tx.h"
#include "crypto.h"
#include "coin.h"
#include "zxmacros.h"
#include "app_mode.h"

#include "parser_impl.h"

unsigned char G_io_seproxyhal_spi_buffer[IO_SEPROXYHAL_BUFFER_SIZE_B];

static bool tx_initialized = false;

unsigned char io_event(unsigned char channel) {
    UNUSED(channel);

    switch (G_io_seproxyhal_spi_buffer[0]) {
        case SEPROXYHAL_TAG_FINGER_EVENT: //
            UX_FINGER_EVENT(G_io_seproxyhal_spi_buffer);
            break;

        case SEPROXYHAL_TAG_BUTTON_PUSH_EVENT: // for Nano S
            UX_BUTTON_PUSH_EVENT(G_io_seproxyhal_spi_buffer);
            break;

        case SEPROXYHAL_TAG_DISPLAY_PROCESSED_EVENT:
            if (!UX_DISPLAYED())
                UX_DISPLAYED_EVENT();
            break;

        case SEPROXYHAL_TAG_TICKER_EVENT: { //
            UX_TICKER_EVENT(G_io_seproxyhal_spi_buffer, {
                    if (UX_ALLOWED) {
                        UX_REDISPLAY();
                    }
            });
            break;
        }

            // unknown events are acknowledged
        default:
            UX_DEFAULT_EVENT();
            break;
    }
    if (!io_seproxyhal_spi_is_status_sent()) {
        io_seproxyhal_general_status();
    }
    return 1; // DO NOT reset the current APDU transport
}

unsigned short io_exchange_al(unsigned char channel, unsigned short tx_len) {
    switch (channel & ~(IO_FLAGS)) {
        case CHANNEL_KEYBOARD:
            break;

            // multiplexed io exchange over a SPI channel and TLV encapsulated protocol
        case CHANNEL_SPI:
            if (tx_len) {
                io_seproxyhal_spi_send(G_io_apdu_buffer, tx_len);

                if (channel & IO_RESET_AFTER_REPLIED) {
                    reset();
                }
                return 0; // nothing received from the master so far (it's a tx
                // transaction)
            } else {
                return io_seproxyhal_spi_recv(G_io_apdu_buffer, sizeof(G_io_apdu_buffer), 0);
            }

        default:
            THROW(INVALID_PARAMETER);
    }
    return 0;
}

void extractHDPath(uint32_t rx, uint32_t offset) {
    if ((rx - offset) < sizeof(uint32_t) * HDPATH_LEN_DEFAULT) {
        THROW(APDU_CODE_WRONG_LENGTH);
    }

    MEMCPY(hdPath, G_io_apdu_buffer + offset, sizeof(uint32_t) * HDPATH_LEN_DEFAULT);

    const bool mainnet = hdPath[0] == HDPATH_0_DEFAULT &&
                         hdPath[1] == HDPATH_1_DEFAULT;

    const bool testnet = hdPath[0] == HDPATH_0_TESTNET &&
                         hdPath[1] == HDPATH_1_TESTNET;

    if (!mainnet && !testnet) {
        THROW(APDU_CODE_DATA_INVALID);
    }

    const bool is_valid = ((hdPath[2] & HDPATH_RESTRICTED_MASK) == 0x80000000u) &&
                        (hdPath[3] == 0x00000000u) &&
                        ((hdPath[4] & HDPATH_RESTRICTED_MASK) == 0x00000000u);

    if (!is_valid && !app_mode_expert()){
        THROW(APDU_CODE_DATA_INVALID);
    }
}

bool process_chunk(volatile uint32_t *tx, uint32_t rx) {
    UNUSED(tx);
    const uint8_t payloadType = G_io_apdu_buffer[OFFSET_PAYLOAD_TYPE];

    if (rx < OFFSET_DATA) {
        THROW(APDU_CODE_WRONG_LENGTH);
    }

    if(G_io_apdu_buffer[OFFSET_P2] != 0 && G_io_apdu_buffer[OFFSET_P2] != 1){
        THROW(APDU_CODE_DATA_INVALID);
    }

    bool is_stake_tx = parser_tx_obj.special_transfer_type == neuron_stake_transaction;

    uint32_t added;
    switch (payloadType) {
        case 0:
            tx_initialize();
            tx_reset();
            extractHDPath(rx, OFFSET_DATA);
            MEMZERO(&parser_tx_obj, sizeof(parser_tx_t));
            if(G_io_apdu_buffer[OFFSET_P2] == 1){
                parser_tx_obj.special_transfer_type = neuron_stake_transaction;
            }else{
                parser_tx_obj.special_transfer_type = normal_transaction;
            }
            tx_initialized = true;
            return false;
        case 1:
            if (is_stake_tx && G_io_apdu_buffer[OFFSET_P2] != 1){
                THROW(APDU_CODE_DATA_INVALID);
            }
            if (!tx_initialized) {
                THROW(APDU_CODE_TX_NOT_INITIALIZED);
            }
            added = tx_append(&(G_io_apdu_buffer[OFFSET_DATA]), rx - OFFSET_DATA);
            if (added != rx - OFFSET_DATA) {
                tx_initialized = false;
                THROW(APDU_CODE_OUTPUT_BUFFER_TOO_SMALL);
            }
            return false;
        case 2:
            if (is_stake_tx && G_io_apdu_buffer[OFFSET_P2] != 1){
                THROW(APDU_CODE_DATA_INVALID);
            }
            if (!tx_initialized) {
                THROW(APDU_CODE_TX_NOT_INITIALIZED);
            }
            added = tx_append(&(G_io_apdu_buffer[OFFSET_DATA]), rx - OFFSET_DATA);
            if (added != rx - OFFSET_DATA) {
                tx_initialized = false;
                THROW(APDU_CODE_OUTPUT_BUFFER_TOO_SMALL);
            }
            return true;
    }
    tx_initialized = false;
    THROW(APDU_CODE_INVALIDP1P2);
}

void handle_generic_apdu(volatile uint32_t *flags, volatile uint32_t *tx, uint32_t rx) {
    UNUSED(flags);

    if (rx > 4 && MEMCMP(G_io_apdu_buffer, "\xE0\x01\x00\x00", 4) == 0) {
        // Respond to get device info command
        uint8_t *p = G_io_apdu_buffer;
        // Target ID        4 bytes
        p[0] = (TARGET_ID >> 24) & 0xFF;
        p[1] = (TARGET_ID >> 16) & 0xFF;
        p[2] = (TARGET_ID >> 8) & 0xFF;
        p[3] = (TARGET_ID >> 0) & 0xFF;
        p += 4;
        // SE Version       [length][non-terminated string]
        *p = os_version(p + 1, 64);
        p = p + 1 + *p;
        // Flags            [length][flags]
        *p = 0;
        p++;
        // MCU Version      [length][non-terminated string]
        *p = os_seph_version(p + 1, 64);
        p = p + 1 + *p;

        *tx = p - G_io_apdu_buffer;
        THROW(APDU_CODE_OK);
    }
}

void app_init() {
    io_seproxyhal_init();

#ifdef TARGET_NANOX
    // grab the current plane mode setting
    G_io_app.plane_mode = os_setting_get(OS_SETTING_PLANEMODE, NULL, 0);
#endif // TARGET_NANOX

    USB_power(0);
    USB_power(1);
    app_mode_reset();
    view_idle_show(0, NULL);

#ifdef HAVE_BLE
    // Enable Bluetooth
    BLE_power(0, NULL);
    BLE_power(1, "Nano X");
#endif // HAVE_BLE

}

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wmissing-noreturn"

void app_main() {
    volatile uint32_t rx = 0, tx = 0, flags = 0;

    for (;;) {
        volatile uint16_t sw = 0;

        BEGIN_TRY;
        {
            TRY;
            {
                rx = tx;
                tx = 0;

                rx = io_exchange(CHANNEL_APDU | flags, rx);
                flags = 0;
                CHECK_APP_CANARY()

                if (rx == 0)
                    THROW(APDU_CODE_EMPTY_BUFFER);

                handle_generic_apdu(&flags, &tx, rx);
                CHECK_APP_CANARY()

                handleApdu(&flags, &tx, rx);
                CHECK_APP_CANARY()
            }
            CATCH(EXCEPTION_IO_RESET)
            {
                // reset IO and UX before continuing
                app_init();
                continue;
            }
            CATCH_OTHER(e);
            {
                switch (e & 0xF000) {
                    case 0x6000:
                    case 0x9000:
                        sw = e;
                        break;
                    default:
                        sw = 0x6800 | (e & 0x7FF);
                        break;
                }
                G_io_apdu_buffer[tx] = sw >> 8;
                G_io_apdu_buffer[tx + 1] = sw;
                tx += 2;
            }
            FINALLY;
            {}
        }
        END_TRY;
    }
}

#pragma clang diagnostic pop
