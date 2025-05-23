/*
 * Copyright (C) 2015 Jared Boone, ShareBrained Technology, Inc.
 * Copyright (C) 2016 Furrtek
 * Copyright (C) 2024 Mark Thompson
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

#include "portapack_persistent_memory.hpp"

#include "audio.hpp"
#include "crc.hpp"
#include "file.hpp"
#include "hal.h"
#include "irq_controls.hpp"
#include "memory_map.hpp"
#include "portapack.hpp"
#include "string_format.hpp"
#include "ui.hpp"
#include "ui_painter.hpp"
#include "ui_flash_utility.hpp"
#include "utility.hpp"
#include "rtc_time.hpp"
#include "file_path.hpp"

#include <algorithm>
#include <string>
#include <fstream>
#include <utility>

#include <ch.h>
#include <hal.h>

using namespace std;
using namespace ui;

namespace portapack {
namespace persistent_memory {

constexpr rf::Frequency target_frequency_reset_value{100000000};

using ppb_range_t = range_t<ppb_t>;
constexpr ppb_range_t ppb_range{-99000, 99000};
constexpr ppb_t ppb_reset_value{0};

using tone_mix_range_t = range_t<int32_t>;
constexpr tone_mix_range_t tone_mix_range{10, 99};
constexpr int32_t tone_mix_reset_value{20};

using afsk_freq_range_t = range_t<int32_t>;
constexpr afsk_freq_range_t afsk_freq_range{1, 4000};
constexpr int32_t afsk_mark_reset_value{1200};
constexpr int32_t afsk_space_reset_value{2200};

using modem_baudrate_range_t = range_t<int32_t>;
constexpr modem_baudrate_range_t modem_baudrate_range{50, 9600};
constexpr int32_t modem_baudrate_reset_value{1200};

/*
using modem_bw_range_t = range_t<int32_t>;
constexpr modem_bw_range_t modem_bw_range { 1000, 50000 };
constexpr int32_t modem_bw_reset_value { 15000 };
*/

using modem_repeat_range_t = range_t<int32_t>;
constexpr modem_repeat_range_t modem_repeat_range{1, 99};
constexpr int32_t modem_repeat_reset_value{5};

using clkout_freq_range_t = range_t<uint32_t>;
constexpr clkout_freq_range_t clkout_freq_range{4, 60000};  // Min. CLK out of Si5351A/B/C-B is 2.5khz , but in our application -intermediate freq 800Mhz-,Min working CLK=4khz.
constexpr uint16_t clkout_freq_reset_value{10000};

enum data_structure_version_enum : uint32_t {
    VERSION_CURRENT = 0x10000005,
};

static const uint32_t TOUCH_CALIBRATION_MAGIC = 0x074af82f;

/* UI config.
 * NB: Will be default init - override in defaults(). */
struct ui_config_t {
    uint16_t clkout_freq;

    // NB: bitsfields have to be the same type or the compiler will
    // split into a new byte hence uint8_t for these booleans.
    uint8_t backlight_timeout : 3;
    uint8_t enable_backlight_timeout : 1;
    uint8_t show_gui_return_icon : 1;
    uint8_t load_app_settings : 1;
    uint8_t save_app_settings : 1;
    uint8_t UNUSED_7 : 1;  // Deprecated, but bit can be set by older firmware

    bool disable_touchscreen : 1;
    bool hide_clock : 1;
    bool clock_show_date : 1;
    bool clkout_enabled : 1;
    bool apply_fake_brightness : 1;  // Fake brightness level, which eventually could be something along the lines of apply_pwm_brightness
    bool stealth_mode : 1;
    bool config_login : 1;
    bool config_splash : 1;
};
static_assert(sizeof(ui_config_t) == sizeof(uint32_t));

/* Additional UI config.
 * NB: Will be default init - override in defaults(). */
struct ui_config2_t {
    /* Top icon bar */
    bool hide_speaker : 1;
    bool hide_converter : 1;
    bool hide_stealth : 1;
    bool hide_camera : 1;
    bool hide_sleep : 1;
    bool hide_bias_tee : 1;
    bool hide_clock : 1;
    bool hide_sd_card : 1;

    bool hide_mute : 1;
    bool hide_fake_brightness : 1;
    bool hide_numeric_battery : 1;
    bool hide_battery_icon : 1;
    bool override_batt_calc : 1;
    bool button_repeat_delay : 1;
    bool button_repeat_speed : 1;
    bool button_long_press_delay : 1;
    bool battery_charge_hint : 1;

    uint8_t theme_id;
};
static_assert(sizeof(ui_config2_t) == sizeof(uint32_t));

/* Additional config.
 * NB: Will be default init - override in defaults(). */
struct misc_config_t {
    bool mute_audio : 1;
    bool disable_speaker : 1;
    bool config_disable_external_tcxo : 1;
    bool config_sdcard_high_speed_io : 1;
    bool config_disable_config_mode : 1;
    bool beep_on_packets : 1;
    bool UNUSED_6 : 1;
    bool UNUSED_7 : 1;

    uint8_t PLACEHOLDER_1;
    uint8_t PLACEHOLDER_2;
    uint8_t PLACEHOLDER_3;
};
static_assert(sizeof(misc_config_t) == sizeof(uint32_t));

#define MC_CONFIG_DISABLE_CONFIG_MODE 0x00000010  // config_disable_config_mode bit in struct above

/* IMPORTANT: Update dump_persistent_memory (below) when changing data_t. */

/* Struct must pack the same way on M4 and M0 cores.
 * NB: When adding new members, keep 32bit-aligned.*/

struct data_t {
    data_structure_version_enum structure_version;
    int64_t target_frequency;
    int32_t correction_ppb;
    uint32_t touch_calibration_magic;
    touch::Calibration touch_calibration;  // 7 * 32 bits.

    // Modem
    uint32_t modem_def_index;
    serial_format_t serial_format;
    int32_t modem_bw;
    int32_t afsk_mark_freq;
    int32_t afsk_space_freq;
    int32_t modem_baudrate;
    int32_t modem_repeat;

    uint32_t UNUSED_2;
    uint32_t UNUSED_3;
    uint32_t UNUSED_4;

    // UI Config
    ui_config_t ui_config;

    uint32_t pocsag_last_address;
    uint32_t pocsag_ignore_address;

    int32_t tone_mix;

    // Hardware
    uint32_t hardware_config;

    // Recon App
    uint64_t recon_config;
    int8_t recon_repeat_nb;
    int8_t recon_repeat_gain;
    uint8_t recon_repeat_delay;

    // enable or disable converter
    bool converter;
    // set up converter (false) or down converter (true) converter
    bool updown_converter;
    bool updown_frequency_rx_correction;
    bool updown_frequency_tx_correction;
    bool lcd_normally_black : 1;
    bool encoder_dial_direction : 1;  // false = normal, true = reverse
    bool UNUSED_6 : 1;
    bool UNUSED_7 : 1;

    // up/down converter offset
    int64_t converter_frequency_offset;

    // frequency correction
    uint32_t frequency_rx_correction;
    uint32_t frequency_tx_correction;

    // Rotary encoder dial sensitivity (encoder.cpp/hpp)
    uint16_t encoder_dial_sensitivity : 4;

    // fake brightness level (not switch, switch is in another place)
    uint16_t fake_brightness_level : 4;

    // Encoder rotation rate multiplier for larger increments when rotated rapidly
    uint16_t encoder_rate_multiplier : 4;

    uint16_t UNUSED : 4;

    // Headphone volume in centibels.
    int16_t headphone_volume_cb;

    // Misc flags
    misc_config_t misc_config;

    // Additional UI settings.
    ui_config2_t ui_config2;

    // recovery mode magic value storage
    uint32_t config_mode_storage;

    // Daylight savings time
    dst_config_t dst_config;

    // Menu Color Scheme
    Color menu_color;

    uint16_t touchscreen_threshold;

    constexpr data_t()
        : structure_version(data_structure_version_enum::VERSION_CURRENT),
          target_frequency(target_frequency_reset_value),
          correction_ppb(ppb_reset_value),
          touch_calibration_magic(TOUCH_CALIBRATION_MAGIC),
          touch_calibration(touch::Calibration()),

          modem_def_index(0),  // TODO: Unused?
          serial_format(),
          modem_bw(15000),  // TODO: Unused?
          afsk_mark_freq(afsk_mark_reset_value),
          afsk_space_freq(afsk_space_reset_value),
          modem_baudrate(modem_baudrate_reset_value),
          modem_repeat(modem_repeat_reset_value),
          UNUSED_2(0),
          UNUSED_3(0),
          UNUSED_4(0),
          ui_config(),

          pocsag_last_address(0),    // TODO: A better default?
          pocsag_ignore_address(0),  // TODO: A better default?

          tone_mix(tone_mix_reset_value),

          hardware_config(0),

          recon_config(0),
          recon_repeat_nb(0),
          recon_repeat_gain(0),
          recon_repeat_delay(0),

          converter(false),
          updown_converter(false),
          updown_frequency_rx_correction(false),
          updown_frequency_tx_correction(false),
          lcd_normally_black(false),
          encoder_dial_direction(false),
          UNUSED_6(false),
          UNUSED_7(false),

          converter_frequency_offset(0),

          frequency_rx_correction(0),
          frequency_tx_correction(0),

          encoder_dial_sensitivity(DIAL_SENSITIVITY_NORMAL),
          fake_brightness_level(BRIGHTNESS_50),
          encoder_rate_multiplier(1),
          UNUSED(0),

          headphone_volume_cb(-600),
          misc_config(),
          ui_config2(),
          config_mode_storage(CONFIG_MODE_NORMAL_VALUE),
          dst_config(),
          menu_color(Color::grey()),
          touchscreen_threshold(32) {
    }
};

struct backup_ram_t {
   private:
    volatile uint32_t regfile[PMEM_SIZE_WORDS - 1];
    volatile uint32_t check_value;

    static void copy(const backup_ram_t& src, backup_ram_t& dst) {
        for (size_t i = 0; i < PMEM_SIZE_WORDS - 1; i++) {
            dst.regfile[i] = src.regfile[i];
        }
        dst.check_value = src.check_value;
    }

    static void copy_from_data_t(const data_t& src, backup_ram_t& dst) {
        const uint32_t* const src_words = (uint32_t*)&src;
        const size_t word_count = (sizeof(data_t) + 3) / 4;
        for (size_t i = 0; i < PMEM_SIZE_WORDS - 1; i++) {
            if (i < word_count) {
                dst.regfile[i] = src_words[i];
            } else {
                dst.regfile[i] = 0;
            }
        }
    }

    uint32_t compute_check_value() {
        CRC<32> crc{0x04c11db7, 0xffffffff, 0xffffffff};
        for (size_t i = 0; i < PMEM_SIZE_WORDS - 1; i++) {
            const auto word = regfile[i];
            crc.process_byte((word >> 0) & 0xff);
            crc.process_byte((word >> 8) & 0xff);
            crc.process_byte((word >> 16) & 0xff);
            crc.process_byte((word >> 24) & 0xff);
        }
        return crc.checksum();
    }

   public:
    /* default constructor */
    backup_ram_t()
        : check_value(0) {
        const data_t defaults = data_t();
        copy_from_data_t(defaults, *this);
    }

    /* copy-assignment operator */
    backup_ram_t& operator=(const backup_ram_t& src) {
        copy(src, *this);
        return *this;
    }

    /* Calculate a check value from `this`, and check against
     * the stored value.
     */
    bool is_valid() {
        return compute_check_value() == check_value;
    }

    /* Assuming `this` contains valid data, update the checksum
     * and copy to the destination.
     */
    void persist_to(backup_ram_t& dst) {
        check_value = compute_check_value();
        copy(*this, dst);
    }

    /* Access functions for DebugPmemView */
    uint32_t pmem_data_word(uint32_t index) {
        return (index > sizeof(regfile) / sizeof(uint32_t)) ? 0xFFFFFFFF : regfile[index];
    }

    uint32_t pmem_stored_checksum(void) {
        return check_value;
    }

    uint32_t pmem_calculated_checksum(void) {
        return compute_check_value();
    }
};

static_assert(sizeof(backup_ram_t) == memory::map::backup_ram.size());
static_assert(sizeof(data_t) <= sizeof(backup_ram_t) - sizeof(uint32_t));

/* Uncomment to get a compiler error with the data_t size. */
// template <size_t N>
// struct ShowSize;
// ShowSize<sizeof(data_t)> __data_t_size;

static backup_ram_t* const backup_ram = reinterpret_cast<backup_ram_t*>(memory::map::backup_ram.base());

static backup_ram_t cached_backup_ram;
static data_t* data = reinterpret_cast<data_t*>(&cached_backup_ram);

namespace cache {

void defaults() {
    cached_backup_ram = backup_ram_t();

    // If the desired default is 0/false, then no need to set it here (buffer is initialized to 0)
    // NB: This function is only called when pmem is reset; also see firmware upgrade handling below.
    set_config_backlight_timer(backlight_config_t{});
    set_config_splash(true);
    set_config_disable_external_tcxo(false);
    set_encoder_dial_sensitivity(DIAL_SENSITIVITY_NORMAL);
    set_encoder_dial_direction(false);  // false = normal, true = reverse
    set_config_speaker_disable(true);   // Disable AK4951 speaker by default (in case of OpenSourceSDRLab H2)
    set_menu_color(Color::grey());
    set_ui_hide_numeric_battery(true);  // hide the numeric battery by default - no space to display it

    // Default values for recon app.
    set_recon_autosave_freqs(false);
    set_recon_autostart_recon(true);
    set_recon_continuous(true);
    set_recon_clear_output(false);
    set_recon_load_freqs(true);
    set_recon_load_repeaters(true);
    set_recon_load_ranges(true);
    set_recon_update_ranges_when_recon(true);
    set_recon_load_hamradios(true);
    set_recon_match_mode(0);
    set_recon_repeat_recorded(false);
    set_recon_repeat_recorded_file_mode(false);  // false delete repeater , true keep repeated
    set_recon_repeat_amp(false);
    set_recon_repeat_gain(35);
    set_recon_repeat_nb(3);
    set_recon_repeat_delay(1);

    set_config_sdcard_high_speed_io(false, true);
}

void init() {
    const auto switches_state = swizzled_switches();

    // ignore for valid check
    auto config_mode_backup = config_mode_storage_direct();
    set_config_mode_storage_direct(CONFIG_MODE_NORMAL_VALUE);

    if (!(((switches_state >> (size_t)ui::KeyEvent::Left & 1) == 1) &&
          ((switches_state >> (size_t)ui::KeyEvent::Right & 1) == 1)) &&
        backup_ram->is_valid()) {
        // Copy valid persistent data into cache.
        cached_backup_ram = *backup_ram;

        // Check that structure data we copied into cache is the expected
        // version. If not, initialize cache to defaults.
        if (data->structure_version != data_structure_version_enum::VERSION_CURRENT) {
            // TODO: Can provide version-to-version upgrade functions here,
            // if we want to be fancy.
            defaults();
        }
    } else {
        // Copy defaults into cache.
        defaults();
    }
    set_config_mode_storage_direct(config_mode_backup);

    // Firmware upgrade handling - adjust newly defined fields where 0 is an invalid default
    if (fake_brightness_level() == 0) set_fake_brightness_level(BRIGHTNESS_50);
    if (menu_color().v == 0) set_menu_color(Color::grey());
}

void persist() {
    cached_backup_ram.persist_to(*backup_ram);
}

} /* namespace cache */

uint32_t get_data_structure_version() {
    return data->structure_version;
}

uint32_t pmem_data_word(uint32_t index) {
    return backup_ram->pmem_data_word(index);
}

uint32_t pmem_stored_checksum(void) {
    return backup_ram->pmem_stored_checksum();
}

uint32_t pmem_calculated_checksum(void) {
    return backup_ram->pmem_calculated_checksum();
}

rf::Frequency target_frequency() {
    rf::tuning_range.reset_if_outside(data->target_frequency, target_frequency_reset_value);
    return data->target_frequency;
}

void set_target_frequency(const rf::Frequency new_value) {
    data->target_frequency = rf::tuning_range.clip(new_value);
}

volume_t headphone_volume() {
    auto volume = volume_t::centibel(data->headphone_volume_cb);
    volume = audio::headphone::volume_range().limit(volume);
    return volume;
}

void set_headphone_volume(volume_t new_value) {
    new_value = audio::headphone::volume_range().limit(new_value);
    data->headphone_volume_cb = new_value.centibel();
}

ppb_t correction_ppb() {
    ppb_range.reset_if_outside(data->correction_ppb, ppb_reset_value);
    return data->correction_ppb;
}

void set_correction_ppb(const ppb_t new_value) {
    const auto clipped_value = ppb_range.clip(new_value);
    data->correction_ppb = clipped_value;
    portapack::clock_manager.set_reference_ppb(clipped_value);
}

void set_touch_calibration(const touch::Calibration& new_value) {
    data->touch_calibration = new_value;
    data->touch_calibration_magic = TOUCH_CALIBRATION_MAGIC;
}

const touch::Calibration& touch_calibration() {
    if (data->touch_calibration_magic != TOUCH_CALIBRATION_MAGIC) {
        set_touch_calibration(touch::Calibration());
    }
    return data->touch_calibration;
}

int32_t tone_mix() {
    tone_mix_range.reset_if_outside(data->tone_mix, tone_mix_reset_value);
    return data->tone_mix;
}

void set_tone_mix(const int32_t new_value) {
    data->tone_mix = tone_mix_range.clip(new_value);
}

int32_t afsk_mark_freq() {
    afsk_freq_range.reset_if_outside(data->afsk_mark_freq, afsk_mark_reset_value);
    return data->afsk_mark_freq;
}

void set_afsk_mark(const int32_t new_value) {
    data->afsk_mark_freq = afsk_freq_range.clip(new_value);
}

int32_t afsk_space_freq() {
    afsk_freq_range.reset_if_outside(data->afsk_space_freq, afsk_space_reset_value);
    return data->afsk_space_freq;
}

void set_afsk_space(const int32_t new_value) {
    data->afsk_space_freq = afsk_freq_range.clip(new_value);
}

uint32_t get_modem_def_index() {
    return data->modem_def_index;
}

int32_t modem_baudrate() {
    modem_baudrate_range.reset_if_outside(data->modem_baudrate, modem_baudrate_reset_value);
    return data->modem_baudrate;
}

void set_modem_baudrate(const int32_t new_value) {
    data->modem_baudrate = modem_baudrate_range.clip(new_value);
}

int32_t modem_bw() {
    return data->modem_bw;
}

/*void set_modem_bw(const int32_t new_value) {
    data->modem_bw = modem_bw_range.clip(new_value);
}
*/

uint8_t modem_repeat() {
    modem_repeat_range.reset_if_outside(data->modem_repeat, modem_repeat_reset_value);
    return data->modem_repeat;
}

void set_modem_repeat(const uint32_t new_value) {
    data->modem_repeat = modem_repeat_range.clip(new_value);
}

serial_format_t serial_format() {
    return data->serial_format;
}

void set_serial_format(const serial_format_t new_value) {
    data->serial_format = new_value;
}

bool show_gui_return_icon() {  // add return icon in touchscreen menu
    return data->ui_config.show_gui_return_icon != 0;
}

bool disable_touchscreen() {  // Option to disable touch screen
    return data->ui_config.disable_touchscreen;
}

bool hide_clock() {  // Hide clock from main menu
    return data->ui_config.hide_clock;
}

bool clock_with_date() {  // Show clock with date, if not hidden
    return data->ui_config.clock_show_date;
}

bool clkout_enabled() {
    return data->ui_config.clkout_enabled;
}

bool config_audio_mute() {
    return data->misc_config.mute_audio;
}

bool config_speaker_disable() {
    return data->misc_config.disable_speaker;
}

bool config_disable_external_tcxo() {
    return data->misc_config.config_disable_external_tcxo;
}

bool config_disable_config_mode() {
    return data->misc_config.config_disable_config_mode;
}

bool beep_on_packets() {
    return data->misc_config.beep_on_packets;
}

bool config_sdcard_high_speed_io() {
    return data->misc_config.config_sdcard_high_speed_io;
}

bool stealth_mode() {
    return data->ui_config.stealth_mode;
}

bool apply_fake_brightness() {
    return data->ui_config.apply_fake_brightness;
}

bool config_login() {
    return data->ui_config.config_login;
}

bool config_splash() {
    return data->ui_config.config_splash;
}

uint8_t config_cpld() {
    return data->hardware_config;
}

backlight_config_t config_backlight_timer() {
    return {static_cast<backlight_timeout_t>(data->ui_config.backlight_timeout),
            data->ui_config.enable_backlight_timeout == 1};
}

void set_gui_return_icon(bool v) {
    data->ui_config.show_gui_return_icon = v ? 1 : 0;
}

bool load_app_settings() {
    return data->ui_config.load_app_settings;
}

void set_load_app_settings(bool v) {
    data->ui_config.load_app_settings = v ? 1 : 0;
}

bool save_app_settings() {
    return data->ui_config.save_app_settings;
}

void set_save_app_settings(bool v) {
    data->ui_config.save_app_settings = v ? 1 : 0;
}

void set_disable_touchscreen(bool v) {
    data->ui_config.disable_touchscreen = v;
}

void set_clock_hidden(bool v) {
    data->ui_config.hide_clock = v;
}

void set_clock_with_date(bool v) {
    data->ui_config.clock_show_date = v;
}

void set_clkout_enabled(bool v) {
    data->ui_config.clkout_enabled = v;
}

void set_config_audio_mute(bool v) {
    data->misc_config.mute_audio = v;
}

void set_config_speaker_disable(bool v) {
    data->misc_config.disable_speaker = v;
}

void set_config_disable_external_tcxo(bool v) {
    data->misc_config.config_disable_external_tcxo = v;
}

void set_config_disable_config_mode(bool v) {
    data->misc_config.config_disable_config_mode = v;
}

void set_beep_on_packets(bool v) {
    data->misc_config.beep_on_packets = v;
}

void set_config_sdcard_high_speed_io(bool v, bool save) {
    if (v) {
        /* 200MHz / (2 * 2) = 50MHz */
        /* TODO: Adjust SCU pin configurations: pull-up/down, slew, glitch filter? */
        sdio_cclk_set(2);
    } else {
        /* 200MHz / (2 * 4) = 25MHz */
        sdio_cclk_set(4);
    }
    if (save)
        data->misc_config.config_sdcard_high_speed_io = v;
}

void set_stealth_mode(bool v) {
    data->ui_config.stealth_mode = v;
}

void set_config_login(bool v) {
    data->ui_config.config_login = v;
}

void set_config_splash(bool v) {
    data->ui_config.config_splash = v;
}

void set_config_cpld(uint8_t i) {
    data->hardware_config = i;
}

void set_config_backlight_timer(const backlight_config_t& new_value) {
    data->ui_config.backlight_timeout = static_cast<uint8_t>(new_value.timeout_enum());
    data->ui_config.enable_backlight_timeout = static_cast<uint8_t>(new_value.timeout_enabled());
}

void set_apply_fake_brightness(const bool v) {
    data->ui_config.apply_fake_brightness = v;
}

uint32_t pocsag_last_address() {
    return data->pocsag_last_address;
}

void set_pocsag_last_address(uint32_t address) {
    data->pocsag_last_address = address;
}

uint32_t pocsag_ignore_address() {
    return data->pocsag_ignore_address;
}

void set_pocsag_ignore_address(uint32_t address) {
    data->pocsag_ignore_address = address;
}

uint16_t clkout_freq() {
    auto freq = data->ui_config.clkout_freq;

    if (freq < clkout_freq_range.minimum || freq > clkout_freq_range.maximum)
        set_clkout_freq(clkout_freq_reset_value);

    return data->ui_config.clkout_freq;
}

void set_clkout_freq(uint16_t freq) {
    data->ui_config.clkout_freq = freq;
}

/* Recon app */
enum recon_config_bits {
    RC_UNUSED_BIT = 63,  // just a reminder that this is a 64-bit field
    RC_AUTOSAVE_FREQS = 31,
    RC_AUTOSTART_RECON = 30,
    RC_CONTINUOUS = 29,
    RC_CLEAR_OUTPUT = 28,
    RC_LOAD_FREQS = 27,
    RC_LOAD_RANGES = 26,
    RC_UPDATE_RANGES = 25,
    RC_LOAD_HAMRADIOS = 24,
    RC_MATCH_MODE = 23,
    RC_AUTO_RECORD_LOCKED = 22,
    RC_REPEAT_RECORDED = 21,
    RC_REPEAT_AMP = 20,
    RC_LOAD_REPEATERS = 19,
    RC_REPEAT_FILE_MODE = 18,
};

bool check_recon_config_bit(uint8_t rc_bit) {
    return ((data->recon_config >> rc_bit) & 1) != 0;
}
void set_recon_config_bit(uint8_t rc_bit, bool v) {
    auto bit_mask = 1LL << rc_bit;
    data->recon_config = v ? (data->recon_config | bit_mask) : (data->recon_config & ~bit_mask);
}
uint64_t get_recon_config() {
    return data->recon_config;
}
bool recon_autosave_freqs() {
    return check_recon_config_bit(RC_AUTOSAVE_FREQS);
}
bool recon_autostart_recon() {
    return check_recon_config_bit(RC_AUTOSTART_RECON);
}
bool recon_continuous() {
    return check_recon_config_bit(RC_CONTINUOUS);
}
bool recon_clear_output() {
    return check_recon_config_bit(RC_CLEAR_OUTPUT);
}
bool recon_load_freqs() {
    return check_recon_config_bit(RC_LOAD_FREQS);
}
bool recon_load_ranges() {
    return check_recon_config_bit(RC_LOAD_RANGES);
}
bool recon_update_ranges_when_recon() {
    return check_recon_config_bit(RC_UPDATE_RANGES);
}
bool recon_load_hamradios() {
    return check_recon_config_bit(RC_LOAD_HAMRADIOS);
}
bool recon_match_mode() {
    return check_recon_config_bit(RC_MATCH_MODE);
}
bool recon_auto_record_locked() {
    return check_recon_config_bit(RC_AUTO_RECORD_LOCKED);
}
bool recon_repeat_recorded() {
    return check_recon_config_bit(RC_REPEAT_RECORDED);
}
int8_t recon_repeat_nb() {
    return data->recon_repeat_nb;
}
int8_t recon_repeat_gain() {
    return data->recon_repeat_gain;
}
uint8_t recon_repeat_delay() {
    return data->recon_repeat_delay;
}
bool recon_repeat_amp() {
    return check_recon_config_bit(RC_REPEAT_AMP);
}
bool recon_load_repeaters() {
    return check_recon_config_bit(RC_LOAD_REPEATERS);
}
bool recon_repeat_recorded_file_mode() {
    return check_recon_config_bit(RC_REPEAT_FILE_MODE);
}
void set_recon_autosave_freqs(const bool v) {
    set_recon_config_bit(RC_AUTOSAVE_FREQS, v);
}
void set_recon_autostart_recon(const bool v) {
    set_recon_config_bit(RC_AUTOSTART_RECON, v);
}
void set_recon_continuous(const bool v) {
    set_recon_config_bit(RC_CONTINUOUS, v);
}
void set_recon_clear_output(const bool v) {
    set_recon_config_bit(RC_CLEAR_OUTPUT, v);
}
void set_recon_load_freqs(const bool v) {
    set_recon_config_bit(RC_LOAD_FREQS, v);
}
void set_recon_load_ranges(const bool v) {
    set_recon_config_bit(RC_LOAD_RANGES, v);
}
void set_recon_update_ranges_when_recon(const bool v) {
    set_recon_config_bit(RC_UPDATE_RANGES, v);
}
void set_recon_load_hamradios(const bool v) {
    set_recon_config_bit(RC_LOAD_HAMRADIOS, v);
}
void set_recon_match_mode(const bool v) {
    set_recon_config_bit(RC_MATCH_MODE, v);
}
void set_recon_auto_record_locked(const bool v) {
    set_recon_config_bit(RC_AUTO_RECORD_LOCKED, v);
}
void set_recon_repeat_recorded(const bool v) {
    set_recon_config_bit(RC_REPEAT_RECORDED, v);
}
void set_recon_repeat_nb(const int8_t v) {
    data->recon_repeat_nb = v;
}
void set_recon_repeat_gain(const int8_t v) {
    data->recon_repeat_gain = v;
}
void set_recon_repeat_delay(const uint8_t v) {
    data->recon_repeat_delay = v;
}
void set_recon_repeat_amp(const bool v) {
    set_recon_config_bit(RC_REPEAT_AMP, v);
}
void set_recon_load_repeaters(const bool v) {
    set_recon_config_bit(RC_LOAD_REPEATERS, v);
}
void set_recon_repeat_recorded_file_mode(const bool v) {
    set_recon_config_bit(RC_REPEAT_FILE_MODE, v);
}

/* UI Config 2 */
bool ui_hide_speaker() {
    return data->ui_config2.hide_speaker;
}
bool ui_hide_mute() {
    return data->ui_config2.hide_mute;
}
bool ui_hide_converter() {
    return data->ui_config2.hide_converter;
}
bool ui_hide_stealth() {
    return data->ui_config2.hide_stealth;
}
bool ui_hide_camera() {
    return data->ui_config2.hide_camera;
}
bool ui_hide_sleep() {
    return data->ui_config2.hide_sleep;
}
bool ui_hide_bias_tee() {
    return data->ui_config2.hide_bias_tee;
}
bool ui_hide_clock() {
    return data->ui_config2.hide_clock;
}
bool ui_hide_sd_card() {
    return data->ui_config2.hide_sd_card;
}
bool ui_hide_fake_brightness() {
    return data->ui_config2.hide_fake_brightness;
}
bool ui_hide_numeric_battery() {
    return data->ui_config2.hide_numeric_battery;
}
bool ui_hide_battery_icon() {
    return data->ui_config2.hide_battery_icon;
}
uint8_t ui_theme_id() {
    return data->ui_config2.theme_id;
}
bool ui_override_batt_calc() {
    return data->ui_config2.override_batt_calc;
}
bool ui_button_repeat_delay() {
    return data->ui_config2.button_repeat_delay;
}
bool ui_button_repeat_speed() {
    return data->ui_config2.button_repeat_speed;
}
bool ui_button_long_press_delay() {
    return data->ui_config2.button_long_press_delay;
}
bool ui_battery_charge_hint() {
    return data->ui_config2.battery_charge_hint;
}

void set_ui_hide_speaker(bool v) {
    data->ui_config2.hide_speaker = v;
}
void set_ui_hide_mute(bool v) {
    data->ui_config2.hide_mute = v;
}
void set_ui_hide_converter(bool v) {
    data->ui_config2.hide_converter = v;
    if (v)
        data->converter = false;
}
void set_ui_hide_stealth(bool v) {
    data->ui_config2.hide_stealth = v;
}
void set_ui_hide_camera(bool v) {
    data->ui_config2.hide_camera = v;
}
void set_ui_hide_sleep(bool v) {
    data->ui_config2.hide_sleep = v;
}
void set_ui_hide_bias_tee(bool v) {
    data->ui_config2.hide_bias_tee = v;
}
void set_ui_hide_clock(bool v) {
    data->ui_config2.hide_clock = v;
}
void set_ui_hide_sd_card(bool v) {
    data->ui_config2.hide_sd_card = v;
}
void set_ui_hide_fake_brightness(bool v) {
    data->ui_config2.hide_fake_brightness = v;
}
void set_ui_hide_numeric_battery(bool v) {
    data->ui_config2.hide_numeric_battery = v;
}
void set_ui_hide_battery_icon(bool v) {
    data->ui_config2.hide_battery_icon = v;
}
void set_ui_theme_id(uint8_t theme_id) {
    data->ui_config2.theme_id = theme_id;
}
void set_ui_override_batt_calc(bool v) {
    data->ui_config2.override_batt_calc = v;
}
void set_ui_button_repeat_delay(bool v) {
    data->ui_config2.button_repeat_delay = v;
}
void set_ui_button_repeat_speed(bool v) {
    data->ui_config2.button_repeat_speed = v;
}
void set_ui_button_long_press_delay(bool v) {
    data->ui_config2.button_long_press_delay = v;
}
void set_ui_battery_charge_hint(bool v) {
    data->ui_config2.battery_charge_hint = v;
}

/* Converter */
bool config_converter() {
    return data->converter;
}
bool config_updown_converter() {
    return data->updown_converter;
}
int64_t config_converter_freq() {
    return data->converter_frequency_offset;
}

void set_config_converter(bool v) {
    data->converter = v;
}
void set_config_updown_converter(bool v) {
    data->updown_converter = v;
}
void set_config_converter_freq(int64_t v) {
    data->converter_frequency_offset = v;
}

// Frequency correction settings

bool config_freq_tx_correction_updown() {
    return data->updown_frequency_tx_correction;
}
bool config_freq_rx_correction_updown() {
    return data->updown_frequency_rx_correction;
}
uint32_t config_freq_tx_correction() {
    return data->frequency_tx_correction;
}
uint32_t config_freq_rx_correction() {
    return data->frequency_rx_correction;
}
void set_freq_tx_correction_updown(bool v) {
    data->updown_frequency_tx_correction = v;
}
void set_freq_rx_correction_updown(bool v) {
    data->updown_frequency_rx_correction = v;
}
void set_config_freq_tx_correction(uint32_t v) {
    data->frequency_tx_correction = v;
}
void set_config_freq_rx_correction(uint32_t v) {
    data->frequency_rx_correction = v;
}

// IPS vs TFT
bool config_lcd_normally_black() {
    return data->lcd_normally_black;
}
void set_lcd_normally_black(bool v) {
    data->lcd_normally_black = v;
}

// Rotary encoder dial settings
uint8_t encoder_dial_sensitivity() {
    return data->encoder_dial_sensitivity;
}
void set_encoder_dial_sensitivity(uint8_t v) {
    data->encoder_dial_sensitivity = v;
}
uint8_t encoder_rate_multiplier() {
    uint8_t v = data->encoder_rate_multiplier;
    if (v == 0) v = 1;  // minimum value is 1; treat 0 the same as 1
    return v;
}
void set_encoder_rate_multiplier(uint8_t v) {
    data->encoder_rate_multiplier = v;
}

bool encoder_dial_direction() {
    return data->encoder_dial_direction;
}
void set_encoder_dial_direction(bool v) {
    data->encoder_dial_direction = v;
}

// Recovery mode magic value storage
static data_t* data_direct_access = reinterpret_cast<data_t*>(memory::map::backup_ram.base());

uint32_t config_mode_storage_direct() {
    return data_direct_access->config_mode_storage;
}
void set_config_mode_storage_direct(uint32_t v) {
    data_direct_access->config_mode_storage = v;
}
bool config_disable_config_mode_direct() {
    // "return data_direct_access->misc_config.config_disable_config_mode"
    // Casting as U32 as workaround for misaligned memory access
    uint32_t misc_config_u32 = *(uint32_t*)&data_direct_access->misc_config;
    return ((misc_config_u32 & MC_CONFIG_DISABLE_CONFIG_MODE) != 0);
}

// Daylight savings time
bool dst_enabled() {
    return data->dst_config.b.dst_enabled;
}
void set_dst_enabled(bool v) {
    data->dst_config.b.dst_enabled = v;
    rtc_time::dst_init();
}
dst_config_t config_dst() {
    return data->dst_config;
}
void set_config_dst(dst_config_t v) {
    data->dst_config = v;
    rtc_time::dst_init();
}

// Fake brightness level (switch is in another place)
uint8_t fake_brightness_level() {
    return data->fake_brightness_level;
}
void set_fake_brightness_level(uint8_t v) {
    data->fake_brightness_level = v;
}

// Cycle through 4 brightness options: disabled -> enabled/50% -> enabled/25% -> enabled/12.5% -> disabled
void toggle_fake_brightness_level() {
    bool fbe = apply_fake_brightness();
    if ((!fbe) || (data->fake_brightness_level >= BRIGHTNESS_12p5)) {
        set_apply_fake_brightness(!fbe);
        data->fake_brightness_level = BRIGHTNESS_50;
    } else {
        data->fake_brightness_level++;
    }
}

// Menu Color Scheme
Color menu_color() {
    return data->menu_color;
}
void set_menu_color(Color v) {
    data->menu_color = v;
}

uint16_t touchscreen_threshold() {
    return data->touchscreen_threshold;
}
void set_touchscreen_threshold(uint16_t v) {
    data->touchscreen_threshold = v;
}

// PMem to sdcard settings

bool should_use_sdcard_for_pmem() {
    return std::filesystem::file_exists(settings_dir / PMEM_FILEFLAG);
}

int save_persistent_settings_to_file() {
    File outfile;

    ensure_directory(settings_dir);
    auto error = outfile.create(settings_dir / PMEM_SETTING_FILE);
    if (error)
        return false;

    outfile.write(reinterpret_cast<char*>(&cached_backup_ram), sizeof(backup_ram_t));
    return true;
}

int load_persistent_settings_from_file() {
    File infile;
    auto error = infile.open(settings_dir / PMEM_SETTING_FILE);
    if (error)
        return false;

    infile.read(reinterpret_cast<char*>(&cached_backup_ram), sizeof(backup_ram_t));
    return true;
}

// Pmem size helper

size_t data_size() {
    return sizeof(data_t);
}

} /* namespace persistent_memory */
} /* namespace portapack */
