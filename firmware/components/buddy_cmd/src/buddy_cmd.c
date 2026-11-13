/* SPDX-License-Identifier: Apache-2.0 */
/*
 * @file buddy_cmd.c
 * @brief Buddy text command protocol.
 */

#include "buddy_cmd.h"

#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "drv_led.h"
#include "drv_solenoid.h"
#include "esp_app_desc.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CMD_LINE_SIZE                 256U
#define CMD_MAX_ARGS                  8U
#define CMD_REPLY_SIZE                768U

#define CMD_JUMP_DEFAULT_COUNT        1U
#define CMD_JUMP_DEFAULT_DURATION_MS  80U
#define CMD_JUMP_DEFAULT_INTERVAL_MS  150U
#define CMD_LED_DEFAULT_BLINK_MS      500U
#define CMD_LED_DEFAULT_BREATHE_MS    2000U

/*
 * Solenoid thermal budget: a token bucket of 12 s on-time, refilled at 20% duty
 * (200 ms per second). 20% matches the validated endurance duty cycle (200 ms on per
 * second for 4 hours), so the long-term average on-time stays within tested limits.
 */
#define CMD_JUMP_BUDGET_CAPACITY_MS   12000LL
#define CMD_JUMP_BUDGET_REFILL_PCT    20LL

#define CMD_REBOOT_DELAY_MS           100U

struct cmd_color
{
    const char *name;
    uint8_t red;
    uint8_t green;
    uint8_t blue;
};

static const struct cmd_color s_colors[] =
{
    {"red", 255U, 0U, 0U},
    {"green", 0U, 255U, 0U},
    {"blue", 0U, 0U, 255U},
    {"white", 255U, 255U, 255U},
    {"yellow", 255U, 180U, 0U},
    {"orange", 255U, 80U, 0U},
    {"cyan", 0U, 255U, 255U},
    {"purple", 160U, 0U, 255U},
    {"pink", 255U, 60U, 120U},
};

static const char *const s_usage[] =
{
    "ping",
    "info",
    "help",
    "led",
    "led off",
    "led <color>|<r> <g> <b>",
    "led blink <color>|<r> <g> <b> [interval_ms 1-60000]",
    "led breathe <color>|<r> <g> <b> [period_ms 100-60000]",
    "led rainbow",
    "jump [count 1-255] [duration_ms 1-5000] [interval_ms 0-5000]",
    "jump stop",
    "reboot",
};

static buddy_cmd_write_fn_t s_write_fn;
static void *s_write_context;
static char s_line[CMD_LINE_SIZE];
static size_t s_line_length;
static bool s_line_too_long;
static int64_t s_jump_budget_ms = CMD_JUMP_BUDGET_CAPACITY_MS;
static int64_t s_jump_budget_updated_us;

/**
 * @brief Format one line, append "\r\n" and send it through the output callback.
 * @param  format: printf format string.
 */
static void send_line(const char *format, ...)
{
    char buffer[CMD_REPLY_SIZE];
    va_list args;
    va_start(args, format);
    int length = vsnprintf(buffer, sizeof(buffer) - 2U, format, args);
    va_end(args);
    if (length < 0)
    {
        return;
    }
    if ((size_t)length > sizeof(buffer) - 3U)
    {
        length = (int)(sizeof(buffer) - 3U);
    }
    buffer[length++] = '\r';
    buffer[length++] = '\n';
    (void)s_write_fn((const uint8_t *)buffer, (size_t)length, s_write_context);
}

/**
 * @brief Reply with an error.
 * @param  code: machine-readable error code (lower_snake_case).
 * @param  message: human-readable explanation.
 */
static void reply_error(const char *code, const char *message)
{
    send_line("ERR %s %s", code, message);
}

/**
 * @brief Strictly parse a decimal unsigned integer and check its range.
 * @param  text: input string.
 * @param  min: minimum value (inclusive).
 * @param  max: maximum value (inclusive).
 * @param  value: parsed value (output).
 * @return true if parsed and within range.
 */
static bool parse_u32(const char *text, uint32_t min, uint32_t max,
                      uint32_t *value)
{
    if (text == NULL || !isdigit((unsigned char)text[0]))
    {
        return false;
    }
    errno = 0;
    char *end = NULL;
    const unsigned long parsed = strtoul(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0' ||
        parsed < min || parsed > max)
    {
        return false;
    }
    *value = (uint32_t)parsed;
    return true;
}

/**
 * @brief Parse a color: one color name, or three integers 0-255.
 * @param  argc: number of remaining arguments.
 * @param  argv: remaining arguments.
 * @param  red/green/blue: parsed color (output).
 * @return Number of arguments consumed; 0 on failure.
 */
static int parse_color(int argc, char **argv,
                       uint8_t *red, uint8_t *green, uint8_t *blue)
{
    if (argc <= 0)
    {
        return 0;
    }
    for (size_t i = 0U; i < sizeof(s_colors) / sizeof(s_colors[0]); ++i)
    {
        if (strcmp(argv[0], s_colors[i].name) == 0)
        {
            *red = s_colors[i].red;
            *green = s_colors[i].green;
            *blue = s_colors[i].blue;
            return 1;
        }
    }

    uint32_t r = 0U;
    uint32_t g = 0U;
    uint32_t b = 0U;
    if (argc >= 3 &&
        parse_u32(argv[0], 0U, 255U, &r) &&
        parse_u32(argv[1], 0U, 255U, &g) &&
        parse_u32(argv[2], 0U, 255U, &b))
    {
        *red = (uint8_t)r;
        *green = (uint8_t)g;
        *blue = (uint8_t)b;
        return 3;
    }
    return 0;
}

/**
 * @brief Protocol name of an LED mode.
 * @param  mode: LED mode.
 * @return Mode name.
 */
static const char *led_mode_name(drv_led_mode_t mode)
{
    switch (mode)
    {
    case DRV_LED_MODE_CONSTANT:
        return "constant";
    case DRV_LED_MODE_BLINK:
        return "blink";
    case DRV_LED_MODE_BREATHE:
        return "breathe";
    case DRV_LED_MODE_EFFECT:
        return "rainbow";
    default:
        return "unknown";
    }
}

/**
 * @brief Reply with the current LED configuration.
 */
static void reply_led_state(void)
{
    drv_led_config_t config;
    if (drv_led_get_config(&config) != ESP_OK)
    {
        reply_error("hw_error", "cannot read led state");
        return;
    }
    send_line("OK {\"mode\":\"%s\",\"r\":%u,\"g\":%u,\"b\":%u,\"ms\":%u}",
              led_mode_name(config.mode),
              (unsigned)config.red,
              (unsigned)config.green,
              (unsigned)config.blue,
              (unsigned)config.timing_ms);
}

/**
 * @brief Handle the led command.
 * @param  argc: number of arguments, including the command name.
 * @param  argv: arguments.
 */
static void cmd_led(int argc, char **argv)
{
    if (argc == 1)
    {
        reply_led_state();
        return;
    }

    drv_led_config_t config =
    {
        .mode = DRV_LED_MODE_CONSTANT,
    };

    if (strcmp(argv[1], "off") == 0 && argc == 2)
    {
        /* Constant all-zero means off. */
    }
    else if (strcmp(argv[1], "rainbow") == 0 && argc == 2)
    {
        config.mode = DRV_LED_MODE_EFFECT;
    }
    else
    {
        int index = 1;
        uint32_t min_ms = 0U;
        uint32_t max_ms = 0U;
        if (strcmp(argv[1], "blink") == 0)
        {
            config.mode = DRV_LED_MODE_BLINK;
            config.timing_ms = CMD_LED_DEFAULT_BLINK_MS;
            min_ms = DRV_LED_BLINK_MIN_INTERVAL_MS;
            max_ms = DRV_LED_BLINK_MAX_INTERVAL_MS;
            index = 2;
        }
        else if (strcmp(argv[1], "breathe") == 0)
        {
            config.mode = DRV_LED_MODE_BREATHE;
            config.timing_ms = CMD_LED_DEFAULT_BREATHE_MS;
            min_ms = DRV_LED_BREATHE_MIN_PERIOD_MS;
            max_ms = DRV_LED_BREATHE_MAX_PERIOD_MS;
            index = 2;
        }

        const int used = parse_color(argc - index, &argv[index],
                                     &config.red, &config.green,
                                     &config.blue);
        if (used == 0)
        {
            reply_error("bad_args",
                        "color must be a name (red green blue white yellow "
                        "orange cyan purple pink) or three numbers 0-255");
            return;
        }
        index += used;

        if (index < argc)
        {
            uint32_t timing = 0U;
            if (config.mode == DRV_LED_MODE_CONSTANT || index + 1 != argc)
            {
                reply_error("bad_args", "too many arguments, see help");
                return;
            }
            if (!parse_u32(argv[index], min_ms, max_ms, &timing))
            {
                reply_error("out_of_range",
                            config.mode == DRV_LED_MODE_BLINK
                                ? "blink interval_ms must be 1-60000"
                                : "breathe period_ms must be 100-60000");
                return;
            }
            config.timing_ms = (uint16_t)timing;
        }
    }

    if (drv_led_set_config(&config) != ESP_OK)
    {
        reply_error("hw_error", "led driver rejected the request");
        return;
    }
    reply_led_state();
}

/**
 * @brief Refill the solenoid on-time budget by the elapsed time.
 * @return Available budget in ms.
 */
static int64_t refill_jump_budget(void)
{
    const int64_t now_us = esp_timer_get_time();
    const int64_t elapsed_ms = (now_us - s_jump_budget_updated_us) / 1000LL;
    if (elapsed_ms > 0)
    {
        s_jump_budget_ms += elapsed_ms * CMD_JUMP_BUDGET_REFILL_PCT / 100LL;
        if (s_jump_budget_ms > CMD_JUMP_BUDGET_CAPACITY_MS)
        {
            s_jump_budget_ms = CMD_JUMP_BUDGET_CAPACITY_MS;
        }
        s_jump_budget_updated_us = now_us;
    }
    return s_jump_budget_ms;
}

/**
 * @brief Handle the jump command (solenoid jumps).
 * @param  argc: number of arguments, including the command name.
 * @param  argv: arguments.
 */
static void cmd_jump(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "stop") == 0)
    {
        if (drv_solenoid_stop() != ESP_OK)
        {
            reply_error("hw_error", "cannot stop solenoid");
            return;
        }
        send_line("OK");
        return;
    }
    if (argc > 4)
    {
        reply_error("bad_args",
                    "usage: jump [count] [duration_ms] [interval_ms]");
        return;
    }

    uint32_t count = CMD_JUMP_DEFAULT_COUNT;
    uint32_t duration_ms = CMD_JUMP_DEFAULT_DURATION_MS;
    uint32_t interval_ms = CMD_JUMP_DEFAULT_INTERVAL_MS;
    if ((argc > 1 && !parse_u32(argv[1], DRV_SOLENOID_MIN_COUNT,
                                DRV_SOLENOID_MAX_COUNT, &count)) ||
        (argc > 2 && !parse_u32(argv[2], DRV_SOLENOID_MIN_DURATION_MS,
                                DRV_SOLENOID_MAX_DURATION_MS,
                                &duration_ms)) ||
        (argc > 3 && !parse_u32(argv[3], 0U, DRV_SOLENOID_MAX_INTERVAL_MS,
                                &interval_ms)))
    {
        reply_error("out_of_range",
                    "count 1-255, duration_ms 1-5000, interval_ms 0-5000");
        return;
    }

    const int64_t cost_ms = (int64_t)count * (int64_t)duration_ms;
    if (cost_ms > CMD_JUMP_BUDGET_CAPACITY_MS)
    {
        reply_error("out_of_range",
                    "count*duration_ms must not exceed 12000 per request");
        return;
    }
    const int64_t budget_ms = refill_jump_budget();
    if (cost_ms > budget_ms)
    {
        char message[96];
        const int64_t wait_s =
            ((cost_ms - budget_ms) * 100LL / CMD_JUMP_BUDGET_REFILL_PCT +
             999LL) / 1000LL;
        (void)snprintf(message, sizeof(message),
                       "solenoid cooling down, retry in %lld s",
                       (long long)wait_s);
        reply_error("busy", message);
        return;
    }

    if (drv_solenoid_start_sequence((uint16_t)duration_ms,
                                    (uint8_t)count,
                                    (uint16_t)interval_ms) != ESP_OK)
    {
        reply_error("hw_error", "solenoid driver rejected the request");
        return;
    }
    s_jump_budget_ms -= cost_ms;
    send_line("OK {\"count\":%u,\"duration_ms\":%u,\"interval_ms\":%u}",
              (unsigned)count, (unsigned)duration_ms, (unsigned)interval_ms);
}

/**
 * @brief Copy a string, keeping only characters that are safe inside a JSON string.
 * @param  dst: output buffer.
 * @param  size: output buffer size.
 * @param  src: input string.
 */
static void copy_json_safe(char *dst, size_t size, const char *src)
{
    size_t out = 0U;
    for (size_t i = 0U; src[i] != '\0' && out + 1U < size; ++i)
    {
        const unsigned char ch = (unsigned char)src[i];
        if (ch >= 0x20U && ch < 0x7FU && ch != '"' && ch != '\\')
        {
            dst[out++] = (char)ch;
        }
    }
    dst[out] = '\0';
}

/**
 * @brief Handle the info command: only the open firmware version and hardware revision.
 */
static void cmd_info(void)
{
    const esp_app_desc_t *app = esp_app_get_description();

    char version[sizeof(app->version)];
    copy_json_safe(version, sizeof(version), app->version);

    send_line("OK {\"version\":\"%s%s\",\"hw\":\"%s\"}",
              BUDDY_CMD_VERSION_PREFIX,
              version,
              board_revision_name(board_get_info()->revision));
}

/**
 * @brief Handle the help command: list all command usages as a JSON array.
 */
static void cmd_help(void)
{
    char buffer[CMD_REPLY_SIZE - 8U];
    size_t used = 0U;
    used += (size_t)snprintf(buffer, sizeof(buffer), "{\"commands\":[");
    for (size_t i = 0U; i < sizeof(s_usage) / sizeof(s_usage[0]); ++i)
    {
        if (used >= sizeof(buffer))
        {
            break;
        }
        used += (size_t)snprintf(buffer + used, sizeof(buffer) - used,
                                 "%s\"%s\"", i == 0U ? "" : ",",
                                 s_usage[i]);
    }
    if (used < sizeof(buffer))
    {
        (void)snprintf(buffer + used, sizeof(buffer) - used, "]}");
    }
    send_line("OK %s", buffer);
}

/**
 * @brief Parse and execute one command line.
 * @param  line: NUL-terminated command line; modified in place.
 */
static void execute_line(char *line)
{
    char *argv[CMD_MAX_ARGS];
    int argc = 0;

    for (char *p = line; *p != '\0'; ++p)
    {
        *p = (char)tolower((unsigned char)*p);
    }

    char *save = NULL;
    for (char *token = strtok_r(line, " \t", &save);
         token != NULL;
         token = strtok_r(NULL, " \t", &save))
    {
        if (argc == (int)CMD_MAX_ARGS)
        {
            reply_error("bad_args", "too many arguments");
            return;
        }
        argv[argc++] = token;
    }
    if (argc == 0 || argv[0][0] == '#')
    {
        return;
    }

    const char *command = argv[0];
    if (strcmp(command, "ping") == 0)
    {
        send_line("OK {\"pong\":true}");
    }
    else if (strcmp(command, "info") == 0)
    {
        cmd_info();
    }
    else if (strcmp(command, "help") == 0)
    {
        cmd_help();
    }
    else if (strcmp(command, "led") == 0)
    {
        cmd_led(argc, argv);
    }
    else if (strcmp(command, "jump") == 0)
    {
        cmd_jump(argc, argv);
    }
    else if (strcmp(command, "reboot") == 0)
    {
        send_line("OK");
        vTaskDelay(pdMS_TO_TICKS(CMD_REBOOT_DELAY_MS));
        esp_restart();
    }
    else
    {
        reply_error("unknown_command", "send 'help' to list commands");
    }
}

esp_err_t buddy_cmd_init(buddy_cmd_write_fn_t write_fn, void *context)
{
    if (write_fn == NULL)
    {
        return ESP_ERR_INVALID_ARG;
    }
    s_write_fn = write_fn;
    s_write_context = context;
    s_line_length = 0U;
    s_line_too_long = false;
    s_jump_budget_ms = CMD_JUMP_BUDGET_CAPACITY_MS;
    s_jump_budget_updated_us = esp_timer_get_time();
    send_line("EVT {\"event\":\"boot\"}");
    return ESP_OK;
}

void buddy_cmd_feed(const uint8_t *data, size_t length)
{
    if (s_write_fn == NULL || (data == NULL && length > 0U))
    {
        return;
    }

    for (size_t i = 0U; i < length; ++i)
    {
        const uint8_t ch = data[i];
        if (ch == '\r' || ch == '\n')
        {
            if (s_line_too_long)
            {
                reply_error("line_too_long", "max 255 bytes per line");
            }
            else if (s_line_length > 0U)
            {
                s_line[s_line_length] = '\0';
                execute_line(s_line);
            }
            s_line_length = 0U;
            s_line_too_long = false;
        }
        else if (!s_line_too_long)
        {
            if (s_line_length < sizeof(s_line) - 1U)
            {
                s_line[s_line_length++] = (char)ch;
            }
            else
            {
                s_line_too_long = true;
            }
        }
    }
}
