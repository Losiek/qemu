/* Virtual Hardware Cosimulation with Memory Mapped IO Device with message
 * queues*/
#include "qemu/osdep.h"

#include <mqueue.h>
#include <stdint.h>

#include "hw/hw.h"
#include "hw/irq.h"
#include "hw/sysbus.h"
#include "qapi/error.h"
#include "qemu/bitops.h"
#include "qemu/log.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/thread.h"
#include "qom/object.h"

#define VIRT_HW_COSIM_MMIO_DEBUG 1
#ifdef VIRT_HW_COSIM_MMIO_DEBUG
#define DPRINTF(fmt, ...)                                                      \
    qemu_log_mask(LOG_GUEST_ERROR, "[virt_hw_cosim_mmio] " fmt, ##__VA_ARGS__)
#else
#define DPRINTF(fmt, ...)                                                      \
    do {                                                                         \
    } while (0)
#endif

#define PRINT_REQ(req) DPRINTF("Request: \naddr=0x%lx\n\tdata=0x%lx\n\tsize=%d\n\top=%d\n", req.addr, req.data, req.size, req.op)
#define PRINT_RESP(resp) DPRINTF("Response: \data=0x%lx\n\error=0x%lx\n", resp.data, resp.error)

#define LOG_ERR(fmt, ...)                                                      \
    qemu_log_mask(LOG_GUEST_ERROR, "[virt_hw_cosim_mmio] " fmt, ##__VA_ARGS__)

#define TYPE_VIRT_HW_COSIM_MMIO "virt-hw-cosim-mmio"

#define VIRT_HW_COSIM_MMIO(obj)                                                \
    OBJECT_CHECK(VirtHwCosimMmioState, (obj), TYPE_VIRT_HW_COSIM_MMIO)

#define VIRT_HW_COSIM_MMIO_REQ_Q_NAME "/virt_hw_cosim_mmio_req_mq"
#define VIRT_HW_COSIM_MMIO_RESP_Q_NAME "/virt_hw_cosim_mmio_resp_mq"
#define VIRT_HW_COSIM_MMIO_IRQ_Q_NAME "/virt_hw_cosim_mmio_irq_mq"

#define VIRT_HW_COSIM_MMIO_NO_DATA 0

#define VIRT_HW_COSIM_MMIO_ACCESS_SIZE 8
#define VIRT_HW_COSIM_MMIO_MIN_ACCESS_SIZE 1
#define VIRT_HW_COSIM_MMIO_MAX_ACCESS_SIZE 8
#define VIRT_HW_COSIM_MMIO_UNALIGNED false
#define VIRT_HW_COSIM_MMIO_DECODE_ERROR -1
#define VIRT_HW_COSIM_MMIO_ACCESS_ERROR -2

// VIRT_HW_COSIM_MMIO device registers
#define REQ_PENDING 0
#define RESP_READY 1
#define RESP_ERROR 2

#define RESP_CLR 1

#define VIRT_HW_COSIM_MMIO_STATUS                                              \
    0x00 // 0 bit -> pending request, 1 bit -> pending response, 2 bit -> resp
         // error
#define VIRT_HW_COSIM_MMIO_ADDR 0x08
#define VIRT_HW_COSIM_MMIO_CMD 0x10
#define VIRT_HW_COSIM_MMIO_WDATA 0x18
#define VIRT_HW_COSIM_MMIO_RDATA 0x20
#define VIRT_HW_COSIM_MMIO_ERRCODE 0x28
#define VIRT_HW_COSIM_MMIO_CONTROL 0x36 // 0 bit -> reset, 1 bit -> resp_clr

typedef enum { HW_COSIM_MMIO_READ, HW_COSIM_MMIO_WRITE } virt_hw_cosim_mmio_op;

typedef struct {
    hwaddr addr;
    uint64_t data;
    uint32_t size;
    virt_hw_cosim_mmio_op op;
} virt_hw_cosim_mmio_req;

typedef struct {
    uint64_t data;
    int error;
} virt_hw_cosim_mmio_resp;

typedef struct {
    uint32_t data;
    int error;
} virt_hw_cosim_mmio_irq;

typedef enum { IDLE, BUSY } state;

typedef struct {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    QemuMutex lock;
    QemuCond cond;

    state state_m;

    struct mq_attr req_queue_attr;
    struct mq_attr resp_queue_attr;
    struct mq_attr irq_queue_attr;

    mqd_t req_queue;
    mqd_t resp_queue;
    mqd_t irq_queue;

    virt_hw_cosim_mmio_req req_m;
    bool req_pending;
    virt_hw_cosim_mmio_resp resp_m;
    bool resp_pending;
    bool resp_error;
    MemTxResult result;
    QemuThread io_thread;

    qemu_irq irq;
    uint32_t irq_status;
    QemuThread irq_thread;

    // Registers
    uint64_t addr_m;
    uint64_t cmd_m;
    uint64_t wdata_m;
    uint64_t rdata_m;
    uint64_t errcode_m;
    uint64_t control_m;
} VirtHwCosimMmioState;


static void *virt_hw_cosim_mmio_io_thread(void *opaque) {
    DPRINTF("Starting IO thread\n");
    VirtHwCosimMmioState *s = (VirtHwCosimMmioState *)opaque;

    qemu_mutex_lock(&s->lock);
    while (true) {
        // Wait for a request from quest
        while (!s->req_pending) {
            qemu_cond_wait(&s->cond, &s->lock);
        }

        // Copy request and clear pending flag
        virt_hw_cosim_mmio_req req = s->req_m;
        s->req_pending = false;

        qemu_mutex_unlock(&s->lock);

        // Send message
        DPRINTF("Sending message to the queue\n");
        PRINT_REQ(req);
        if (mq_send(s->req_queue, (char *)&req, sizeof(virt_hw_cosim_mmio_req),
                    0) == -1) {
            LOG_ERR("Cannot send the request message\n");
            qemu_mutex_lock(&s->lock);
            s->result = MEMTX_ERROR;
            qemu_mutex_unlock(&s->lock);
            goto error;
        }

        // Wait for message in response
        DPRINTF("Waiting for the message\n");
        virt_hw_cosim_mmio_resp resp;
        ssize_t bytes = mq_receive(s->resp_queue, (char *)&resp,
                sizeof(virt_hw_cosim_mmio_resp), NULL);
        if (bytes < 0) {
            LOG_ERR("Cannot receive the resposnse message\n");
            qemu_mutex_lock(&s->lock);
            s->result = MEMTX_ERROR;
            qemu_mutex_unlock(&s->lock);
            goto error;
        }
        DPRINTF("[IO] Got response. Data: 0x%lx, error: %u\n", resp.data, resp.error);

        qemu_mutex_lock(&s->lock);
        s->resp_m = resp;
        s->resp_pending = true;
        // Unpack the response to the registers
        s->rdata_m = s->resp_m.data;
        s->errcode_m = s->resp_m.error;
        qemu_mutex_unlock(&s->lock);
    }

error:
    return NULL;
}

static void virt_hw_cosim_mmio_set_irq_status(void *opaque) {
    VirtHwCosimMmioState *s = (VirtHwCosimMmioState *)opaque;
    DPRINTF("Set IRQ: %d\n", s->irq_status);
    qemu_set_irq(s->irq, s->irq_status);
}

static inline uint64_t mask_read_value(hwaddr offset, uint64_t data,
        unsigned size) {
    switch (size) {
        case 1:
            data >>= (offset & 7) * 8;
            data &= 0xff;
            break;
        case 2:
            data >>= (offset & 7) * 8;
            data &= 0xffff;
            break;
        case 4:
            data >>= (offset & 7) * 8;
            data &= 0xffffffff;
            break;
        case 8:
            break;
        default:
            data = 0;
            break;
    }
    return data;
}

static MemTxResult
virt_hw_cosim_mmio_read_with_attrs(void *opaque, hwaddr offset, uint64_t *data,
        unsigned size, MemTxAttrs attrs) {
    // Left for debug
    // DPRINTF("Read from offset 0x%lx of size %d\n", offset, size);

    VirtHwCosimMmioState *s = (VirtHwCosimMmioState *)opaque;

    hwaddr word_offset = (hwaddr)(offset / 8) * 8;

    qemu_mutex_lock(&s->lock);

    switch (word_offset) {
        case VIRT_HW_COSIM_MMIO_STATUS:
            *data = (s->resp_error << RESP_ERROR) | (s->resp_pending << RESP_READY) |
                (s->req_pending << REQ_PENDING);
            break;
        case VIRT_HW_COSIM_MMIO_ADDR:
            *data = s->addr_m;
            break;
        case VIRT_HW_COSIM_MMIO_CMD:
            *data = s->cmd_m;
            break;
        case VIRT_HW_COSIM_MMIO_WDATA:
            *data = s->wdata_m;
            break;
        case VIRT_HW_COSIM_MMIO_RDATA:
            *data = s->rdata_m;
            break;
        case VIRT_HW_COSIM_MMIO_ERRCODE:
            *data = s->errcode_m;
            break;
        case VIRT_HW_COSIM_MMIO_CONTROL:
            *data = s->control_m;
            break;
        default:
            LOG_ERR("Unknown device address: 0x%lx", offset);
            goto error;
    }

    // Do proper masking/shifting for sizes <= 8 bytes
    *data = mask_read_value(offset, *data, size);

    qemu_mutex_unlock(&s->lock);
    return MEMTX_OK;

error:
    qemu_mutex_unlock(&s->lock);
    return MEMTX_DECODE_ERROR;
}

static inline uint64_t mask_write_value(hwaddr offset, uint64_t data,
        unsigned size, uint64_t old_data) {
    uint64_t mask = UINT64_MAX;
    unsigned shift = 0;
    switch (size) {
        case 1:
            data &= 0xff;
            mask = 0xff;
            break;
        case 2:
            data &= 0xffff;
            mask = 0xffff;
            break;
        case 4:
            data &= 0xffffffff;
            mask = 0xffffffff;
            break;
        case 8:
            break;
        default:
            data = 0;
            break;
    }
    shift = (offset & 7) * 8;
    data <<= shift;
    mask <<= shift;
    return (old_data & ~mask) | data;
}

static MemTxResult
virt_hw_cosim_mmio_write_with_attrs(void *opaque, hwaddr offset, uint64_t value,
        unsigned size, MemTxAttrs attrs) {
    DPRINTF("Write 0x%lx to offset 0x%lx, with size %u\n", value, offset, size);

    VirtHwCosimMmioState *s = (VirtHwCosimMmioState *)opaque;

    qemu_mutex_lock(&s->lock);
    MemTxResult rval = MEMTX_OK;

    hwaddr word_offset = (hwaddr)(offset / 8) * 8;
    uint64_t old_value;

    switch (word_offset) {
        case VIRT_HW_COSIM_MMIO_STATUS:
            old_value = (s->resp_pending << RESP_READY) | (s->resp_error << RESP_ERROR);
            value = mask_write_value(offset, value, size, old_value);
            // Can clear the status bits
            if (value & (1 << RESP_READY))
                s->resp_pending = false;
            if (value & (1 << RESP_ERROR))
                s->resp_error = false;
            break;
        case VIRT_HW_COSIM_MMIO_ADDR:
            s->addr_m = mask_write_value(offset, value, size, s->addr_m);
            break;
        case VIRT_HW_COSIM_MMIO_CMD:
            // Writing (at first byte) to command register will trigger sending message
            // to the request queue and will clear the response pending bit
            s->cmd_m = mask_write_value(offset, value, size, s->cmd_m);
            if (offset == word_offset) {
                // Already pending request
                if (s->req_pending) {
                    DPRINTF("Request already pending\n");
                    // rval = MEMTX_ERROR;
                    break;
                }
                virt_hw_cosim_mmio_op op = (virt_hw_cosim_mmio_op)s->cmd_m;
                uint64_t data = 0;
                if (op == HW_COSIM_MMIO_WRITE) {
                    data = s->wdata_m;
                }
                // Create a message
                virt_hw_cosim_mmio_req req = {
                    .addr = s->addr_m,
                    .data = data,
                    .size = size,
                    .op = op
                };

                s->req_m = req;
                s->req_pending = true;
                s->state_m = (s->req_pending || s->resp_pending);
                // Clear response pending bit
                s->resp_pending = false;
                // Wake the IO thread
                qemu_cond_signal(&s->cond);
            }
            break;
        case VIRT_HW_COSIM_MMIO_WDATA:
            s->wdata_m = mask_write_value(offset, value, size, s->wdata_m);
            break;
        case VIRT_HW_COSIM_MMIO_RDATA:
            s->rdata_m = mask_write_value(offset, value, size, s->rdata_m);
            break;
        case VIRT_HW_COSIM_MMIO_ERRCODE:
            DPRINTF("Writing to ERRCODE register has no effect\n");
            break;
        case VIRT_HW_COSIM_MMIO_CONTROL:
            s->control_m = mask_write_value(offset, value, size, s->control_m);
            break;
        default:
            LOG_ERR("Unknown device address: 0x%lx", offset);
            rval = MEMTX_DECODE_ERROR;
    }
    qemu_mutex_unlock(&s->lock);
    return rval;
}

static const MemoryRegionOps virt_hw_cosim_mmio_ops = {
    .read_with_attrs = virt_hw_cosim_mmio_read_with_attrs,
    .write_with_attrs = virt_hw_cosim_mmio_write_with_attrs,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid =
    {
        .min_access_size = VIRT_HW_COSIM_MMIO_MIN_ACCESS_SIZE,
        .max_access_size = VIRT_HW_COSIM_MMIO_MAX_ACCESS_SIZE,
        .unaligned = VIRT_HW_COSIM_MMIO_UNALIGNED,
    },
    .impl =
    {
        .min_access_size = VIRT_HW_COSIM_MMIO_MIN_ACCESS_SIZE,
        .max_access_size = VIRT_HW_COSIM_MMIO_MAX_ACCESS_SIZE,
    },
};

static void virt_hw_cosim_mmio_create_mqs(VirtHwCosimMmioState *s,
        Error **errp) {
    // Configure request message queue
    s->req_queue_attr.mq_maxmsg = 1;
    s->req_queue_attr.mq_msgsize = sizeof(virt_hw_cosim_mmio_req);
    s->req_queue_attr.mq_flags = 0;
    s->req_queue_attr.mq_curmsgs = 0;
    // First, remove the queue
    mq_unlink(VIRT_HW_COSIM_MMIO_REQ_Q_NAME);
    // Open request message queue
    s->req_queue =
        mq_open(VIRT_HW_COSIM_MMIO_REQ_Q_NAME, O_CREAT | O_EXCL | O_WRONLY,
                S_IRWXU, &s->req_queue_attr);
    if (s->req_queue == (mqd_t)-1) {
        error_setg(errp, "virt-hw-cosim-mmio: Could not open the %s message queue",
                VIRT_HW_COSIM_MMIO_REQ_Q_NAME);
    }

    // Configure response message queue
    s->resp_queue_attr.mq_maxmsg = 1;
    s->resp_queue_attr.mq_msgsize = sizeof(virt_hw_cosim_mmio_resp);
    s->resp_queue_attr.mq_flags = 0;
    s->resp_queue_attr.mq_curmsgs = 0;
    // First, remove the queue
    mq_unlink(VIRT_HW_COSIM_MMIO_RESP_Q_NAME);
    // Open response message queue
    s->resp_queue =
        mq_open(VIRT_HW_COSIM_MMIO_RESP_Q_NAME, O_CREAT | O_EXCL | O_RDONLY,
                S_IRWXU, &s->resp_queue_attr);
    if (s->resp_queue == (mqd_t)-1) {
        error_setg(errp, "virt-hw-cosim-mmio: Could not open the %s message queue",
                VIRT_HW_COSIM_MMIO_RESP_Q_NAME);
    }

    // Configure irq message queue
    s->irq_queue_attr.mq_maxmsg = 1;
    s->irq_queue_attr.mq_msgsize = sizeof(virt_hw_cosim_mmio_irq);
    s->irq_queue_attr.mq_flags = 0;
    s->irq_queue_attr.mq_curmsgs = 0;
    // First, remove the queue
    mq_unlink(VIRT_HW_COSIM_MMIO_IRQ_Q_NAME);
    // Open irq message queue
    s->irq_queue =
        mq_open(VIRT_HW_COSIM_MMIO_IRQ_Q_NAME, O_CREAT | O_EXCL | O_RDONLY,
                S_IRWXU, &s->irq_queue_attr);
    if (s->irq_queue == (mqd_t)-1) {
        error_setg(errp, "virt-hw-cosim-mmio: Could not open the %s message queue",
                VIRT_HW_COSIM_MMIO_IRQ_Q_NAME);
    }
}

static void *virt_hw_cosim_mmio_irq_thread(void *opaque) {
    VirtHwCosimMmioState *s = opaque;
    virt_hw_cosim_mmio_irq irq;

    DPRINTF("Starting IRQ thread\n");
    while (true) {
        ssize_t bytes = mq_receive(s->irq_queue, (char *)&irq,
                sizeof(virt_hw_cosim_mmio_irq), NULL);
        if (bytes < 0) {
            LOG_ERR("Cannot receive the irq message\n");
        } else {
            // Get new IRQ status from the message
            qemu_mutex_lock(&s->lock);
            s->irq_status = irq.data;
            qemu_mutex_unlock(&s->lock);
            // Schedule IRQ for raising
            aio_bh_schedule_oneshot(qemu_get_aio_context(),
                    virt_hw_cosim_mmio_set_irq_status, s);
        }
    }
    return NULL;
}

static void virt_hw_cosim_mmio_realize(DeviceState *d, Error **errp) {
    DPRINTF("virt_hw_cosim_mmio_realize\n");

    VirtHwCosimMmioState *s = VIRT_HW_COSIM_MMIO(d);
    SysBusDevice *sbd = SYS_BUS_DEVICE(d);
    // Initialize memory region
    memory_region_init_io(&s->iomem, OBJECT(s), &virt_hw_cosim_mmio_ops, s,
            TYPE_VIRT_HW_COSIM_MMIO, 0x200);
    // Initialize the SysBus
    sysbus_init_mmio(sbd, &s->iomem);
    // Initialize mutex
    qemu_mutex_init(&s->lock);
    // Initialize condition
    qemu_cond_init(&s->cond);
    // Single interrupt line
    sysbus_init_irq(sbd, &s->irq);
    // Create queues
    virt_hw_cosim_mmio_create_mqs(s, errp);
    // Initial values
    s->state_m = IDLE;
    s->req_pending = false;
    s->resp_pending = false;
    // Create an IO thread
    qemu_thread_create(&s->io_thread, "virt_hw_cosim_mmio_io_thread",
            virt_hw_cosim_mmio_io_thread, s, QEMU_THREAD_DETACHED);
    // Create an IRQ thread
    qemu_thread_create(&s->irq_thread, "virt_hw_cosim_mmio_irq_thread",
            virt_hw_cosim_mmio_irq_thread, s, QEMU_THREAD_DETACHED);
}

static void virt_hw_cosim_mmio_unrealize(DeviceState *d) {
    // Clean-up
    VirtHwCosimMmioState *s = VIRT_HW_COSIM_MMIO(d);
    // SysBusDevice *sbd = SYS_BUS_DEVICE(d);

    mq_close(s->req_queue);
    mq_unlink(VIRT_HW_COSIM_MMIO_REQ_Q_NAME);

    mq_close(s->resp_queue);
    mq_unlink(VIRT_HW_COSIM_MMIO_RESP_Q_NAME);

    mq_close(s->irq_queue);
    mq_unlink(VIRT_HW_COSIM_MMIO_IRQ_Q_NAME);

    // TODO: Kill the irq_thread
}

static void virt_hw_cosim_mmio_reset(DeviceState *d) {
    VirtHwCosimMmioState *s = VIRT_HW_COSIM_MMIO(d);
    DPRINTF("Reset\n");
    // Ensure IRQ is de-asserted on reset
    qemu_set_irq(s->irq, 0);
}

static void virt_hw_cosim_mmio_class_init(ObjectClass *klass, void *data) {
    qemu_log_mask(LOG_UNIMP, "virt_hw_cosim_mmio class initialized!\n");
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = virt_hw_cosim_mmio_realize;
    dc->unrealize = virt_hw_cosim_mmio_unrealize;
    dc->legacy_reset = virt_hw_cosim_mmio_reset;
}

static const TypeInfo virt_hw_cosim_mmio_info = {
    .name = TYPE_VIRT_HW_COSIM_MMIO,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(VirtHwCosimMmioState),
    .class_init = virt_hw_cosim_mmio_class_init,
};

static void virt_hw_cosim_mmio_register_types(void) {
    type_register_static(&virt_hw_cosim_mmio_info);
}

type_init(virt_hw_cosim_mmio_register_types)
