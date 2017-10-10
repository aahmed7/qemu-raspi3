/*
 * Raspberry Pi emulation (c) 2012 Gregory Estrade
 * Upstreaming code cleanup [including bcm2835_*] (c) 2013 Jan Petrous
 *
 * Rasperry Pi 2 emulation and refactoring Copyright (c) 2015, Microsoft
 * Written by Andrew Baumann
 *
 * Raspberry Pi 3 emulation 2017 by bzt
 *
 * This code is licensed under the GNU GPLv2 and later.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu-common.h"
#include "cpu.h"
#include "hw/arm/bcm2837.h"
#include "hw/arm/raspi_platform.h"
#include "hw/sysbus.h"
#include "exec/address-spaces.h"

/* According to https://www.raspberrypi.org/documentation/hardware/raspberrypi/bcm2837/README.md
 * The underlying architecture of the BCM2837 is identical to the BCM2836. The only significant
 * difference is the replacement of the ARMv7 quad core cluster with a quad-core ARM Cortex A53
 * (ARMv8) cluster. So we use cortex-a53- here. */

/* Peripheral base address seen by the CPU */
#define BCM2836_PERI_BASE       0x3F000000

/* "QA8" (Pi3) interrupt controller and mailboxes etc. */
#define BCM2836_CONTROL_BASE    0x40000000
 
static void bcm2837_init(Object *obj)
{
    BCM2837State *s = BCM2837(obj);
    int n;

    for (n = 0; n < BCM2837_NCPUS; n++) {
        object_initialize(&s->cpus[n], sizeof(s->cpus[n]),
                          "cortex-a53-" TYPE_ARM_CPU);
        object_property_add_child(obj, "cpu[*]", OBJECT(&s->cpus[n]),
                                  &error_abort);
    }

    object_initialize(&s->control, sizeof(s->control), TYPE_BCM2836_CONTROL);
    object_property_add_child(obj, "control", OBJECT(&s->control), NULL);
    qdev_set_parent_bus(DEVICE(&s->control), sysbus_get_default());

    object_initialize(&s->peripherals, sizeof(s->peripherals),
                      TYPE_BCM2835_PERIPHERALS);
    object_property_add_child(obj, "peripherals", OBJECT(&s->peripherals),
                              &error_abort);
    object_property_add_alias(obj, "board-rev", OBJECT(&s->peripherals),
                              "board-rev", &error_abort);
    object_property_add_alias(obj, "vcram-size", OBJECT(&s->peripherals),
                              "vcram-size", &error_abort);
    qdev_set_parent_bus(DEVICE(&s->peripherals), sysbus_get_default());
}

static void bcm2837_realize(DeviceState *dev, Error **errp)
{
    BCM2837State *s = BCM2837(dev);
    Object *obj;
    Error *err = NULL;
    int n;

    /* common peripherals from bcm2835 */

    obj = object_property_get_link(OBJECT(dev), "ram", &err);
    if (obj == NULL) {
        error_setg(errp, "%s: required ram link not found: %s",
                   __func__, error_get_pretty(err));
        return;
    }

    object_property_add_const_link(OBJECT(&s->peripherals), "ram", obj, &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    object_property_set_bool(OBJECT(&s->peripherals), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    object_property_add_alias(OBJECT(s), "sd-bus", OBJECT(&s->peripherals),
                              "sd-bus", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    sysbus_mmio_map_overlap(SYS_BUS_DEVICE(&s->peripherals), 0,
                            BCM2836_PERI_BASE, 1);

    /* bcm2836 interrupt controller (and mailboxes, etc.) */
    object_property_set_bool(OBJECT(&s->control), true, "realized", &err);
    if (err) {
        error_propagate(errp, err);
        return;
    }

    sysbus_mmio_map(SYS_BUS_DEVICE(&s->control), 0, BCM2836_CONTROL_BASE);

    sysbus_connect_irq(SYS_BUS_DEVICE(&s->peripherals), 0,
        qdev_get_gpio_in_named(DEVICE(&s->control), "gpu-irq", 0));
    sysbus_connect_irq(SYS_BUS_DEVICE(&s->peripherals), 1,
        qdev_get_gpio_in_named(DEVICE(&s->control), "gpu-fiq", 0));

    for (n = 0; n < BCM2837_NCPUS; n++) {
        /* Mirror bcm2836, which has clusterid set to 0xf
         * TODO: this should be converted to a property of ARM_CPU
         */
        s->cpus[n].mp_affinity = 0xF00 | n;

        /* set periphbase/CBAR value for CPU-local registers */
        object_property_set_int(OBJECT(&s->cpus[n]),
                                BCM2836_PERI_BASE + MCORE_OFFSET,
                                "reset-cbar", &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }

        /* start powered off if not enabled */
        object_property_set_bool(OBJECT(&s->cpus[n]), n >= s->enabled_cpus,
                                 "start-powered-off", &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }

        object_property_set_bool(OBJECT(&s->cpus[n]), true, "realized", &err);
        if (err) {
            error_propagate(errp, err);
            return;
        }

        /* Connect irq/fiq outputs from the interrupt controller. */
        qdev_connect_gpio_out_named(DEVICE(&s->control), "irq", n,
                qdev_get_gpio_in(DEVICE(&s->cpus[n]), ARM_CPU_IRQ));
        qdev_connect_gpio_out_named(DEVICE(&s->control), "fiq", n,
                qdev_get_gpio_in(DEVICE(&s->cpus[n]), ARM_CPU_FIQ));

        /* Connect timers from the CPU to the interrupt controller */
        qdev_connect_gpio_out(DEVICE(&s->cpus[n]), GTIMER_PHYS,
                qdev_get_gpio_in_named(DEVICE(&s->control), "cntpnsirq", n));
        qdev_connect_gpio_out(DEVICE(&s->cpus[n]), GTIMER_VIRT,
                qdev_get_gpio_in_named(DEVICE(&s->control), "cntvirq", n));
        qdev_connect_gpio_out(DEVICE(&s->cpus[n]), GTIMER_HYP,
                qdev_get_gpio_in_named(DEVICE(&s->control), "cnthpirq", n));
        qdev_connect_gpio_out(DEVICE(&s->cpus[n]), GTIMER_SEC,
                qdev_get_gpio_in_named(DEVICE(&s->control), "cntpsirq", n));
    }
}

static Property bcm2837_props[] = {
    DEFINE_PROP_UINT32("enabled-cpus", BCM2837State, enabled_cpus, BCM2837_NCPUS),
    DEFINE_PROP_END_OF_LIST()
};

static void bcm2837_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->props = bcm2837_props;
    dc->realize = bcm2837_realize;
}

static const TypeInfo bcm2837_type_info = {
    .name = TYPE_BCM2837,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(BCM2837State),
    .instance_init = bcm2837_init,
    .class_init = bcm2837_class_init,
};

static void bcm2837_register_types(void)
{
    type_register_static(&bcm2837_type_info);
}

type_init(bcm2837_register_types)