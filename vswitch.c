
#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/moduleparam.h>
#include <linux/kdev_t.h>
#include <linux/time.h>
#include <linux/semaphore.h>
#include <asm/uaccess.h>
#include <asm/errno.h>

#define DEVICE_NAME "vswitch"
#define CLASS_NAME "vswitch"

#define PORTS 4
#define MTU 128
#define BUFFER_SIZE 2048
#define CACHE_TTL 10 * 1000000 /* nanoseconds */

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Henrik Nyman");
MODULE_DESCRIPTION("A kernel module that implements a virtual network switch");
MODULE_VERSION("0.1");


static int major_number;
static struct class *vswitch_class = NULL;


static short int ports = PORTS;
module_param(ports, short, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(ports, "Number of ports in the switch (defaults to 4)");
static int mtu = MTU;
module_param(mtu, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(mtu, "Max transmission unit");
static int buffer_size = BUFFER_SIZE;
module_param(buffer_size, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(buffer_size, "Size of the circular buffer for each port");
static int cache_ttl = CACHE_TTL;
module_param(cache_ttl, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP);
MODULE_PARM_DESC(cache_ttl, "Time-To-Live of the host id cache row (in ms)");


static int dev_open(struct inode *, struct file *);
static int dev_release(struct inode *, struct file *);
static ssize_t dev_read(struct file *, char *, size_t, loff_t *);
static ssize_t dev_write(struct file *, const char *, size_t, loff_t *);

static struct file_operations fops =
{
    .open = dev_open,
    .read = dev_read,
    .write = dev_write,
    .release = dev_release
};

struct circ_buf
{
    char *buffer;      /* data buffer */
    char *buffer_end;  /* end of data buffer */
    size_t capacity;   /* maximum number of bytes in the buffer */
    size_t count;      /* number of unread bytes in the buffer */
    char *head;        /* pointer to head */
    char *tail;        /* pointer to tail */
};

struct vswitch_port
{
    unsigned short num;   /* ID of the port */
    int connected;        /* 1 if there is a device connected, 0 if not */
    unsigned int host;    /* ID of the device connected to the port */
    long valid_until;     /* Field host is valid until this timestamp */
    struct device *dev;   /* The device link */
    struct circ_buf buf;  /* Circular buffer for outgoing data */
    struct semaphore sem; /* Spinlock for writing data into buffer */
    wait_queue_head_t queue;
};

static struct vswitch_port *interfaces;

void cb_init(struct circ_buf *cb, size_t capacity)
{
    cb->buffer = kmalloc(capacity, GFP_KERNEL);
    if(cb->buffer == NULL)
    {
        // handle error
    }
    cb->buffer_end = cb->buffer + capacity;
    cb->capacity = capacity;
    cb->count = 0;
    /* cb->sz = sz; */
    cb->head = cb->buffer;
    cb->tail = cb->buffer;
}

void cb_clear(struct circ_buf *cb)
{
    cb->count = 0;
    cb->head = cb->buffer;
    cb->tail = cb->buffer;
}

void cb_free(struct circ_buf *cb)
{
    kfree(cb->buffer);
}

void cb_push_back(struct circ_buf *cb, const char *item, size_t len)
{
    if(cb->count + len > cb->capacity)
    {
        /* The buffer is full, drop the frame. */
        printk(KERN_DEBUG "vswitch: Dropping frame: No space left in buffer\n");
        return;
    }
    if (cb->head + len <= cb->buffer_end)
    {
        /* Not crossing the buffer edge. */
        printk(KERN_DEBUG "vswitch: Write bytes to buffer, no wrap");
        memcpy(cb->head, item, len);
        cb->head += len;
    }
    else
    {
        printk(KERN_DEBUG "vswitch: Write bytes to buffer, wrap");
        memcpy(cb->head, item, cb->buffer_end - cb->head);
        memcpy(cb->buffer, &item[cb->buffer_end - cb->head], len - (cb->buffer_end - cb->head));
        cb->head = cb->buffer + len - (cb->buffer_end - cb->head);
    }

    if(cb->head == cb->buffer_end)
        cb->head = cb->buffer;
    cb->count += len;
}

void cb_pop_front(struct circ_buf *cb, char *item, size_t len)
{
    if(cb->count < len)
    {
        printk(KERN_DEBUG "vswitch: Nothing to read\n");
        return;
    }

    if (cb->tail + len <= cb->buffer_end)
    {
        /* Not crossing the buffer edge. */
        printk(KERN_DEBUG "vswitch: Read bytes from buffer, no wrap");
        memcpy(item, cb->tail, len);
        cb->tail += len;
    }
    else
    {
        printk(KERN_DEBUG "vswitch: Read bytes from buffer, wrap");
        memcpy(item, cb->tail, cb->buffer_end - cb->tail);
        memcpy(&item[cb->buffer_end - cb->tail], cb->buffer, len - (cb->buffer_end - cb->tail));
        cb->tail = cb->buffer + len - (cb->buffer_end - cb->tail);
    }

    if(cb->tail == cb->buffer_end)
        cb->tail = cb->buffer;
    cb->count -= len;
}

void clear_port_buffer(struct vswitch_port *p)
{
    down(&(p->sem));
    cb_clear(&(p->buf));
    up(&(p->sem));
}

void write_to_port(struct vswitch_port *p, char *buf, size_t len)
{
    down(&(p->sem));
    cb_push_back(&(p->buf), buf, len);
    up(&(p->sem));
    wake_up_interruptible(&p->queue);
}

static int __init vswitch_init(void)
{
    int i, j;
    struct device* device;
    static char device_name[32];

    printk(KERN_DEBUG "vswitch: Initializing virtual switch with %d ports\n", ports);
    major_number = register_chrdev(0, DEVICE_NAME, &fops);

    if (major_number < 0)
    {
        printk(KERN_ALERT "vswitch: Failed to register a major number\n");
        return major_number;
    }
    vswitch_class = class_create(THIS_MODULE, CLASS_NAME);
    if (IS_ERR(vswitch_class))
    {
        unregister_chrdev(major_number, DEVICE_NAME);
        printk(KERN_ALERT "vswitch: Failed to register device class\n");
    }

    /* Register devices for all of the switch ports. */
    interfaces = kmalloc(sizeof(struct vswitch_port) *ports, GFP_KERNEL);

    for (i = 0; i < ports; ++i)
    {
        snprintf(device_name, 32, "%s%d", DEVICE_NAME, i);
        printk(KERN_DEBUG "vswitch: Creating device /dev/%s\n", device_name);

        device = device_create(vswitch_class, NULL, MKDEV(major_number, i),
                               NULL, device_name);

        interfaces[i] = (struct vswitch_port){.num = i, .connected = 0,
                                              .dev = device};

        /* Initialize the output buffer for the port. */
        cb_init(&(interfaces[i].buf), buffer_size);
        sema_init(&(interfaces[i].sem), 1);
        init_waitqueue_head(&(interfaces[i].queue));

        if (IS_ERR(device))
        {
            /* Revert already created devices. */
            for (j = 0; j < i; ++j)
            {
                device_destroy(vswitch_class, MKDEV(major_number, j));
                cb_free(&(interfaces[i].buf));
            }

            class_destroy(vswitch_class);
            unregister_chrdev(major_number, DEVICE_NAME);
            printk(KERN_ALERT "vswitch: Failed to create device\n");
            return PTR_ERR(device);
        }

    }
    printk(KERN_DEBUG "vswitch: Virtual switch running\n");
    return 0;
}

static void __exit vswitch_exit(void)
{
    int i;

    printk(KERN_DEBUG "vswitch: Stopping virtual switch\n");

    for (i = 0; i < ports; ++i)
    {
        device_destroy(vswitch_class, MKDEV(major_number, i));
        cb_free(&(interfaces[i].buf));
    }

    class_unregister(vswitch_class);
    class_destroy(vswitch_class);
    unregister_chrdev(major_number, DEVICE_NAME);

    kfree(interfaces);
    printk(KERN_DEBUG "vswitch: Virtual switch stopped\n");
}

static int dev_open(struct inode *inodep, struct file *filep)
{
    unsigned int minor_number = MINOR(inodep->i_rdev);

    if (interfaces[minor_number].connected)
    {
        return -EACCES;
    }
    interfaces[minor_number].connected = 1;
    interfaces[minor_number].valid_until = 0;


    /* Clear the buffer in case the device that was previously connected to
       this port did not consume it. */
    printk(KERN_DEBUG "vswitch: Port %d connected\n", minor_number);
    return 0;
}

static int dev_release(struct inode *inodep, struct file *filep)
{
    unsigned int minor_number = MINOR(inodep->i_rdev);
    interfaces[minor_number].connected = 0;
    printk(KERN_DEBUG "vswitch: Port %d disconnected\n", minor_number);
    return 0;
}

static ssize_t dev_read(struct file *filep, char *buffer,
                        size_t len, loff_t *offset)
{
    char tmp_buf[mtu];
    unsigned int minor_number = MINOR(filep->f_inode->i_rdev);
    
    /* Reading does not need to be protected with a semaphore, circular buffer
       with only one consumer is thread safe (in reads). */
    if (wait_event_interruptible(interfaces[minor_number].queue,
                                 (interfaces[minor_number].buf.count >= len)))
        return -ERESTARTSYS;
    
    cb_pop_front(&(interfaces[minor_number].buf), tmp_buf, len);

    if (copy_to_user(buffer, tmp_buf, len))
        return -EFAULT;
    return len;
}

static ssize_t dev_write(struct file *filep, const char *buffer,
                         size_t len, loff_t *offset)
{
    unsigned int *src, *dst, *payload_len;
    unsigned int minor_number = MINOR(filep->f_inode->i_rdev);
    struct timespec ts;
    char tmp_buf[mtu];
    char *payload;
    int i;

    if (len > mtu)
        return -EINVAL;

    if (copy_from_user(tmp_buf, buffer, mtu) != 0)
        return -EFAULT;

    src = (unsigned int *)&tmp_buf[0];

    /* Update id table. */
    interfaces[minor_number].host = *src;
    getnstimeofday(&ts);
    interfaces[minor_number].valid_until = ts.tv_nsec + cache_ttl;

    dst = (unsigned int *)&tmp_buf[4];
    payload_len = (unsigned int *)&tmp_buf[8];
    payload = &tmp_buf[16];

    /* Ensure payload length is valid. */
    if (*payload_len > mtu - 16)
        return -EINVAL;

    printk(KERN_DEBUG "vswitch: %d bytes from %d to %d\n", *payload_len, *src, *dst);


    for (i = 0; i < ports; ++i)
    {
        if (i != minor_number && interfaces[i].host == *dst
            && interfaces[i].connected && ts.tv_nsec < interfaces[i].valid_until)
        {
            printk(KERN_DEBUG "vswitch: Dst found from cache\n");
            write_to_port(&(interfaces[i]), tmp_buf, *payload_len + 16);
            return len;
        }
    }

    /* No cache entry found, broadcast. */
    printk(KERN_DEBUG "vswitch: No cache entry found, broadcasting\n");
    for (i = 0; i < ports; ++i)
        if (i != minor_number && interfaces[i].connected)
            write_to_port(&(interfaces[i]), tmp_buf, *payload_len + 16);
    return len;
}

module_init(vswitch_init);
module_exit(vswitch_exit);
