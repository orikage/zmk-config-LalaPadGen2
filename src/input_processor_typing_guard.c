/*
 * Input processor that drops matching input events (e.g. trackpad tap clicks)
 * for a short time after the last key press, to avoid accidental clicks while
 * typing.
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_input_processor_typing_guard

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/dt-bindings/input/input-event-codes.h>
#include <drivers/input_processor.h>

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <zmk/event_manager.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keys.h>

#define TYPING_GUARD_MAX_LISTENERS 8

struct typing_guard_config {
    uint32_t require_prior_idle_ms;
    bool ignore_modifiers;
    uint16_t type;
    size_t codes_len;
    const uint16_t *codes;
};

struct typing_guard_data {
    /* Per listener: bit i set = press of codes[i] was dropped, so drop its release too. */
    uint32_t dropped[TYPING_GUARD_MAX_LISTENERS];
};

static int64_t last_key_press_ms = INT64_MIN / 2;
static int64_t last_non_mod_key_press_ms = INT64_MIN / 2;

static void neutralize(struct input_event *event) {
    if (event->type == INPUT_EV_KEY) {
        /* Same trick as behaviors-consume: unused button, keeps evt->sync intact. */
        event->code = INPUT_BTN_8;
    }
    event->value = 0;
}

static int typing_guard_handle_event(const struct device *dev, struct input_event *event,
                                     uint32_t param1, uint32_t param2,
                                     struct zmk_input_processor_state *state) {
    ARG_UNUSED(param1);
    ARG_UNUSED(param2);

    const struct typing_guard_config *cfg = dev->config;
    struct typing_guard_data *data = dev->data;

    if (event->type != cfg->type) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    size_t idx;
    for (idx = 0; idx < cfg->codes_len; idx++) {
        if (cfg->codes[idx] == event->code) {
            break;
        }
    }
    if (idx == cfg->codes_len) {
        return ZMK_INPUT_PROC_CONTINUE;
    }

    uint32_t *dropped = &data->dropped[state->input_device_index % TYPING_GUARD_MAX_LISTENERS];

    if (event->type == INPUT_EV_KEY && event->value == 0) {
        /* Release: drop only if the matching press was dropped. */
        if (*dropped & BIT(idx)) {
            *dropped &= ~BIT(idx);
            neutralize(event);
        }
        return ZMK_INPUT_PROC_CONTINUE;
    }

    int64_t last = cfg->ignore_modifiers ? last_non_mod_key_press_ms : last_key_press_ms;
    if (k_uptime_get() - last < cfg->require_prior_idle_ms) {
        LOG_DBG("typing guard: drop type %d code %d", event->type, event->code);
        if (event->type == INPUT_EV_KEY) {
            *dropped |= BIT(idx);
        }
        neutralize(event);
    }

    return ZMK_INPUT_PROC_CONTINUE;
}

static int typing_guard_keycode_listener(const zmk_event_t *eh) {
    const struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev == NULL || !ev->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    int64_t now = k_uptime_get();

    last_key_press_ms = now;
    if (!is_mod(ev->usage_page, ev->keycode)) {
        last_non_mod_key_press_ms = now;
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(processor_typing_guard, typing_guard_keycode_listener);
ZMK_SUBSCRIPTION(processor_typing_guard, zmk_keycode_state_changed);

static struct zmk_input_processor_driver_api typing_guard_driver_api = {
    .handle_event = typing_guard_handle_event,
};

static int typing_guard_init(const struct device *dev) { return 0; }

#define TYPING_GUARD_INST(n)                                                                       \
    static const uint16_t typing_guard_codes_##n[] = DT_INST_PROP(n, codes);                       \
    BUILD_ASSERT(ARRAY_SIZE(typing_guard_codes_##n) <= 32, "at most 32 codes are supported");     \
    static struct typing_guard_data typing_guard_data_##n = {};                                    \
    static const struct typing_guard_config typing_guard_config_##n = {                            \
        .require_prior_idle_ms = DT_INST_PROP(n, require_prior_idle_ms),                           \
        .ignore_modifiers = DT_INST_PROP(n, ignore_modifiers),                                     \
        .type = DT_INST_PROP(n, type),                                                             \
        .codes_len = ARRAY_SIZE(typing_guard_codes_##n),                                           \
        .codes = typing_guard_codes_##n,                                                           \
    };                                                                                             \
    DEVICE_DT_INST_DEFINE(n, &typing_guard_init, NULL, &typing_guard_data_##n,                     \
                          &typing_guard_config_##n, POST_KERNEL,                                   \
                          CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &typing_guard_driver_api);

DT_INST_FOREACH_STATUS_OKAY(TYPING_GUARD_INST)
