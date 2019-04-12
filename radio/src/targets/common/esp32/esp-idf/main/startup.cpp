#include <stdio.h>
#include "esp_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_task.h"
#include "soc/timer_group_struct.h"
#include "driver/periph_ctrl.h"
#include "driver/timer.h"
#include "esp_system.h"
#include "esp_timer.h"
//#define LOG_LOCAL_LEVEL ESP_LOG_DEBUG
#include "esp_log.h"
#include "esp_spiffs.h"
#define HASASSERT
#include "opentx.h"



#define MENUS_STACK_SIZE       0xA00
#define MIXER_STACK_SIZE       0x800
#define AUDIO_STACK_SIZE       0x800
#define AUDIO_PLAY_STACK_SIZE  0xA00
#define PER10MS_STACK_SIZE     0x500
#define ENC_STACK_SIZE         0x800
#define MENU_TASK_PERIOD_TICKS      50/portTICK_PERIOD_MS    // 50ms
#define MENU_TASK_CORE 0
#define MIXER_TASK_CORE 1
#define AUDIO_TASK_CORE 0
#define PER10MS_TASK_CORE 0
#define ENC_TASK_CORE 0

static const char *TAG = "startup.cpp";
TaskHandle_t xMenusTaskHandle = NULL;
TaskHandle_t xMixerTaskHandle = NULL;
TaskHandle_t xAudioTaskHandle = NULL;
TaskHandle_t xPer10msTaskHandle = NULL;
TaskHandle_t xEncTaskHandle = NULL;

SemaphoreHandle_t xAudioSem = NULL;
SemaphoreHandle_t xPer10msSem = NULL;
extern SemaphoreHandle_t xPPMSem;
//uint16_t testDuration;

uint16_t menusStackAvailable()
{
    return uxTaskGetStackHighWaterMark(xMenusTaskHandle);
}

uint16_t mixerStackAvailable()
{
    return uxTaskGetStackHighWaterMark(xMixerTaskHandle);
}

uint16_t per10msStackAvailable()
{
    return uxTaskGetStackHighWaterMark(xPer10msTaskHandle);
}

uint16_t audioStackAvailable()
{
    return uxTaskGetStackHighWaterMark(xAudioTaskHandle);
}


uint16_t getTmr2MHz()
{
    return ((uint16_t) esp_timer_get_time())*2;
}

void menusTask(void * pvParameters)
{
    TickType_t xLastWakeTime;
    const TickType_t xTimeIncrement = MENU_TASK_PERIOD_TICKS;

    ESP_LOGI(TAG,"Starting menusTask.");
    opentxInit();

    xLastWakeTime = xTaskGetTickCount ();
    while (1) {
        vTaskDelayUntil( &xLastWakeTime, xTimeIncrement );
        perMain();
    }
    DEBUG_TIMER_STOP(debugTimerPerMain);


#if defined(SIMU)
    if (main_thread_running == 0)
    break;
#endif
}

void mixerTask(void * pdata)
{
    static uint32_t lastRunTime;
    s_pulses_paused = true;
    ESP_LOGI(TAG,"Starting mixerTask.\n");
    while(1) {

#if defined(SIMU)
        if (main_thread_running == 0)
        return;
#endif

#if defined(SBUS)
        processSbusInput();
#endif

        xSemaphoreTake( xPPMSem, 20/portTICK_PERIOD_MS); // run at least every 20ms
        lastRunTime = esp_timer_get_time()/1000;
        //    if (isForcePowerOffRequested()) {
        //      pwrOff();
        //    }
        if (!s_pulses_paused) {
            int64_t t0 = esp_timer_get_time();

            DEBUG_TIMER_START(debugTimerMixer);
            doMixerCalculations();
            DEBUG_TIMER_START(debugTimerMixerCalcToUsage);
            DEBUG_TIMER_SAMPLE(debugTimerMixerIterval);
            DEBUG_TIMER_STOP(debugTimerMixer);

#if defined(TELEMETRY_FRSKY) || defined(TELEMETRY_MAVLINK)
            DEBUG_TIMER_START(debugTimerTelemetryWakeup);
            telemetryWakeup();
            DEBUG_TIMER_STOP(debugTimerTelemetryWakeup);
#endif

#if defined(BLUETOOTH)
            bluetoothWakeup();
#endif

            if (heartbeat == HEART_WDT_CHECK) {
                wdt_reset();
                heartbeat = 0;
            }

            t0 = esp_timer_get_time() - t0;
            if (t0 > maxMixerDuration) maxMixerDuration = t0 ;
        }
    }
}


void  per10msTask(void * pdata)
{
    while(1) {
        xSemaphoreTake(xPer10msSem, portMAX_DELAY);
        //        uint32_t now = esp_timer_get_time();
        per10ms();
        //        testDuration = (uint16_t)(esp_timer_get_time()-now);
    }
}

void tasksStart()
{
    BaseType_t ret;

    ESP_LOGI(TAG,"Starting tasks.");
    xAudioSem = xSemaphoreCreateMutex();
    if( xAudioSem == NULL ) {
        ESP_LOGE(TAG,"Failed to create semaphore: xPer10msSem.");
    } else {
        ret=xTaskCreatePinnedToCore( audioTask, "audioTask", AUDIO_STACK_SIZE, NULL, ESP_TASK_PRIO_MAX -8, &xAudioTaskHandle, AUDIO_TASK_CORE );
        configASSERT( xAudioTaskHandle );
    }

    xPPMSem = xSemaphoreCreateMutex();
    if( xPPMSem == NULL ) {
        ESP_LOGE(TAG,"Failed to create semaphore: xPPMSem.");
    }
    ret=xTaskCreatePinnedToCore( menusTask, "menusTask", MENUS_STACK_SIZE, NULL, ESP_TASK_PRIO_MAX -9, &xMenusTaskHandle, MENU_TASK_CORE );
    configASSERT( xMenusTaskHandle );

    ret=xTaskCreatePinnedToCore( mixerTask, "mixerTask", MIXER_STACK_SIZE, NULL, ESP_TASK_PRIO_MAX -6, &xMixerTaskHandle, MIXER_TASK_CORE );
    configASSERT( xMixerTaskHandle );

    ret=xTaskCreatePinnedToCore( per10msTask, "per10msTask", PER10MS_STACK_SIZE, NULL, ESP_TASK_PRIO_MAX -5, &xPer10msTaskHandle, PER10MS_TASK_CORE );
    configASSERT( xPer10msTaskHandle );

    ret=xTaskCreatePinnedToCore( encoderTask, "encoderTask", ENC_STACK_SIZE, NULL, ESP_TASK_PRIO_MAX -4, &xEncTaskHandle, ENC_TASK_CORE );
    configASSERT( xEncTaskHandle );
}

void IRAM_ATTR timer_group0_isr(void *para)
{
    int timer_idx = (int) para;

    /* Retrieve the interrupt status and the counter value
    from the timer that reported the interrupt */
    uint32_t intr_status = TIMERG0.int_st_timers.val;
    TIMERG0.hw_timer[timer_idx].update = 1;

    /* Clear the interrupt
    and update the alarm time for the timer with reload */
    if ((intr_status & BIT(timer_idx)) && timer_idx == TIMER_0) {
        TIMERG0.int_clr_timers.t0 = 1;
    } else if ((intr_status & BIT(timer_idx)) && timer_idx == TIMER_1) {
        TIMERG0.int_clr_timers.t1 = 1;
    }

    /* After the alarm has been triggered
    we need enable it again, so it is triggered the next time */
    TIMERG0.hw_timer[timer_idx].config.alarm_en = TIMER_ALARM_EN;

    //    if(NULL!=xPer10msSem){
    BaseType_t mustYield=false;
    xSemaphoreGiveFromISR(xPer10msSem, &mustYield);
    if (mustYield) portYIELD_FROM_ISR();
    //    }
}

static void tg0_timer_init(timer_idx_t timer_idx)
{
    /* Select and initialize basic parameters of the timer */
    timer_config_t config;
    memset(&config, 0, sizeof(config));
    config.divider = 16; // TIMER_BASE_CLK/16
    config.counter_dir = TIMER_COUNT_UP;
    config.counter_en = TIMER_PAUSE;
    config.alarm_en = TIMER_ALARM_EN;
    config.intr_type = TIMER_INTR_LEVEL;
    config.auto_reload = 1;
    timer_init(TIMER_GROUP_0, timer_idx, &config);

    /* Timer's counter will initially start from value below.
    Also, if auto_reload is set, this value will be automatically reload on alarm */
    timer_set_counter_value(TIMER_GROUP_0, timer_idx, 0x00000000ULL);

    /* Configure the alarm value and the interrupt on alarm. */
    timer_set_alarm_value(TIMER_GROUP_0, timer_idx, TIMER_BASE_CLK/(16*100)); //100Hz
    timer_enable_intr(TIMER_GROUP_0, timer_idx);
    timer_isr_register(TIMER_GROUP_0, timer_idx, timer_group0_isr,
    (void *) timer_idx, ESP_INTR_FLAG_IRAM, NULL);

    xPer10msSem = xSemaphoreCreateBinary();
    if( xPer10msSem == NULL ) {
        ESP_LOGE(TAG,"Failed to create semaphore: xPer10msSem.");
        return;
    }
    timer_start(TIMER_GROUP_0, timer_idx);
}

void timer10msInit()
{
    ESP_LOGI(TAG,"Starting 10ms timer.");
    tg0_timer_init(TIMER_0); //10 ms interrupt
}

void espLogI(const char * format, ...)
{
    va_list arglist;
    va_start(arglist, format);
    vprintf( format, arglist);
    va_end(arglist);
}

char g_ssid[sizeof(g_eeGeneral.ssid)];
char g_passwd[sizeof(g_eeGeneral.passwd)];

#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && defined(CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS)

void vTaskGetRunTimeStatsA( )
{
    TaskStatus_t *pxTaskStatusArray;
    volatile UBaseType_t uxArraySize, x;
    uint32_t ulTotalRunTime, ulStatsAsPercentage;

    // Take a snapshot of the number of tasks in case it changes while this
    // function is executing.
    uxArraySize = uxTaskGetNumberOfTasks();

    // Allocate a TaskStatus_t structure for each task.  An array could be
    // allocated statically at compile time.
    pxTaskStatusArray = (TaskStatus_t *) pvPortMalloc( uxArraySize * sizeof( TaskStatus_t ) );

    if( pxTaskStatusArray != NULL )
    {
        // Generate raw status information about each task.
        uxArraySize = uxTaskGetSystemState( pxTaskStatusArray, uxArraySize, &ulTotalRunTime );

        // For percentage calculations.
        ulTotalRunTime /= 100UL;

        // Avoid divide by zero errors.
        if( ulTotalRunTime > 0 )
        {
            // For each populated position in the pxTaskStatusArray array,
            // format the raw data as human readable ASCII data
            for( x = 0; x < uxArraySize; x++ )
            {
                // What percentage of the total run time has the task used?
                // This will always be rounded down to the nearest integer.
                // ulTotalRunTimeDiv100 has already been divided by 100.
                ulStatsAsPercentage = pxTaskStatusArray[ x ].ulRunTimeCounter / ulTotalRunTime;

                if( ulStatsAsPercentage > 0UL )
                {
                    ESP_LOGI(TAG, "stat: %-16s\t%12u\t%u%%", pxTaskStatusArray[ x ].pcTaskName, pxTaskStatusArray[ x ].ulRunTimeCounter, ulStatsAsPercentage );
                }
                else
                {
                    // If the percentage is zero here then the task has
                    // consumed less than 1% of the total run time.
                    ESP_LOGI(TAG, "stat: %-16s\t%12u\t<1%%", pxTaskStatusArray[ x ].pcTaskName, pxTaskStatusArray[ x ].ulRunTimeCounter );
                }
            }
        }

        // The array is no longer needed, free the memory it consumes.
        vPortFree( pxTaskStatusArray );
    }
}
#endif
int main();

extern "C"   void app_main()
{
    main();
    /*    initFS();
        mountSDCard();
        initWiFi();
        startWiFi(g_eeGeneral.ssid,g_eeGeneral.passwd,g_eeGeneral.ftppass);
    */
    TaskHandle_t tasks[]= {xMenusTaskHandle,xMixerTaskHandle,xAudioTaskHandle,xPer10msTaskHandle,xEncTaskHandle};
    uint8_t nTasks= sizeof(tasks)/sizeof(tasks[0]);
    while(1) {
        //        isWiFiStarted();
        ESP_LOGD(TAG,"s_pulses_paused: %d",s_pulses_paused);
        for(uint8_t i=0; i< nTasks; i++) {
            ESP_LOGD(TAG,"Min stack: %s: %d",pcTaskGetTaskName(tasks[i]),uxTaskGetStackHighWaterMark(tasks[i]));
        }
        ESP_LOGI(TAG,"maxMixerDuration: %d us.",maxMixerDuration);
        //        ESP_LOGI(TAG,"last 10ms task duration %d",testDuration);
#if defined(CONFIG_FREERTOS_USE_TRACE_FACILITY) && defined(CONFIG_FREERTOS_USE_STATS_FORMATTING_FUNCTIONS)        
        ESP_LOGI(TAG,"");
        vTaskGetRunTimeStatsA();

#endif

        vTaskDelay(5000/portTICK_PERIOD_MS);
    };
}
