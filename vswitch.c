/*
 * vswitch - Virtual network switch kernel module
 *
 * Author: Henrik Nyman <henrikjohannesnyman@gmail.com>
 * Copyright: (c) 2017 Henrik Nyman
 * License GPLv3
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Linux kernel module implementing a simple virtual network switch.
 */
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

/* Default parameter values */
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


/* Kernel module parameters */
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


/**
 * Circular buffer implementation.
 *
 * vswitch contains a simple implementation of a circular buffer. Each of the
 * ports have their own buffer for outgoing data. No buffer for incoming data
 * is used, so if there is no space for the frame in output buffer at the time
 * of incoming frame, the frame is discarded.
 * The circular buffer works with variable-lenght items, so no space gets wasted
 * if the packets are smaller than MTU. Packets larger than MTU get dropped.
 *
 */  
struct circ_buf
{
    char *buffer;      /* data buffer */
    char *buffer_end;  /* end of data buffer */
    size_t capacity;   /* maximum number of bytes in the buffer */
    size_t count;      /* number of unread bytes in the buffer */
    char *head;        /* pointer to head */
    char *tail;        /* pointer to tail */
};

/**
 * cb_init() - initialize a new circular buffer
 * 
 * @capacity: amount of bytes to allocate for the buffer
 */
void cb_init(struct circ_buf *cb, size_t capacity)
{
    cb->buffer = kmalloc(capacity, GFP_KERNEL);
    cb->buffer_end = cb->buffer + capacity;
    cb->capacity = capacity;
    cb->count = 0;
    cb->head = cb->buffer;
    cb->tail = cb->buffer;
}

/**
 * cb_push_back() - add new item to the circular buffer
 *
 * @cb: a pointer to a circular buffer struct
 * @item: a pointer to a data buffer to insert into cb
 * @len: amount of bytes to append to the buffer
 *
 * Tries to append given data to the circular buffer. If there is no space left
 * in the buffer, data is silently discarded - no bytes get written. Data may
 * wrap over the edge of the buffer, then it will be written in two parts.
 */
void cb_push_back(struct circ_buf *cb, const char *item, size_t len)
{
    if (cb->count + len > cb->capacity)
    {
        /* The buffer is full, drop the frame. */
        printk(KERN_DEBUG "vswitch: Dropping frame: No space left in buffer\n");
        return;
    }
    if (cb->head + len <= cb->buffer_end)
    {
        /* Not crossing the buffer edge. */
        memcpy(cb->head, item, len);
        cb->head += len;
    }
    else
    {
        /* Crossing the buffer edge. */
        memcpy(cb->head, item, cb->buffer_end - cb->head);
        memcpy(cb->buffer, &item[cb->buffer_end - cb->head],
               len - (cb->buffer_end - cb->head));
        cb->head = cb->buffer + len - (cb->buffer_end - cb->head);
    }

    /* Exactly at the edge */
    if(cb->head == cb->buffer_end)
        cb->head = cb->buffer;
    cb->count += len;
}

/**
 * cb_pop_front() - get n bytes from the circular buffer
 *
 * @cb: a pointer to a circular buffer struct
 * @item: a pointer to a data buffer which data will be written into
 * @len: amount of bytes to read from the buffer
 *
 * Tries to read given amount of data from the circular buffer. If there is no
 * data available in the buffer, nothing is done. Data may wrap over the edge of
 * the buffer, then it will be read in two parts.
 */
void cb_pop_front(struct circ_buf *cb, char *item, size_t len)
{
    if (cb->count < len)
    {
        /* Nothing to read here. This should not happen since the reading
           thread is awoken only when enough data is available, and there can
           only be one reading thread. */
        return;
    }

    if (cb->tail + len <= cb->buffer_end)
    {
        /* Not crossing the buffer edge. */
        memcpy(item, cb->tail, len);
        cb->tail += len;
    }
    else
    {
        /* Crossing the buffer edge. */
        memcpy(item, cb->tail, cb->buffer_end - cb->tail);
        memcpy(&item[cb->buffer_end - cb->tail], cb->buffer,
               len - (cb->buffer_end - cb->tail));
        cb->tail = cb->buffer + len - (cb->buffer_end - cb->tail);
    }

    /* Exactly at the edge */
    if(cb->tail == cb->buffer_end)
        cb->tail = cb->buffer;
    cb->count -= len;
}

/**
 * Virtual switch port.
 *
 * Represents a virtual switch port with an ougoing buffer and a wait queue.
 * The port struct contains a single cache entry for the id of the connected
 * host.
 */
struct vswitch_port
{
    unsigned short num;      /* Number of the port */
    int connected;           /* 1 if there is a device connected, 0 if not */
    unsigned int host;       /* Cached ID of the device connected to the port */
    long valid_until;        /* Field host is valid until this timestamp */
    struct device *dev;      /* The device link */
    struct circ_buf buf;     /* Circular buffer for outgoing data */
    struct semaphore sem;    /* Semaphore for writing data into buffer */
    wait_queue_head_t queue; /* Wait queue for read operations */
};

/**
 * Structure containing all switch ports.
 */
static struct vswitch_port *interfaces;


/**
 * write_to_port() - Write buffer to switch port
 *
 * @p: address of a switch port
 * @buf: source buffer
 * @len: amount of bytes to copy
 *
 * Write `len` bytes from buffer `buf` into switch port `p`s output buffer. If
 * there is no space in the circular buffer, the frame is silently dropped. 
 */
void write_to_port(struct vswitch_port *p, char *buf, size_t len)
{
    down(&(p->sem));
    cb_push_back(&(p->buf), buf, len);
    up(&(p->sem));
    wake_up_interruptible(&p->queue);
}

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


/**
 * vswitch_init() - initialize virtual switch
 *
 * Allocates and initializes required data structures for the switch operation.
 * Sets up device files for all switch ports.
 */
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

    /* Allocate space for the switch structures */
    interfaces = kmalloc(sizeof(struct vswitch_port) *ports, GFP_KERNEL);

    /* Register devices for all of the switch ports. */
    for (i = 0; i < ports; ++i)
    {
        /* Append a running number to the device name */
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
                kfree(interfaces[i].buf.buffer);
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

/**
 * vswitch_init() - initialize virtual switch
 *
 * Allocates and initializes required data structures for the switch operation.
 * Sets up device files for all switch ports.
 */
static void __exit vswitch_exit(void)
{
    int i;

    printk(KERN_DEBUG "vswitch: Stopping virtual switch\n");

    for (i = 0; i < ports; ++i)
    {
        device_destroy(vswitch_class, MKDEV(major_number, i));
        kfree(interfaces[i].buf.buffer);
    }

    class_unregister(vswitch_class);
    class_destroy(vswitch_class);
    unregister_chrdev(major_number, DEVICE_NAME);

    kfree(interfaces);
    printk(KERN_DEBUG "vswitch: Virtual switch stopped\n");
}

/**
 * dev_open() - connect a cable to virtual switch
 *
 * @inodep: pointer to the inode of the device file
 * @filep: pointer to the device file
 *
 * Connect a virtual cable to the device file. If there is already a client
 * connected to this port, permission error is returned. Two cables cannot be
 * connected to the same port of a real switch, either.
 */
static int dev_open(struct inode *inodep, struct file *filep)
{
    unsigned int minor_number = MINOR(inodep->i_rdev);

    if (interfaces[minor_number].connected)
    {
        printk(KERN_DEBUG "vswitch: Port %d already in use\n", minor_number);
        return -EACCES;
    }

    /* Set the connection status to connected and clear the host cache. */
    interfaces[minor_number].connected = 1;
    interfaces[minor_number].valid_until = 0;

    printk(KERN_DEBUG "vswitch: Port %d connected\n", minor_number);
    return 0;
}

/**
 * dev_release() - disconnect cable from the virtual switch
 *
 * @inodep: pointer to the inode of the device file
 * @filep: pointer to the device file
 *
 * Remove the connection from this device file. 
 */
static int dev_release(struct inode *inodep, struct file *filep)
{
    unsigned int minor_number = MINOR(inodep->i_rdev);
    interfaces[minor_number].connected = 0;
    printk(KERN_DEBUG "vswitch: Port %d disconnected\n", minor_number);
    return 0;
}

/**
 * dev_read() - blocking read from the switch port
 *
 * @filep: pointer to the device file
 * @buffer: target buffer for the read
 * @len: amount of data requested
 * @offset: offset in the data buffer (not used)
 *
 * Copy data from the input buffer into the user-space buffer `buffer`. If no
 * data is available, block until some can be read. Reading can be done without
 * locks since there can be only one client connected to a device at any time.
 */
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

/**
 * dev_write() - write into the switch port
 *
 * @filep: pointer to the device file
 * @buffer: source buffer for the file
 * @len: amount of data to be written
 * @offset: offset in the data buffer (not used)
 *
 * Write data into the switch port. If the destination id is not found in the
 * cache, broadcast the message to all connected ports.
 * Data format:
 * Source ID: 4 byte signed integer (little-endian)
 * Destination ID: 4 byte signed integer (little-endian)
 * Payload size: 4 byte signed integer (little-endian)
 * Padding: 4 bytes of zero
 * Payload: N bytes of payload, N <= mtu - 16
 */
static ssize_t dev_write(struct file *filep, const char *buffer,
                         size_t len, loff_t *offset)
{
    unsigned int *src, *dst, *payload_len;
    unsigned int minor_number = MINOR(filep->f_inode->i_rdev);
    struct timespec ts;
    char tmp_buf[mtu];
    char *payload;
    int i;

    /* Do not allow writing multiple frames at once. */
    if (len > mtu)
        return -EINVAL;

    if (copy_from_user(tmp_buf, buffer, mtu) != 0)
        return -EFAULT;

    src = (unsigned int *)&tmp_buf[0];

    /* Update id cache with the sender id. */
    interfaces[minor_number].host = *src;
    getnstimeofday(&ts);
    interfaces[minor_number].valid_until = ts.tv_nsec + cache_ttl;

    /* TODO: May need to verify if bytes are aligned correctly. */
    dst = (unsigned int *)&tmp_buf[4];
    payload_len = (unsigned int *)&tmp_buf[8];
    payload = &tmp_buf[16];

    /* Ensure payload length is valid. */
    if (*payload_len > mtu - 16)
        return -EINVAL;

    for (i = 0; i < ports; ++i)
    {
        if (i != minor_number && interfaces[i].host == *dst &&
            interfaces[i].connected && ts.tv_nsec < interfaces[i].valid_until)
        {
            printk(KERN_DEBUG "vswitch: %d bytes from %d to %d\n",
                   *payload_len, *src, *dst);
            write_to_port(&(interfaces[i]), tmp_buf, *payload_len + 16);
            return len;
        }
    }

    /* No cache entry found, broadcast. */
    printk(KERN_DEBUG "vswitch: %d bytes from %d to %d -> broadcast\n",
           *payload_len, *src, *dst);
    for (i = 0; i < ports; ++i)
        if (i != minor_number && interfaces[i].connected)
            write_to_port(&(interfaces[i]), tmp_buf, *payload_len + 16);
    return len;
}

module_init(vswitch_init);
module_exit(vswitch_exit);
