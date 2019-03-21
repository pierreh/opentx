#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2s.h"
#include "esp_log.h"
#define HASASSERT
#include "opentx.h"

static const char *TAG = "audio_driver.cpp";


void initAudio(){
    ESP_LOGI(TAG,"initAudio()");
    i2s_config_t i2s_config;
    memset(&i2s_config, 0, sizeof(i2s_config));
    i2s_config.mode =(i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_DAC_BUILT_IN);
    i2s_config.sample_rate = AUDIO_SAMPLE_RATE;
    i2s_config.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT; /* the DAC module will only take the 8bits from MSB */
    i2s_config.channel_format = I2S_CHANNEL_FMT_ALL_LEFT;
    i2s_config.communication_format = I2S_COMM_FORMAT_I2S_LSB;
    i2s_config.intr_alloc_flags = 0; // default interrupt priority
    i2s_config.dma_buf_count = 2;
    i2s_config.dma_buf_len = 1024;
    i2s_config.use_apll = false;

    esp_err_t err = i2s_driver_install(I2S_NUM_0,&i2s_config,0,NULL);
    if (err != ESP_OK) {
    // handle other errors
        ESP_LOGE(TAG,"%s",esp_err_to_name(err));
    }
    //You can call i2s_set_dac_mode to set built-in DAC output mode.
    i2s_set_dac_mode(I2S_DAC_CHANNEL_LEFT_EN);
}

void setSampleRate(uint32_t frequency){
    ESP_LOGI(TAG,"setSampleRate: %d",frequency);
//    i2s_set_sample_rates(I2S_NUM_0, frequency);
    i2s_set_clk(I2S_NUM_0, frequency, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
}

