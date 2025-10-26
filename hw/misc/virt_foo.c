/* Virtual Foo Device with message queues*/
#include "qemu/osdep.h"

#include <mqueue.h>

#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "qapi/error.h"
#include "qemu/thread.h"
#include "qemu/main-loop.h"


#define VIRT_FOO_DEBUG
#ifdef VIRT_FOO_DEBUG
#define DPRINTF(fmt, ...) qemu_log_mask(LOG_GUEST_ERROR, "[virt_foo] " fmt, ##__VA_ARGS__)
#else
#define DPRINTF(fmt, ...) do {} while (0)
#endif

#define TYPE_VIRT_FOO   "virt-foo"
// OBJECT_DECLARE_SIMPLE_TYPE(VirtFooState, VIRT_FOO)
#define VIRT_FOO(obj)   OBJECT_CHECK(VirtFooState, (obj), TYPE_VIRT_FOO)

#define VIRT_FOO_REQ_Q_NAME "/virt_foo_req_mq"
#define VIRT_FOO_RESP_Q_NAME "/virt_foo_resp_mq"
#define VIRT_FOO_IRQ_Q_NAME "/virt_foo_irq_mq"

#define VIRT_FOO_NO_DATA 0

// Register map
#define IRQ_CLR          0x10

typedef struct {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t irq_status;
    struct mq_attr req_queue_attr, resp_queue_attr, irq_queue_attr;
    mqd_t req_queue, resp_queue, irq_queue;
    QemuThread irq_thread;
} VirtFooState;

typedef enum {FOO_READ, FOO_WRITE} virt_foo_op;

typedef struct {
    hwaddr addr;
    uint64_t data;
    uint32_t size;
    virt_foo_op op;
} virt_foo_req;

typedef struct {
    uint64_t data;
    int error;
} virt_foo_resp;

typedef struct {
    uint32_t data;
    int error;
} virt_foo_irq;


static void virt_foo_set_irq_status(void *opaque)
{
    VirtFooState *s = (VirtFooState *)opaque;
    DPRINTF("Set IRQ: %d\n", s->irq_status);
    qemu_set_irq(s->irq, s->irq_status);
}

static uint64_t virt_foo_read(void *opaque, hwaddr offset, unsigned size)
{
    DPRINTF("Read from offset 0x%lx of size %d\n", offset, size);

    VirtFooState *s = (VirtFooState *)opaque;

    // Create a message
    virt_foo_req req = { .addr = offset, .data = VIRT_FOO_NO_DATA, .size = size, .op = FOO_READ };
    // Send message
    if (mq_send(s->req_queue, (char *)&req, sizeof(virt_foo_req), 0) == -1) {
        DPRINTF("Cannot send the request message\n");
        // TODO: Some error handling
    }
    // Wait for message in response
    virt_foo_resp resp;
    ssize_t bytes = mq_receive(s->resp_queue, (char *)&resp, sizeof(virt_foo_resp), NULL);
    if (bytes < 0) {
        DPRINTF("Cannot send the request message\n");
        // TODO: Some error handling
    }

    // Check if response is valid
    if (resp.error == 0) {
        return resp.data;
    }

    DPRINTF("Got error in response: %d\n", resp.error);
    // TODO: Some error handling
    return 0;
}

static void virt_foo_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    DPRINTF("Write 0x%lx to offset 0x%lx\n", value, offset);

    VirtFooState *s = (VirtFooState *)opaque;

    // Create a message
    virt_foo_req req = { .addr = offset, .data = value, .size = size, .op = FOO_WRITE };
    // Send message
    if (mq_send(s->req_queue, (char *)&req, sizeof(virt_foo_req), 0) == -1) {
        DPRINTF("Cannot send the request message\n");
        // TODO: Some error handling
    }
    // Wait for message in response
    virt_foo_resp resp;
    ssize_t bytes = mq_receive(s->resp_queue, (char *)&resp, sizeof(virt_foo_resp), NULL);
    if (bytes < 0) {
        DPRINTF("Cannot send the request message\n");
        // TODO: Some error handling
    }

    // Check if response is valid
    if (resp.error) {
        DPRINTF("Got error in response: %d\n", resp.error);
    }
}

static const MemoryRegionOps virt_foo_ops = {
    .read = virt_foo_read,
    .write = virt_foo_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void virt_foo_create_mqs(VirtFooState *s, Error **errp)
{
    // Configure request message queue
    s->req_queue_attr.mq_maxmsg = 1;
    s->req_queue_attr.mq_msgsize = sizeof(virt_foo_req);
    s->req_queue_attr.mq_flags = 0;
    s->req_queue_attr.mq_curmsgs = 0;
    // Open request message queue
    s->req_queue = mq_open(VIRT_FOO_REQ_Q_NAME, O_CREAT|O_WRONLY, S_IRWXU, &s->req_queue_attr);
    if (s->req_queue == (mqd_t)-1) {
        error_setg(errp, "virt-foo: Could not open the %s message queue", VIRT_FOO_REQ_Q_NAME);
    }

    // Configure response message queue
    s->resp_queue_attr.mq_maxmsg = 1;
    s->resp_queue_attr.mq_msgsize = sizeof(virt_foo_resp);
    s->resp_queue_attr.mq_flags = 0;
    s->resp_queue_attr.mq_curmsgs = 0;
    // Open response message queue
    s->resp_queue = mq_open(VIRT_FOO_RESP_Q_NAME, O_CREAT|O_RDONLY, S_IRWXU, &s->resp_queue_attr);
    if (s->resp_queue == (mqd_t)-1) {
        error_setg(errp, "virt-foo: Could not open the %s message queue", VIRT_FOO_RESP_Q_NAME);
    }

    // Configure irq message queue
    s->irq_queue_attr.mq_maxmsg = 1;
    s->irq_queue_attr.mq_msgsize = sizeof(virt_foo_irq);
    s->irq_queue_attr.mq_flags = 0;
    s->irq_queue_attr.mq_curmsgs = 0;
    // Open irq message queue
    s->irq_queue = mq_open(VIRT_FOO_IRQ_Q_NAME, O_CREAT|O_RDONLY, S_IRWXU, &s->irq_queue_attr);
    if (s->irq_queue == (mqd_t)-1) {
        error_setg(errp, "virt-foo: Could not open the %s message queue", VIRT_FOO_IRQ_Q_NAME);
    }
}

static void *irq_thread(void *opaque)
{
    VirtFooState *s = opaque;
    virt_foo_irq irq;

    while (1) {
        ssize_t bytes = mq_receive(s->irq_queue, (char *)&irq, sizeof(virt_foo_irq), NULL);
        if (bytes < 0) {
            DPRINTF("Cannot receive the irq message\n");
            // TODO: Some error handling
        } else {
            // Get new IRQ status from the message (should I lock it?)
            s->irq_status = irq.data;
            // Schedule IRQ for raising
            aio_bh_schedule_oneshot(qemu_get_aio_context(), virt_foo_set_irq_status, s);
        }
    }
    return NULL;
}

static void virt_foo_realize(DeviceState *d, Error **errp)
{
    VirtFooState *s = VIRT_FOO(d);
    SysBusDevice *sbd = SYS_BUS_DEVICE(d);

    memory_region_init_io(&s->iomem, OBJECT(s), &virt_foo_ops, s, TYPE_VIRT_FOO, 0x200);
    sysbus_init_mmio(sbd, &s->iomem);
    // Single interrupt line
    sysbus_init_irq(sbd, &s->irq);
    // Create queues
    virt_foo_create_mqs(s, errp);
    // Create an IRQ thread
    qemu_thread_create(&s->irq_thread, "irq_thread", irq_thread, s, QEMU_THREAD_DETACHED);
}

static void virt_foo_unrealize(DeviceState *d)
{
    // Clean-up
    VirtFooState *s = VIRT_FOO(d);
    // SysBusDevice *sbd = SYS_BUS_DEVICE(d);

    mq_close(s->req_queue);
    mq_unlink(VIRT_FOO_REQ_Q_NAME);

    mq_close(s->resp_queue);
    mq_unlink(VIRT_FOO_RESP_Q_NAME);

    mq_close(s->irq_queue);
    mq_unlink(VIRT_FOO_IRQ_Q_NAME);

    // TODO: Kill the irq_thread
}

static void virt_foo_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->realize = virt_foo_realize;
    dc->unrealize = virt_foo_unrealize;
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
