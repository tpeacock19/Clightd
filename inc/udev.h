#pragma once

#include "../inc/commons.h"

void get_udev_device(const char *backlight_interface, const char *subsystem,
                     sd_bus_error **ret_error, struct udev_device **dev);