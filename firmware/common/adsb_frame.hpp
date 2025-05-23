/*
 * Copyright (C) 2014 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2017 Furrtek
 *
 * This file is part of PortaPack.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; see the file COPYING.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street,
 * Boston, MA 02110-1301, USA.
 */

#ifndef __ADSB_FRAME_H__
#define __ADSB_FRAME_H__

#include <cstring>
#include <string>
#include <cstdint>

namespace adsb {

alignas(4) const uint8_t adsb_preamble[16] = {1, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 0, 0, 0, 0};
alignas(4) const char icao_id_lut[65] = "#ABCDEFGHIJKLMNOPQRSTUVWXYZ##### ###############0123456789######";

class ADSBFrame {
   public:
    uint8_t get_DF() {
        return (raw_data[0] >> 3);
    }

    uint8_t get_msg_type() {
        return (raw_data[4] >> 3);
    }

    uint8_t get_msg_sub() {
        return (raw_data[4] & 7);
    }

    uint8_t get_sil_value() {
        return ((raw_data[10] >> 6) & 0b11);  // 83-84 bits
    }

    uint32_t get_ICAO_address() {
        return (raw_data[1] << 16) + (raw_data[2] << 8) + raw_data[3];
    }

    void set_rx_timestamp(uint32_t timestamp) {
        rx_timestamp = timestamp;
    }
    uint32_t get_rx_timestamp() {
        return rx_timestamp;
    }

    void clear() {
        index = 0;
        memset(raw_data, 0, 14);
    }

    void push_byte(uint8_t byte) {
        if (index >= 14)
            return;

        raw_data[index++] = byte;
    }

    uint8_t* get_raw_data() const {
        return (uint8_t*)raw_data;
    }

    void make_CRC() {
        uint32_t computed_CRC = compute_CRC();

        uint8_t crc_pos = (raw_data[0] & 0x80) ? 11 : 4;

        // Insert CRC in frame
        raw_data[crc_pos] = (computed_CRC >> 16) & 0xFF;
        raw_data[crc_pos + 1] = (computed_CRC >> 8) & 0xFF;
        raw_data[crc_pos + 2] = computed_CRC & 0xFF;
    }

    uint32_t check_CRC() {
        uint32_t computed_CRC = compute_CRC();

        uint8_t crc_pos = (raw_data[0] & 0x80) ? 11 : 4;

        uint32_t received_CRC = (raw_data[crc_pos] << 16) |
                                (raw_data[crc_pos + 1] << 8) |
                                raw_data[crc_pos + 2];

        return (received_CRC ^ computed_CRC) & 0xFFFFFF;
    }

    bool empty() {
        return (index == 0);
    }

   private:
    static const uint8_t adsb_preamble[16];
    static const char icao_id_lut[65];
    alignas(4) uint8_t index{0};
    alignas(4) uint8_t raw_data[14]{};  // 112 bits at most
    uint32_t rx_timestamp{};

    uint32_t compute_CRC() {
        uint8_t adsb_crc[14] = {0};  // Temp buffer
        uint8_t b, c, s, bitn;
        const uint32_t crc_poly = 0x1205FFF;
        uint8_t data_len = (raw_data[0] & 0x80) ? 11 : 4;

        // Copy frame data
        memcpy(adsb_crc, raw_data, data_len);

        // Compute CRC
        for (c = 0; c < data_len; c++) {
            for (b = 0; b < 8; b++) {
                if ((adsb_crc[c] << b) & 0x80) {
                    for (s = 0; s < 25; s++) {
                        bitn = (c * 8) + b + s;
                        if ((crc_poly >> s) & 1) adsb_crc[bitn >> 3] ^= (0x80 >> (bitn & 7));
                    }
                }
            }
        }

        return (adsb_crc[data_len] << 16) + (adsb_crc[data_len + 1] << 8) + adsb_crc[data_len + 2];
    }
};

} /* namespace adsb */

#endif /*__ADSB_FRAME_H__*/
