/*
 * picokbd -- Raspberry Pi Pico W を USB キーボードにして、Wi-Fi (UDP) で受けたコマンドどおりにキーを押す
 *
 * X68000 Z の USB に挿し、Raspberry Pi などから操作する (ESC で rcmd を止める、Ctrl+F11+Del でリセットなど)。
 * UDP ポート 6869 に 1 行のテキストを送ると、終わったら "ok" (解釈できなければ "err ...") を返す。
 *   key esc enter ...        1 つずつ押して離す
 *   combo ctrl f11 del       同時に押して (既定 100ms) 離す
 *   hold <ms>                次の combo の押している時間
 *   type <文字列>             ASCII を打つ (US 配列の文字として送る)
 * Wi-Fi の SSID / パスワードは wifi.h (git に入れない) の WIFI_SSID / WIFI_PASSWORD。
 */
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "lwip/udp.h"
#include "bsp/board_api.h"
#include "tusb.h"
#include "usb_descriptors.h"
#include "wifi.h"

#define PORT    6869
#define TAP_MS  40                      /* 押してから離すまで / 次のキーまで */

/* 受けたコマンド 1 件 (UDP の受信コールバックで積み、メインループで実行する) */
static char cmdbuf[512];
static volatile bool cmd_ready;
static ip_addr_t cmd_from;
static u16_t cmd_port;
static struct udp_pcb *pcb;

static void reply(const char *s)
{
    struct pbuf *p = pbuf_alloc(PBUF_TRANSPORT, strlen(s), PBUF_RAM);
    if (p == NULL)
        return;
    memcpy(p->payload, s, strlen(s));
    cyw43_arch_lwip_begin();
    udp_sendto(pcb, p, &cmd_from, cmd_port);
    cyw43_arch_lwip_end();
    pbuf_free(p);
}

static void on_udp(void *arg, struct udp_pcb *upcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    if (!cmd_ready) {
        int n = pbuf_copy_partial(p, cmdbuf, sizeof cmdbuf - 1, 0);
        while (n > 0 && (cmdbuf[n - 1] == '\n' || cmdbuf[n - 1] == '\r'))
            n--;
        cmdbuf[n] = '\0';
        cmd_from = *addr;
        cmd_port = port;
        cmd_ready = true;
    }
    pbuf_free(p);
}

/* USB を動かしたまま待つ */
static void wait_ms(int ms)
{
    absolute_time_t end = make_timeout_time_ms(ms);
    while (absolute_time_diff_us(get_absolute_time(), end) > 0)
        tud_task();
}

static void send_report(uint8_t mod, const uint8_t keys[6])
{
    for (int i = 0; i < 100 && !tud_hid_n_ready(ITF_NUM_KEYBOARD); i++)
        wait_ms(1);
    tud_hid_n_keyboard_report(ITF_NUM_KEYBOARD, 0, mod, keys);
    wait_ms(TAP_MS);
}

static const struct { const char *name; uint8_t code; uint8_t mod; } names[] = {
    { "esc", HID_KEY_ESCAPE }, { "enter", HID_KEY_ENTER }, { "ret", HID_KEY_ENTER },
    { "tab", HID_KEY_TAB }, { "bs", HID_KEY_BACKSPACE }, { "space", HID_KEY_SPACE },
    { "del", HID_KEY_DELETE }, { "ins", HID_KEY_INSERT }, { "home", HID_KEY_HOME }, { "end", HID_KEY_END },
    { "pgup", HID_KEY_PAGE_UP }, { "pgdn", HID_KEY_PAGE_DOWN },
    { "up", HID_KEY_ARROW_UP }, { "down", HID_KEY_ARROW_DOWN }, { "left", HID_KEY_ARROW_LEFT }, { "right", HID_KEY_ARROW_RIGHT },
    { "f1", HID_KEY_F1 }, { "f2", HID_KEY_F2 }, { "f3", HID_KEY_F3 }, { "f4", HID_KEY_F4 },
    { "f5", HID_KEY_F5 }, { "f6", HID_KEY_F6 }, { "f7", HID_KEY_F7 }, { "f8", HID_KEY_F8 },
    { "f9", HID_KEY_F9 }, { "f10", HID_KEY_F10 }, { "f11", HID_KEY_F11 }, { "f12", HID_KEY_F12 },
    { "ctrl", 0, KEYBOARD_MODIFIER_LEFTCTRL }, { "shift", 0, KEYBOARD_MODIFIER_LEFTSHIFT },
    { "alt", 0, KEYBOARD_MODIFIER_LEFTALT }, { "gui", 0, KEYBOARD_MODIFIER_LEFTGUI },
};

/* 1 語をキーにする。英数字 1 文字も受け付ける。戻り値: 見つかれば true */
static bool lookup(const char *w, uint8_t *code, uint8_t *mod)
{
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (strcmp(w, names[i].name) == 0) {
            *code = names[i].code;
            *mod = names[i].mod;
            return true;
        }
    if (strlen(w) == 1) {
        static const uint8_t conv[128][2] = { HID_ASCII_TO_KEYCODE };
        unsigned char c = w[0];
        if (c < 128 && conv[c][1]) {
            *code = conv[c][1];
            *mod = conv[c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0;
            return true;
        }
    }
    return false;
}

static void run(char *line)
{
    static int hold = 100;
    static const uint8_t none[6] = { 0 };
    char *save, *verb = strtok_r(line, " ", &save);
    if (verb == NULL) {
        reply("err empty");
        return;
    }
    if (strcmp(verb, "type") == 0) {
        static const uint8_t conv[128][2] = { HID_ASCII_TO_KEYCODE };
        char *s = save;
        for (; s && *s; s++) {
            unsigned char c = *s;
            if (c >= 128 || conv[c][1] == 0)
                continue;
            uint8_t keys[6] = { conv[c][1] };
            send_report(conv[c][0] ? KEYBOARD_MODIFIER_LEFTSHIFT : 0, keys);
            send_report(0, none);
        }
        reply("ok");
        return;
    }
    if (strcmp(verb, "hold") == 0) {
        char *w = strtok_r(NULL, " ", &save);
        hold = w ? atoi(w) : 100;
        reply("ok");
        return;
    }
    bool combo = strcmp(verb, "combo") == 0;
    if (!combo && strcmp(verb, "key") != 0) {
        reply("err verb");
        return;
    }
    uint8_t mod = 0, keys[6] = { 0 };
    int nk = 0;
    for (char *w; (w = strtok_r(NULL, " ", &save)) != NULL; ) {
        uint8_t c, m;
        for (char *q = w; *q; q++)
            *q = tolower((unsigned char)*q);
        if (!lookup(w, &c, &m)) {
            reply("err key");
            send_report(0, none);
            return;
        }
        if (combo) {
            mod |= m;
            if (c && nk < 6)
                keys[nk++] = c;
            send_report(mod, keys);     /* 1 つずつ押し足す (Ctrl を先に押した状態にする) */
        } else {
            uint8_t k1[6] = { c };
            send_report(m, k1);
            send_report(0, none);
        }
    }
    if (combo) {
        wait_ms(hold);
        send_report(0, none);
    }
    reply("ok");
}

int main(void)
{
    board_init();
    tud_init(BOARD_TUD_RHPORT);
    if (cyw43_arch_init())
        return 1;
    cyw43_arch_enable_sta_mode();
    cyw43_wifi_pm(&cyw43_state, cyw43_pm_value(CYW43_NO_POWERSAVE_MODE, 20, 1, 1, 1));
    while (cyw43_arch_wifi_connect_timeout_ms(WIFI_SSID, WIFI_PASSWORD, CYW43_AUTH_WPA2_AES_PSK, 15000))
        wait_ms(1000);
    cyw43_arch_gpio_put(CYW43_WL_GPIO_LED_PIN, 1);
    cyw43_arch_lwip_begin();
    pcb = udp_new();
    udp_bind(pcb, IP_ANY_TYPE, PORT);
    udp_recv(pcb, on_udp, NULL);
    cyw43_arch_lwip_end();
    for (;;) {
        tud_task();
        if (cmd_ready) {
            run(cmdbuf);
            cmd_ready = false;
        }
    }
}

/* HID の必須コールバック (使わない) */
uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t *buffer, uint16_t reqlen)
{
    return 0;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id, hid_report_type_t report_type, uint8_t const *buffer, uint16_t bufsize)
{
}
