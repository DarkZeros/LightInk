#include "peripherals.h"
#include "hardware.h"
#include "power.h"

#include "driver/gpio.h"
#include "Arduino.h"

void Peripherals::vibrator(const std::vector<int>& pattern) {
  constexpr const gpio_config_t kConf = {
    .pin_bit_mask = (1ULL<<HW::kVibratorPin),
    .mode = GPIO_MODE_OUTPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_DISABLE,
  };

  //Configure GPIO with the given settings
  gpio_config(&kConf);

  for(auto i = 0; i < pattern.size(); i++) {
    gpio_set_level((gpio_num_t)HW::kVibratorPin, i % 2 ? 0 : 1);
    delay(pattern[i]);
  }
  gpio_set_level((gpio_num_t)HW::kVibratorPin, 0);
}

#include "driver/ledc.h"
#include "soc/ledc_struct.h"

struct Speaker {
  constexpr static auto kTimer = LEDC_TIMER_0;
  constexpr static auto kChannel = LEDC_CHANNEL_0;
  constexpr static auto kSpeedMode = LEDC_LOW_SPEED_MODE;
  constexpr static auto kClock = LEDC_USE_RC_FAST_CLK;

  Power::Lock powerLock = Power::Lock(Power::Flag::Speaker);

  // Select resolution based on the frequency values, not to overflow or underflow divisor
  static ledc_timer_bit_t calc_resolution(uint32_t freq) {
    if (freq < 64) return LEDC_TIMER_10_BIT;
    if (freq < 128) return LEDC_TIMER_9_BIT;
    if (freq < 256) return LEDC_TIMER_8_BIT;
    if (freq < 512) return LEDC_TIMER_7_BIT;
    if (freq < 1024) return LEDC_TIMER_6_BIT;
    if (freq < 2 * 1024) return LEDC_TIMER_5_BIT;
    if (freq < 4 * 1024) return LEDC_TIMER_4_BIT;
    if (freq < 8 * 1024) return LEDC_TIMER_3_BIT;
    if (freq < 16 * 1024) return LEDC_TIMER_2_BIT;
    return LEDC_TIMER_1_BIT;
  }

  Speaker() {
    // Enable light sleep while Speaker active
    esp_sleep_pd_config(ESP_PD_DOMAIN_RC_FAST, ESP_PD_OPTION_ON);
    gpio_sleep_sel_dis((gpio_num_t)HW::kSpeakerPin);

    // Set up channel config, never changes
    constexpr ledc_channel_config_t ledc_channel = {
        .gpio_num = (gpio_num_t)HW::kSpeakerPin,
        .speed_mode = kSpeedMode,
        .channel = kChannel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = kTimer,
        .duty = 0,
        .hpoint = 0,
        .sleep_mode = LEDC_SLEEP_MODE_KEEP_ALIVE,
        .flags = {
          .output_invert = 0,
        }
    };
    ledc_channel_config(&ledc_channel);

    // ledc_timer_config_t ledc_timer = {
    //   .speed_mode = kSpeedMode,
    //   .duty_resolution = LEDC_TIMER_2_BIT, // calc_resolution(freq),
    //   .timer_num = kTimer,
    //   .freq_hz = 0, // Set output frequency
    //   .clk_cfg = kClock,
    //   .deconfigure = false,
    // };
    // ledc_timer_config(&ledc_timer);
  }
  ~Speaker() {
    stop();
    esp_sleep_pd_config(ESP_PD_DOMAIN_RC_FAST, ESP_PD_OPTION_AUTO);
  }

  void set(uint32_t freq) {
    if (freq == 0) {
      stop();
      return;
    }
    // Update timer + duty
    const auto dutyBit = calc_resolution(freq);
    uint32_t halfDuty = 1 << (dutyBit - 1);

    ledc_stop(kSpeedMode, kChannel, 0);  // Clear duty_start / sig_out_en first

    ledc_timer_config_t ledc_timer = {
      .speed_mode = kSpeedMode,
      .duty_resolution = dutyBit,
      .timer_num = kTimer,
      .freq_hz = freq, // Set output frequency
      .clk_cfg = kClock,
      .deconfigure = false,
    };
    ledc_timer_config(&ledc_timer);

    // // ledc_set_freq(kSpeedMode, kTimer, freq);
    // ledc_set_duty(kSpeedMode, kChannel, halfDuty);
    // ledc_update_duty(kSpeedMode, kChannel);
    // // ledc_ls_channel_update(kSpeedMode, kChannel);
    // // ledc_hal_ls_channel_update(&(p_ledc_obj[kSpeedMode]->ledc_hal), channel);

    // // ledc_set_freq(kSpeedMode, kTimer, freq);
    // // ledc_set_duty_and_update(kSpeedMode, kChannel, halfDuty, 0);

    // Directly write registers, bypassing the duty_start spin-wait
    LEDC.channel_group[kSpeedMode].channel[kChannel].duty.duty = halfDuty << 4;
    LEDC.channel_group[kSpeedMode].channel[kChannel].conf0.sig_out_en = 1;
    LEDC.channel_group[kSpeedMode].channel[kChannel].conf1.duty_inc = 1;
    LEDC.channel_group[kSpeedMode].channel[kChannel].conf1.duty_num = 1;
    LEDC.channel_group[kSpeedMode].channel[kChannel].conf1.duty_cycle = 1;
    LEDC.channel_group[kSpeedMode].channel[kChannel].conf1.duty_scale = 0;
    // Trigger low-speed channel update (no spin-wait)
    LEDC.channel_group[kSpeedMode].channel[kChannel].conf0.low_speed_update = 1;
  }

  void stop() {
    ledc_stop(kSpeedMode, kChannel, 0);
  }
};

void Peripherals::speaker(const std::vector<std::pair<int, int>>& pattern) {
  Speaker speaker;

  for(auto& [note, duration] : pattern) {
    speaker.set(note);
    delay(duration);
  }
}


#define NOTE_B0  31
#define NOTE_C1  33
#define NOTE_CS1 35
#define NOTE_D1  37
#define NOTE_DS1 39
#define NOTE_E1  41
#define NOTE_F1  44
#define NOTE_FS1 46
#define NOTE_G1  49
#define NOTE_GS1 52
#define NOTE_A1  55
#define NOTE_AS1 58
#define NOTE_B1  62
#define NOTE_C2  65
#define NOTE_CS2 69
#define NOTE_D2  73
#define NOTE_DS2 78
#define NOTE_E2  82
#define NOTE_F2  87
#define NOTE_FS2 93
#define NOTE_G2  98
#define NOTE_GS2 104
#define NOTE_A2  110
#define NOTE_AS2 117
#define NOTE_B2  123
#define NOTE_C3  131
#define NOTE_CS3 139
#define NOTE_D3  147
#define NOTE_DS3 156
#define NOTE_E3  165
#define NOTE_F3  175
#define NOTE_FS3 185
#define NOTE_G3  196
#define NOTE_GS3 208
#define NOTE_A3  220
#define NOTE_AS3 233
#define NOTE_B3  247
#define NOTE_C4  262
#define NOTE_CS4 277
#define NOTE_D4  294
#define NOTE_DS4 311
#define NOTE_E4  330
#define NOTE_F4  349
#define NOTE_FS4 370
#define NOTE_G4  392
#define NOTE_GS4 415
#define NOTE_A4  440
#define NOTE_AS4 466
#define NOTE_B4  494
#define NOTE_C5  523
#define NOTE_CS5 554
#define NOTE_D5  587
#define NOTE_DS5 622
#define NOTE_E5  659
#define NOTE_F5  698
#define NOTE_FS5 740
#define NOTE_G5  784
#define NOTE_GS5 831
#define NOTE_A5  880
#define NOTE_AS5 932
#define NOTE_B5  988
#define NOTE_C6  1047
#define NOTE_CS6 1109
#define NOTE_D6  1175
#define NOTE_DS6 1245
#define NOTE_E6  1319
#define NOTE_F6  1397
#define NOTE_FS6 1480
#define NOTE_G6  1568
#define NOTE_GS6 1661
#define NOTE_A6  1760
#define NOTE_AS6 1865
#define NOTE_B6  1976
#define NOTE_C7  2093
#define NOTE_CS7 2217
#define NOTE_D7  2349
#define NOTE_DS7 2489
#define NOTE_E7  2637
#define NOTE_F7  2794
#define NOTE_FS7 2960
#define NOTE_G7  3136
#define NOTE_GS7 3322
#define NOTE_A7  3520
#define NOTE_AS7 3729
#define NOTE_B7  3951
#define NOTE_C8  4186
#define NOTE_CS8 4435
#define NOTE_D8  4699
#define NOTE_DS8 4978
#define REST 0

// change this to make the song slower or faster
int tempo=144; 

// change this to whichever pin you want to use
int buzzer = 11;

// notes of the moledy followed by the duration.
// a 4 means a quarter note, 8 an eighteenth , 16 sixteenth, so on
// !!negative numbers are used to represent dotted notes,
// so -4 means a dotted quarter note, that is, a quarter plus an eighteenth!!
int melody[] = {

  //Based on the arrangement at https://www.flutetunes.com/tunes.php?id=192
  
  NOTE_E5, 4,  NOTE_B4,8,  NOTE_C5,8,  NOTE_D5,4,  NOTE_C5,8,  NOTE_B4,8,
  NOTE_A4, 4,  NOTE_A4,8,  NOTE_C5,8,  NOTE_E5,4,  NOTE_D5,8,  NOTE_C5,8,
  NOTE_B4, -4,  NOTE_C5,8,  NOTE_D5,4,  NOTE_E5,4,
  NOTE_C5, 4,  NOTE_A4,4,  NOTE_A4,8,  NOTE_A4,4,  NOTE_B4,8,  NOTE_C5,8,

  NOTE_D5, -4,  NOTE_F5,8,  NOTE_A5,4,  NOTE_G5,8,  NOTE_F5,8,
  NOTE_E5, -4,  NOTE_C5,8,  NOTE_E5,4,  NOTE_D5,8,  NOTE_C5,8,
  NOTE_B4, 4,  NOTE_B4,8,  NOTE_C5,8,  NOTE_D5,4,  NOTE_E5,4,
  NOTE_C5, 4,  NOTE_A4,4,  NOTE_A4,4, REST, 4,

  NOTE_E5, 4,  NOTE_B4,8,  NOTE_C5,8,  NOTE_D5,4,  NOTE_C5,8,  NOTE_B4,8,
  NOTE_A4, 4,  NOTE_A4,8,  NOTE_C5,8,  NOTE_E5,4,  NOTE_D5,8,  NOTE_C5,8,
  NOTE_B4, -4,  NOTE_C5,8,  NOTE_D5,4,  NOTE_E5,4,
  NOTE_C5, 4,  NOTE_A4,4,  NOTE_A4,8,  NOTE_A4,4,  NOTE_B4,8,  NOTE_C5,8,

  NOTE_D5, -4,  NOTE_F5,8,  NOTE_A5,4,  NOTE_G5,8,  NOTE_F5,8,
  NOTE_E5, -4,  NOTE_C5,8,  NOTE_E5,4,  NOTE_D5,8,  NOTE_C5,8,
  NOTE_B4, 4,  NOTE_B4,8,  NOTE_C5,8,  NOTE_D5,4,  NOTE_E5,4,
  NOTE_C5, 4,  NOTE_A4,4,  NOTE_A4,4, REST, 4,
  

  NOTE_E5,2,  NOTE_C5,2,
  NOTE_D5,2,   NOTE_B4,2,
  NOTE_C5,2,   NOTE_A4,2,
  NOTE_GS4,2,  NOTE_B4,4,  REST,8, 
  NOTE_E5,2,   NOTE_C5,2,
  NOTE_D5,2,   NOTE_B4,2,
  NOTE_C5,4,   NOTE_E5,4,  NOTE_A5,2,
  NOTE_GS5,2,

};

void Peripherals::tetris() {
  Speaker speaker;
  int notes=sizeof(melody)/sizeof(melody[0])/2; 

  // this calculates the duration of a whole note in ms (60s/tempo)*4 beats
  int wholenote = (60000 * 4) / tempo;

  int divider = 0, noteDuration = 0;
  for (int thisNote = 0; thisNote < notes * 2; thisNote = thisNote + 2) {

    // calculates the duration of each note
    divider = melody[thisNote + 1];
    if (divider > 0) {
      // regular note, just proceed
      noteDuration = (wholenote) / divider;
    } else if (divider < 0) {
      // dotted notes are represented with negative durations!!
      noteDuration = (wholenote) / abs(divider);
      noteDuration *= 1.5; // increases the duration in half for dotted notes
    }

    // we only play the note for 90% of the duration, leaving 10% as a pause
    speaker.set(melody[thisNote]);
    delay(noteDuration * 0.9);

    speaker.set(0);
    delay(noteDuration * 0.1);
  }
}