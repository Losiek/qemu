/* Virtula Foo Device */
#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qom/object.h"

#define VIRT_FOO_DEBUG
#ifdef VIRT_FOO_DEBUG
#define DPRINTF(fmt, ...) qemu_log_mask(LOG_GUEST_ERROR, "[virt_foo] " fmt, ##__VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

#define TYPE_VIRT_FOO   "virt-foo"
// OBJECT_DECLARE_SIMPLE_TYPE(VirtFooState, VIRT_FOO)
#define VIRT_FOO(obj)   OBJECT_CHECK(VirtFooState, (obj), TYPE_VIRT_FOO)

// Register map
#define REG_ID          0x0
#define CHIP_ID         0xf001

#define REG_INIT        0x4
#define CHIP_EN         BIT(0)

#define REG_CMD         0x8

#define REG_INIT_STATUS 0xc
#define INT_ENABLED     BIT(0)
#define INT_BUFFER_DEQ  BIT(1)


typedef struct {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t id;
    uint32_t init;
    uint32_t cmd;
    uint32_t status;
} VirtFooState;

static void virt_foo_set_irq(VirtFooState *s, int irq)
{
    DPRINTF("Set IRQ 0x%x\n", irq);

    s->status = irq;
    qemu_set_irq(s->irq, 1);
}

static void virt_foo_clear_irq(VirtFooState *s)
{
    DPRINTF("Clear IRQ\n");
    qemu_set_irq(s->irq, 0);
}

static uint64_t virt_foo_read(void *opaque, hwaddr offset, unsigned size)
{
    DPRINTF("Read from offset 0x%lx\n", offset);

    VirtFooState *s = (VirtFooState *)opaque;
    bool is_enabled = s->init & CHIP_EN;

    if (!is_enabled) {
        fprintf(stderr, "[virt-foo] Device is disabled\n");
        return 0;
    }

    switch (offset) {
    case REG_ID:
        return s->id;
    case REG_INIT:
        return s->init;
    case REG_CMD:
        return s->cmd;
    case REG_INIT_STATUS:
        virt_foo_clear_irq(s);
        return s->status;
    default:
        break;
    }

    return 0;
}

static void virt_foo_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    DPRINTF("Write 0x%lx to offset 0x%lx\n", value, offset);

    VirtFooState *s = (VirtFooState *)opaque;

    switch (offset) {
    case REG_INIT:
        s->init = (uint32_t)value;
        if (value)
            virt_foo_set_irq(s, INT_ENABLED);
        break;
    case REG_CMD:
        s->cmd = (uint32_t)value;
        virt_foo_set_irq(s, INT_BUFFER_DEQ);
        break;
    default:
        break;
    }
}

static const MemoryRegionOps virt_foo_ops = {
    .read = virt_foo_read,
    .write = virt_foo_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void virt_foo_realize(DeviceState *d, Error **errp)
{
    VirtFooState *s = VIRT_FOO(d);
    SysBusDevice *sbd = SYS_BUS_DEVICE(d);

    memory_region_init_io(&s->iomem, OBJECT(s), &virt_foo_ops, s, TYPE_VIRT_FOO, 0x200);
    sysbus_init_mmio(sbd, &s->iomem);
    // Single interrupt line
    sysbus_init_irq(sbd, &s->irq);

    s->id = CHIP_ID;
    s->init = 0;
}

static void virt_foo_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = virt_foo_realize;
}

static const TypeInfo virt_foo_info = {
    .name           = TYPE_VIRT_FOO,
    .parent         = TYPE_SYS_BUS_DEVICE,
    .instance_size  = sizeof(VirtFooState),
    .class_init     = virt_foo_class_init,
};

static void virt_foo_register_types(void)
{
    type_register_static(&virt_foo_info);
}

type_init(virt_foo_register_types)
