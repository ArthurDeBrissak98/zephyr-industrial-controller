#include "controller_settings.h"
#include <zephyr/settings/settings.h>
#include <errno.h>
#include <string.h>

K_MUTEX_DEFINE(settings_lock);
static bool initialized;
static bool found;
static int read_error;
static struct controller_parameters loaded;

static int settings_set(const char *name, size_t size, settings_read_cb read_cb, void *arg)
{
    if (strcmp(name, "parameters")) return -ENOENT;
    uint8_t data[CONTROLLER_SETTINGS_SIZE];
    found = true;
    if (size != sizeof(data)) {
        read_error = -EINVAL;
        return 0;
    }
    ssize_t length = read_cb(arg, data, sizeof(data));
    if (length != (ssize_t)sizeof(data)) {
        read_error = length < 0 ? length : -EIO;
        return 0;
    }
    read_error = controller_settings_decode(data, sizeof(data), &loaded);
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(controller, "controller", NULL, settings_set, NULL, NULL);

int controller_settings_init(void)
{
    k_mutex_lock(&settings_lock, K_FOREVER);
    int ret = initialized ? 0 : settings_subsys_init();
    if (!ret) initialized = true;
    k_mutex_unlock(&settings_lock);
    return ret;
}

int controller_settings_read(struct controller_parameters *parameters)
{
    k_mutex_lock(&settings_lock, K_FOREVER);
    found = false;
    read_error = 0;
    int ret = initialized ? settings_load_subtree("controller") : -ENODEV;
    if (!ret) ret = read_error;
    if (!ret && !found) ret = -ENOENT;
    if (!ret) *parameters = loaded;
    k_mutex_unlock(&settings_lock);
    return ret;
}

int controller_settings_write(const struct controller_parameters *parameters)
{
    uint8_t data[CONTROLLER_SETTINGS_SIZE];
    int ret = controller_settings_encode(parameters, data);
    if (ret) return ret;
    k_mutex_lock(&settings_lock, K_FOREVER);
    ret = initialized ? settings_save_one("controller/parameters", data, sizeof(data)) : -ENODEV;
    k_mutex_unlock(&settings_lock);
    return ret;
}

int controller_settings_erase(void)
{
    k_mutex_lock(&settings_lock, K_FOREVER);
    int ret = initialized ? settings_delete("controller/parameters") : -ENODEV;
    k_mutex_unlock(&settings_lock);
    return ret;
}
