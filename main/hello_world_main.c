// Esteira Industrial (ESP32 + FreeRTOS) — 4 tasks + Touch (polling) + Instrumentação RT
// - ENC_SENSE (periódica 5 ms) -> notifica SPD_CTRL
// - SPD_CTRL (hard RT) -> controle PI simulado, trata HMI (soft) se solicitado
// - SORT_ACT (hard RT, evento Touch B) -> aciona "desviador"
// - SAFETY_TASK (hard RT, evento Touch D) -> E-stop
// - TOUCH (polling) e UART para injetar eventos
// - STATS imprime métricas RT: releases, hard_miss, Cmax, Lmax (evento->start), Rmax (evento->end)

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "esp_timer.h"
#include "esp_log.h"
#include "esp_mac.h"

#include "driver/gpio.h"
#include "driver/touch_pad.h"   // legacy API (5.x)
#include "driver/uart.h"

// ========= WIFI / SNTP =========
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include <sys/time.h>
#include <time.h>

// ===== MONITOR_TASK: sockets =====
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#define TAG "ESTEIRA"
// static const char *TTAG = "TIME"; // (opcional) deixei comentado para não avisar unused

#define WIFI_SSID "Tourinho_2.4GHz"
#define WIFI_PASS "@Leonardo18"

#define MON_MODE_UDP   1
#define MON_MODE_TCP   2
#ifndef MON_MODE
#define MON_MODE       MON_MODE_TCP
#endif

// Ajuste para seu PC (ipconfig/ifconfig):
#define PC_IP          "192.168.100.173"
#define PC_UDP_PORT    6010
#define ESP_UDP_RXPORT 6010
#define TCP_LISTEN_PORT 5000

// ====== Touch pads (ESP32 WROOM): T7=GPIO27, T8=GPIO33, T9=GPIO32 ======
#define TP_OBJ   TOUCH_PAD_NUM7   // B -> objeto
#define TP_HMI   TOUCH_PAD_NUM8   // C -> HMI
#define TP_ESTOP TOUCH_PAD_NUM9   // D -> E-stop

// ====== Períodos, prioridades, stack ======
#define ENC_T_MS        5
#define PRIO_ESTOP      5
#define PRIO_ENC        4
#define PRIO_CTRL       3
#define PRIO_SORT       3
#define PRIO_TOUCH      2
#define PRIO_STATS      1
#define STK_MAIN        4096
#define STK_AUX         4096

// ====== Deadlines (us) ======
#define D_ENC_US    5000
#define D_CTRL_US  10000
#define D_SORT_US  10000
#define D_SAFE_US   5000

// ======== SNTP: âncora de tempo ========
static volatile bool   g_time_synced = false;
static int64_t         g_sync_epoch_us = 0;
static int64_t         g_sync_esp_us   = 0;

static void time_sync_notification_cb(struct timeval *tv) {
    g_time_synced   = true;
    g_sync_epoch_us = (int64_t)tv->tv_sec * 1000000LL + (int64_t)tv->tv_usec;
    g_sync_esp_us   = esp_timer_get_time();
}

static void sntp_start_and_wait(void) {
    sntp_setoperatingmode(SNTP_OPMODE_POLL);
    sntp_setservername(0, "pool.ntp.org");
    sntp_set_sync_mode(SNTP_SYNC_MODE_SMOOTH);
    sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    sntp_init();

    for (int i = 0; i < 50 && !g_time_synced; ++i) {
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static inline int64_t now_epoch_us_sntp(void) {
    if (g_time_synced) {
        int64_t delta = esp_timer_get_time() - g_sync_esp_us;
        return g_sync_epoch_us + (delta < 0 ? 0 : delta);
    } else {
        struct timeval tv; gettimeofday(&tv, NULL);
        return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
    }
}

static inline void log_now_str_sntp(char *out, size_t n) {
    int64_t us = now_epoch_us_sntp();
    time_t  sec = (time_t)(us / 1000000LL);
    struct tm tm_local;
    localtime_r(&sec, &tm_local);
    strftime(out, n, "%d/%m/%y %H : %M : %S", &tm_local);
}

// ========= WiFi =========
static void wifi_init(void){
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    wifi_config_t wc = {0};
    strcpy((char*)wc.sta.ssid, WIFI_SSID);
    strcpy((char*)wc.sta.password, WIFI_PASS);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_connect());
}

// ====== Estado da esteira ======
typedef struct {
    float rpm;
    float pos_mm;
    float set_rpm;
} belt_state_t;

static belt_state_t g_belt = { .rpm = 0.f, .pos_mm = 0.f, .set_rpm = 120.0f };

// ====== Instrumentação ======
typedef struct {
    volatile uint32_t releases, starts, finishes;
    volatile uint32_t hard_miss, soft_miss;
    volatile int64_t  last_release_us, last_start_us, last_end_us;
    volatile int64_t  worst_exec_us, worst_latency_us, worst_response_us;
} rt_stats_t;

static rt_stats_t st_enc  = {0};
static rt_stats_t st_ctrl = {0};
static rt_stats_t st_sort = {0};
static rt_stats_t st_safe = {0};

// ====== IPC / handles ======
static TaskHandle_t hENC = NULL, hCTRL = NULL, hSORT = NULL, hSAFE = NULL;
static TaskHandle_t hCtrlNotify = NULL; // ENC -> CTRL
typedef struct { int64_t t_evt_us; } sort_evt_t;
static QueueHandle_t qSort = NULL;
static SemaphoreHandle_t semEStop = NULL;
static SemaphoreHandle_t semHMI   = NULL;

static volatile int64_t g_last_touchB_us = 0;
static volatile int64_t g_last_touchD_us = 0;
static volatile int64_t g_last_enc_release_us = 0;

// ===== Util =====
static inline void stats_on_release(rt_stats_t *s, int64_t t_rel) {
    s->releases++;
    s->last_release_us = t_rel;
}
static inline void stats_on_start(rt_stats_t *s, int64_t t_start) {
    s->starts++;
    s->last_start_us = t_start;
    int64_t lat = t_start - s->last_release_us;
    if (lat > s->worst_latency_us) s->worst_latency_us = lat;
}
static inline void stats_on_finish(rt_stats_t *s, int64_t t_end, int64_t D_us, bool hard) {
    s->finishes++;
    s->last_end_us = t_end;
    int64_t exec = t_end - s->last_start_us;
    if (exec > s->worst_exec_us) s->worst_exec_us = exec;
    int64_t resp = t_end - s->last_release_us;
    if (resp > s->worst_response_us) s->worst_response_us = resp;
    if (resp > D_us) {
        if (hard) s->hard_miss++; else s->soft_miss++;
    }
}

static inline void cpu_tight_loop_us(uint32_t us) {
    int64_t start = esp_timer_get_time();
    while ((esp_timer_get_time() - start) < us) { __asm__ __volatile__("nop"); }
}

static inline TickType_t ticks_from_ms(uint32_t ms) {
    TickType_t t = pdMS_TO_TICKS(ms);
    if (ms > 0 && t == 0) return 1;
    return t;
}

// ===== MONITOR_TASK: JSON de telemetria =====
static int g_seq = 0;
static inline int json_telemetry(char *buf, size_t n, const char *proto) {
    int64_t t_us = now_epoch_us_sntp();
    int len = snprintf(buf, n,
        "{\"seq\":%d,\"proto\":\"%s\",\"t_us\":%lld,"
        "\"rpm\":%.1f,\"set\":%.1f,\"pos\":%.1f,"
        "\"enc\":{\"Cmax\":%lld,\"Lmax\":%lld,\"Rmax\":%lld},"
        "\"ctrl\":{\"Cmax\":%lld,\"Lmax\":%lld,\"Rmax\":%lld},"
        "\"sort\":{\"Cmax\":%lld,\"Lmax\":%lld,\"Rmax\":%lld},"
        "\"safe\":{\"Cmax\":%lld,\"Lmax\":%lld,\"Rmax\":%lld}}",
        g_seq++, proto, (long long)t_us,
        g_belt.rpm, g_belt.set_rpm, g_belt.pos_mm,
        (long long)st_enc.worst_exec_us,  (long long)st_enc.worst_latency_us,  (long long)st_enc.worst_response_us,
        (long long)st_ctrl.worst_exec_us, (long long)st_ctrl.worst_latency_us, (long long)st_ctrl.worst_response_us,
        (long long)st_sort.worst_exec_us, (long long)st_sort.worst_latency_us, (long long)st_sort.worst_response_us,
        (long long)st_safe.worst_exec_us, (long long)st_safe.worst_latency_us, (long long)st_safe.worst_response_us
    );
    return (len < 0) ? 0 : len;
}

// ===== Comandos vindos do PC =====
static void handle_command_line(const char *line) {
    // Aceita: "SORT", "SAFE", "HMI", "SET 1234"
    if (strncasecmp(line, "SORT", 4) == 0) {
        g_last_touchB_us = esp_timer_get_time();
        sort_evt_t e = { g_last_touchB_us };
        (void)xQueueSend(qSort, &e, 0);
        ESP_LOGI(TAG, "CMD: SORT");
    } else if (strncasecmp(line, "SAFE", 4) == 0) {
        g_last_touchD_us = esp_timer_get_time();
        (void)xSemaphoreGive(semEStop);
        ESP_LOGW(TAG, "CMD: SAFE");
    } else if (strncasecmp(line, "HMI", 3) == 0) {
        (void)xSemaphoreGive(semHMI);
        ESP_LOGI(TAG, "CMD: HMI");
    } else if (strncasecmp(line, "SET", 3) == 0) {
        float v = g_belt.set_rpm;
        (void)sscanf(line + 3, "%f", &v);
        if (v < 0) { v = 0; }
        if (v > 5000) { v = 5000; }
        g_belt.set_rpm = v;
        ESP_LOGI(TAG, "CMD: SET %.1f", v);
    }
}

// ===== MONITOR_TASK (UDP client + RX UDP) =====
static void monitor_task_udp(void *arg) {
    int tx = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in pc = {0};
    pc.sin_family = AF_INET; pc.sin_port = htons(PC_UDP_PORT);
    inet_pton(AF_INET, PC_IP, &pc.sin_addr.s_addr);

    int rx = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    struct sockaddr_in me = {0};
    me.sin_family = AF_INET; me.sin_port = htons(ESP_UDP_RXPORT);
    me.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(rx, (struct sockaddr*)&me, sizeof(me));

    TickType_t last = xTaskGetTickCount();
    const TickType_t T = pdMS_TO_TICKS(500);
    char pay[256];
    char tbuf[32];

    while (1) {
        int len = json_telemetry(pay, sizeof(pay), "UDP");
        sendto(tx, pay, len, 0, (struct sockaddr*)&pc, sizeof(pc));

        log_now_str_sntp(tbuf, sizeof(tbuf));
        ESP_LOGI(TAG, "[%s ] MON: UDP TX -> %s:%d len=%d", tbuf, PC_IP, PC_UDP_PORT, len);

        // RX não-bloqueante
        struct timeval tv = { .tv_sec = 0, .tv_usec = 1000*10 };
        fd_set rfds; FD_ZERO(&rfds); FD_SET(rx, &rfds);
        int r = select(rx+1, &rfds, NULL, NULL, &tv);
        if (r > 0 && FD_ISSET(rx, &rfds)) {
            char buf[256]; struct sockaddr_in from; socklen_t flen = sizeof(from);
            int n = recvfrom(rx, buf, sizeof(buf)-1, 0, (struct sockaddr*)&from, &flen);
            if (n > 0) { buf[n] = 0; handle_command_line(buf); }
        }

        vTaskDelayUntil(&last, T);
    }
}

// ===== MONITOR_TASK (TCP server) =====
static void monitor_task_tcp(void *arg) {
    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET; addr.sin_port = htons(TCP_LISTEN_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr));
    listen(listen_fd, 1);
    ESP_LOGI(TAG, "MON: TCP servidor na porta %d", TCP_LISTEN_PORT);

    char pay[256], line[256], tbuf[32];
    const TickType_t T = pdMS_TO_TICKS(500);

    while (1) {
        struct sockaddr_in6 src; socklen_t sl = sizeof(src);
        int sock = accept(listen_fd, (struct sockaddr *)&src, &sl);
        if (sock < 0) continue;
        ESP_LOGI(TAG, "MON: cliente conectado (TCP)");
        const char *hello = "ESP32 MON pronto. Comandos: SORT | SAFE | HMI | SET <rpm>\n";
        send(sock, hello, strlen(hello), 0);

        TickType_t last = xTaskGetTickCount();
        while (1) {
            if (xTaskGetTickCount() - last >= T) {
                int len = json_telemetry(pay, sizeof(pay), "TCP");
                if (send(sock, pay, len, 0) <= 0) break;
                log_now_str_sntp(tbuf, sizeof(tbuf));
                ESP_LOGI(TAG, "[%s ] MON: TCP TX len=%d", tbuf, len);
                last = xTaskGetTickCount();
            }
            struct timeval tv = { .tv_sec = 0, .tv_usec = 1000*10 };
            fd_set rfds; FD_ZERO(&rfds); FD_SET(sock, &rfds);
            int r = select(sock+1, &rfds, NULL, NULL, &tv);
            if (r > 0 && FD_ISSET(sock, &rfds)) {
                int n = recv(sock, line, sizeof(line)-1, 0);
                if (n <= 0) break;
                line[n] = 0;
                handle_command_line(line);
                int len = snprintf(pay, sizeof(pay), "{\"ok\":true,\"echo\":\"%.*s\"}\n", n-1, line);
                if (len > 0) send(sock, pay, len, 0);
            }
        }
        ESP_LOGI(TAG, "MON: cliente saiu");
        shutdown(sock, 0); close(sock);
    }
}

// ===== seletor da MONITOR_TASK =====
static void task_monitor(void *arg) {
#if MON_MODE == MON_MODE_UDP
    monitor_task_udp(arg);
#else
    monitor_task_tcp(arg);
#endif
    vTaskDelete(NULL);
}

// ========= Prototipação das tasks =========
static void task_enc_sense(void *arg);
static void task_spd_ctrl(void *arg);
static void task_sort_act(void *arg);
static void task_safety(void *arg);
static void task_touch_poll(void *arg);
static void task_stats(void *arg);
static void task_uart_cmd(void *arg);

// (Opcional) Idle hook p/ %CPU
#if configUSE_IDLE_HOOK
static volatile int64_t idle_us_acc = 0;
void vApplicationIdleHook(void) {
    static int64_t last = 0;
    int64_t now = esp_timer_get_time();
    if (last) idle_us_acc += (now - last);
    last = now;
}
#endif

#if (configUSE_TRACE_FACILITY==1) && (configGENERATE_RUN_TIME_STATS==1)
static void print_runtime_stats(void) {
    static char buf[1024];
    vTaskGetRunTimeStats(buf);
    ESP_LOGI(TAG, "\nTask               Time(us)   %%CPU\n%s", buf);
}
#endif

/* ====== ENC_SENSE (5 ms) ====== */
static void task_enc_sense(void *arg)
{
    TickType_t next = xTaskGetTickCount();
    TickType_t T = ticks_from_ms(ENC_T_MS);

    for (;;) {
        int64_t t_rel = esp_timer_get_time();
        stats_on_release(&st_enc, t_rel);
        g_last_enc_release_us = t_rel;

        stats_on_start(&st_enc, esp_timer_get_time());

        // Dinâmica simulada
        float err = g_belt.set_rpm - g_belt.rpm;
        g_belt.rpm += 0.05f * err;
        g_belt.pos_mm += (g_belt.rpm / 60.0f) * (ENC_T_MS / 1000.0f) * 100.0f;

        if (g_belt.rpm < 0.0f)     g_belt.rpm = 0.0f;
        if (g_belt.rpm > 5000.0f)  g_belt.rpm = 5000.0f;

        cpu_tight_loop_us(700);

        stats_on_finish(&st_enc, esp_timer_get_time(), D_ENC_US, /*hard=*/true);

        if (hCtrlNotify) xTaskNotifyGive(hCtrlNotify);

        vTaskDelayUntil(&next, T);
    }
}

/* ====== SPD_CTRL ====== */
static void task_spd_ctrl(void *arg)
{
    float kp = 0.4f, ki = 0.1f, integ = 0.f;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        stats_on_release(&st_ctrl, g_last_enc_release_us);
        stats_on_start(&st_ctrl, esp_timer_get_time());

        float err = g_belt.set_rpm - g_belt.rpm;
        integ += err * (ENC_T_MS / 1000.0f);
        if (integ > 10000.0f) integ = 10000.0f;
        if (integ < -10000.0f) integ = -10000.0f;

        float u = kp * err + ki * integ;
        g_belt.set_rpm += 0.1f * u;

        if (g_belt.set_rpm < 0.0f)     g_belt.set_rpm = 0.0f;
        if (g_belt.set_rpm > 5000.0f)  g_belt.set_rpm = 5000.0f;

        cpu_tight_loop_us(1200);

        stats_on_finish(&st_ctrl, esp_timer_get_time(), D_CTRL_US, /*hard=*/true);

        if (xSemaphoreTake(semHMI, 0) == pdTRUE) {
            printf("HMI: rpm=%.1f set=%.1f pos=%.1fmm\n", g_belt.rpm, g_belt.set_rpm, g_belt.pos_mm);
            cpu_tight_loop_us(400);
        }
    }
}

/* ====== SORT_ACT ====== */
static void task_sort_act(void *arg)
{
    sort_evt_t ev;
    for (;;) {
        if (xQueueReceive(qSort, &ev, portMAX_DELAY) == pdTRUE) {
            stats_on_release(&st_sort, ev.t_evt_us);
            stats_on_start(&st_sort, esp_timer_get_time());

            cpu_tight_loop_us(700);

            stats_on_finish(&st_sort, esp_timer_get_time(), D_SORT_US, /*hard=*/true);
        }
    }
}

/* ====== SAFETY_TASK ====== */
static void task_safety(void *arg)
{
    for (;;) {
        if (xSemaphoreTake(semEStop, portMAX_DELAY) == pdTRUE) {
            stats_on_release(&st_safe, g_last_touchD_us);
            stats_on_start(&st_safe, esp_timer_get_time());

            g_belt.set_rpm = 0.f;
            cpu_tight_loop_us(900);

            stats_on_finish(&st_safe, esp_timer_get_time(), D_SAFE_US, /*hard=*/true);
            ESP_LOGW(TAG, "E-STOP executado");
        }
    }
}

/* ====== TOUCH polling ====== */
static void task_touch_poll(void *arg)
{
    ESP_ERROR_CHECK(touch_pad_init());
    ESP_ERROR_CHECK(touch_pad_set_fsm_mode(TOUCH_FSM_MODE_TIMER));
    ESP_ERROR_CHECK(touch_pad_set_measurement_interval(20)); // ~2.5ms
    ESP_ERROR_CHECK(touch_pad_set_voltage(TOUCH_HVOLT_2V7, TOUCH_LVOLT_0V5, TOUCH_HVOLT_ATTEN_1V));
    ESP_ERROR_CHECK(touch_pad_filter_start(10));

    ESP_ERROR_CHECK(touch_pad_config(TP_OBJ,   0));
    ESP_ERROR_CHECK(touch_pad_config(TP_HMI,   0));
    ESP_ERROR_CHECK(touch_pad_config(TP_ESTOP, 0));

    vTaskDelay(ticks_from_ms(80));

    uint16_t base_obj=0, base_hmi=0, base_stop=0;
    ESP_ERROR_CHECK(touch_pad_read_raw_data(TP_OBJ,   &base_obj));
    ESP_ERROR_CHECK(touch_pad_read_raw_data(TP_HMI,   &base_hmi));
    ESP_ERROR_CHECK(touch_pad_read_raw_data(TP_ESTOP, &base_stop));
    if (base_obj  < 50) base_obj  = 2000;
    if (base_hmi  < 50) base_hmi  = 2000;
    if (base_stop < 50) base_stop = 2000;

    ESP_LOGI(TAG, "RAW baseline: OBJ=%u (T7/GPIO27)  HMI=%u (T8/GPIO33)  ESTOP=%u (T9/GPIO32)",
             base_obj, base_hmi, base_stop);

    uint16_t th_obj  = (uint16_t)(base_obj  * 0.70f);
    uint16_t th_hmi  = (uint16_t)(base_hmi  * 0.70f);
    uint16_t th_stop = (uint16_t)(base_stop * 0.70f);

    bool prev_obj=false, prev_hmi=false, prev_stop=false;
    TickType_t debounce = ticks_from_ms(30);

    while (1) {
        uint16_t raw;

        // OBJ (Touch B)
        touch_pad_read_raw_data(TP_OBJ, &raw);
        bool obj = (raw < th_obj);
        if (obj && !prev_obj) {
            g_last_touchB_us = esp_timer_get_time();
            sort_evt_t e = { .t_evt_us = g_last_touchB_us };
            (void)xQueueSend(qSort, &e, 0);
            ESP_LOGI(TAG, "OBJ: raw=%u (thr=%u)", raw, th_obj);
        }
        prev_obj = obj;

        // HMI (Touch C)
        touch_pad_read_raw_data(TP_HMI, &raw);
        bool hmi = (raw < th_hmi);
        if (hmi && !prev_hmi) { (void)xSemaphoreGive(semHMI); ESP_LOGI(TAG, "HMI: raw=%u (thr=%u)", raw, th_hmi); }
        prev_hmi = hmi;

        // ESTOP (Touch D)
        touch_pad_read_raw_data(TP_ESTOP, &raw);
        bool stop = (raw < th_stop);
        if (stop && !prev_stop) {
            g_last_touchD_us = esp_timer_get_time();
            (void)xSemaphoreGive(semEStop);
            ESP_LOGW(TAG, "E-STOP: raw=%u (thr=%u)", raw, th_stop);
        }
        prev_stop = stop;

        vTaskDelay(debounce);
    }
}

/* ====== UART CMD ====== */
static void task_uart_cmd(void *arg)
{
    const uart_port_t U = UART_NUM_0;
    uart_driver_install(U, 256, 0, 0, NULL, 0);
    ESP_LOGI(TAG, "UART: b=OBJ  c=HMI  d=E-STOP  r=RAWs");

    uint8_t ch;
    while (1) {
        if (uart_read_bytes(U, &ch, 1, pdMS_TO_TICKS(10)) == 1) {
            switch (ch) {
                case 'b': case 'B': {
                    g_last_touchB_us = esp_timer_get_time();
                    sort_evt_t e = { g_last_touchB_us };
                    (void)xQueueSend(qSort,&e,0);
                    ESP_LOGI(TAG,"[UART] OBJ");
                } break;
                case 'c': case 'C': { (void)xSemaphoreGive(semHMI); ESP_LOGI(TAG,"[UART] HMI"); } break;
                case 'd': case 'D': {
                    g_last_touchD_us = esp_timer_get_time();
                    (void)xSemaphoreGive(semEStop);
                    ESP_LOGW(TAG,"[UART] E-STOP");
                } break;
                case 'r': case 'R': {
                    uint16_t ro=0, rc=0, rd=0;
                    touch_pad_read_raw_data(TP_OBJ,&ro);
                    touch_pad_read_raw_data(TP_HMI,&rc);
                    touch_pad_read_raw_data(TP_ESTOP,&rd);
                    ESP_LOGI(TAG, "RAWs -> OBJ=%u  HMI=%u  ESTOP=%u", ro, rc, rd);
                } break;
                default: break;
            }
        }
        vTaskDelay(ticks_from_ms(5));
    }
}

/* ====== STATS: log 1x/s ====== */
static void task_stats(void *arg)
{
    TickType_t last = xTaskGetTickCount();
    TickType_t period = ticks_from_ms(1000);

    // Para %CPU simples com Idle Hook:
    int64_t last_sample = 0, last_idle = 0;

    for (;;) {
        vTaskDelayUntil(&last, period);

        char tbuf[32];

        log_now_str_sntp(tbuf, sizeof(tbuf));
        ESP_LOGI(TAG, "[%s ] =================================================================================", tbuf);

        log_now_str_sntp(tbuf, sizeof(tbuf));
        ESP_LOGI(TAG, "[%s ] STATS: rpm=%.1f set=%.1f pos=%.1fmm",
                 tbuf, g_belt.rpm, g_belt.set_rpm, g_belt.pos_mm);

        log_now_str_sntp(tbuf, sizeof(tbuf));
        ESP_LOGI(TAG, "[%s ] ENC: rel=%u fin=%u hard=%u Cmax=%lldus Lmax=%lldus Rmax=%lldus",
                 tbuf,
                 st_enc.releases, st_enc.finishes, st_enc.hard_miss,
                 (long long)st_enc.worst_exec_us,
                 (long long)st_enc.worst_latency_us,
                 (long long)st_enc.worst_response_us);

        log_now_str_sntp(tbuf, sizeof(tbuf));
        ESP_LOGI(TAG, "[%s ] CTRL: rel=%u fin=%u hard=%u Cmax=%lldus Lmax=%lldus Rmax=%lldus",
                 tbuf,
                 st_ctrl.releases, st_ctrl.finishes, st_ctrl.hard_miss,
                 (long long)st_ctrl.worst_exec_us,
                 (long long)st_ctrl.worst_latency_us,
                 (long long)st_ctrl.worst_response_us);

        log_now_str_sntp(tbuf, sizeof(tbuf));
        ESP_LOGI(TAG, "[%s ] SORT: rel=%u fin=%u hard=%u Cmax=%lldus Lmax=%lldus Rmax=%lldus",
                 tbuf,
                 st_sort.releases, st_sort.finishes, st_sort.hard_miss,
                 (long long)st_sort.worst_exec_us,
                 (long long)st_sort.worst_latency_us,
                 (long long)st_sort.worst_response_us);

        log_now_str_sntp(tbuf, sizeof(tbuf));
        ESP_LOGI(TAG, "[%s ] SAFE: rel=%u fin=%u hard=%u Cmax=%lldus Lmax=%lldus Rmax=%lldus",
                 tbuf,
                 st_safe.releases, st_safe.finishes, st_safe.hard_miss,
                 (long long)st_safe.worst_exec_us,
                 (long long)st_safe.worst_latency_us,
                 (long long)st_safe.worst_response_us);

        // %CPU simples via Idle Hook (se habilitado)
        #if configUSE_IDLE_HOOK
        {
            int64_t now = esp_timer_get_time();
            if (!last_sample) { last_sample = now; last_idle = idle_us_acc; }
            else {
                int64_t dt = now - last_sample;
                int64_t didle = idle_us_acc - last_idle;
                float cpu = 100.0f * (1.0f - (float)didle/(float)dt);
                log_now_str_sntp(tbuf, sizeof(tbuf));
                ESP_LOGI(TAG, "[%s ] CPU Util (IdleHook): %.1f%% (janela %.0f ms)",
                         tbuf, cpu, dt/1000.0f);
                last_sample = now; last_idle = idle_us_acc;
            }
        }
        #endif

        #if (configUSE_TRACE_FACILITY==1) && (configGENERATE_RUN_TIME_STATS==1)
            log_now_str_sntp(tbuf, sizeof(tbuf));
            ESP_LOGI(TAG, "[%s ] Runtime stats:", tbuf);
            print_runtime_stats();
        #endif
    }
}

/* ====== app_main ====== */
void app_main(void)
{   
    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();

    // Fuso (Brasil sem DST): UTC-3
    setenv("TZ", "BRT3", 1);
    tzset();

    // SNTP
    sntp_start_and_wait();

    // Hora atual
    char tbuf[32];
    log_now_str_sntp(tbuf, sizeof(tbuf));
    ESP_LOGI(TAG, "[%s ] Hora atual (SNTP): sistema sincronizado %s", tbuf, g_time_synced ? "SIM" : "NÃO");

    // IPC
    qSort    = xQueueCreate(8, sizeof(sort_evt_t));
    semEStop = xSemaphoreCreateBinary();
    semHMI   = xSemaphoreCreateBinary();

    // Tarefas principais (core 0)
    xTaskCreatePinnedToCore(task_safety,    "SAFETY",    STK_MAIN, NULL, PRIO_ESTOP, &hSAFE, 0);
    xTaskCreatePinnedToCore(task_enc_sense, "ENC_SENSE", STK_MAIN, NULL, PRIO_ENC,   &hENC,  0);
    xTaskCreatePinnedToCore(task_spd_ctrl,  "SPD_CTRL",  STK_MAIN, NULL, PRIO_CTRL,  &hCTRL, 0);
    xTaskCreatePinnedToCore(task_sort_act,  "SORT_ACT",  STK_MAIN, NULL, PRIO_SORT,  &hSORT, 0);

    // Encadeamento ENC -> CTRL
    hCtrlNotify = hCTRL;

    // Tarefas auxiliares
    xTaskCreatePinnedToCore(task_touch_poll, "TOUCH",    STK_AUX, NULL, PRIO_TOUCH, NULL, 0);
    xTaskCreatePinnedToCore(task_uart_cmd,   "UART_CMD", STK_AUX, NULL, PRIO_TOUCH, NULL, 0);
    xTaskCreatePinnedToCore(task_stats,      "STATS",    STK_AUX, NULL, PRIO_STATS, NULL, 0);
    xTaskCreatePinnedToCore(task_monitor,    "MONITOR",  4096,    NULL, PRIO_STATS, NULL, 0);

    log_now_str_sntp(tbuf, sizeof(tbuf));
    ESP_LOGI(TAG, "[%s ] Sistema iniciado", tbuf);
}
