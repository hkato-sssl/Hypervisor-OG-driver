#define pr_fmt(fmt) "%s: " fmt, KBUILD_MODNAME

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/property.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/poll.h>
#include "hvc_p128.h"

#define DRIVER_NAME		"hvcs-p128"
#define BASE_MINOR		0

#define P128_STS_DATA_READY	(1UL)
#define P128_STS_TX_EMPTY	(1UL << 1)

static int op_open(struct inode *inode, struct file *file);
static int op_release(struct inode *inode, struct file *file);
static ssize_t op_read(struct file *file, char *buff, size_t count, loff_t *pos);
static ssize_t op_write(struct file *file, const char *buff, size_t count, loff_t *pos);
static __poll_t op_poll(struct file *filp, poll_table *wait);
//static long op_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

struct p128_device {
	struct device		*dev;
	unsigned int		busy;
	u32			id;
	u16			ifno;
	u16			irq;
	struct {
		char		data_ready;
		char		tx_empty;
	} status;

	struct mutex		rmux;
	struct mutex		wmux;
	struct wait_queue_head	rwtq;
	struct wait_queue_head	wwtq;
	spinlock_t		rlock;
	spinlock_t		wlock;
};

struct p128 {
	struct list_head	list;

	dev_t			major;
	dev_t			minor;
	struct cdev		cdev;
	struct class		*class;

	const char		*name;
	u32			id;

	u16			nr_devices;
	struct p128_device	*devices;
};

static LIST_HEAD(p128_list);

static const struct of_device_id of_p128[] = {
	{
		.compatible = "sssl,hvcs-p128",
	},
	{}
};
MODULE_DEVICE_TABLE(of, of_p128);

static struct file_operations file_ops = {
	.owner = THIS_MODULE,
	.open = op_open,
	.release = op_release,
	.read = op_read,
	.write = op_write,
	.poll = op_poll,
};

static struct p128 *find_p128(dev_t dev)
{
	unsigned int major;
	unsigned int minor;
	struct p128 *p128;

	major = MAJOR(dev);
	minor = MINOR(dev);
	list_for_each_entry(p128, &p128_list, list) {
		if ((major == p128->major) && (minor >= p128->minor) && (minor < (p128->minor + p128->nr_devices))) {
			return p128;
		}
	}

	return NULL;
}

static int op_open(struct inode *inode, struct file *file)
{
	int err;
	struct p128 *p128;
	struct p128_device *dev;

	p128 = find_p128(inode->i_rdev);
	if (p128) {
		dev = p128->devices + (MINOR(inode->i_rdev) - p128->minor);
		file->private_data = dev;
		err = 0;
	} else {
		err = -ENODEV;
	}

	return err;
}

static int op_release(struct inode *inode, struct file *file)
{
	return 0;
}

static irqreturn_t irq_handler(int irq, void *dev_id)
{
	int err;
	u32 status;
	struct p128_device *dev;

	dev = dev_id;
	err = hvc_p128_get_status(dev->id, dev->ifno, &status);
	if (err) {
		pr_err("hvc_p128_get_status(id=%08x, ifno=%u) -> %d\n", dev->id, dev->ifno, err);
		return IRQ_NONE;
	}

	if ((status & P128_STS_DATA_READY) && (! dev->status.data_ready)) {
		dev->status.data_ready = 1;
		wmb();
		wake_up(&(dev->rwtq));
	}

	if ((status & P128_STS_TX_EMPTY) && (! dev->status.tx_empty)) {
		dev->status.tx_empty = 1;
		wmb();
		wake_up(&(dev->wwtq));
	}

	return IRQ_HANDLED;
}

static ssize_t op_read(struct file *file, char *buff, size_t count, loff_t *pos)
{
	ssize_t err;
	unsigned long flags;
	uint64_t tmp[16];
	struct p128_device *dev;

	dev = file->private_data;
	err = mutex_lock_interruptible(&(dev->rmux));
	if (err)
		return err;

	err = wait_event_interruptible(dev->rwtq, dev->status.data_ready);
	if (err)
		goto read_exit;

	spin_lock_irqsave(&(dev->rlock), flags);

	if (count >= 128) {
		err = hvc_p128_receive(dev->id, dev->ifno, buff);
		if (! err) {
			dev->status.data_ready = 0;
			wmb();
			err = 128;
		} else {
			pr_err("hvc_p128_receive(id=0x%08x, ifno=%u, buff=%p) -> %ld\n", dev->id, dev->ifno, buff, err);
			err = -ENODEV;
		}
	} else {
		err = hvc_p128_receive(dev->id, dev->ifno, tmp);
		if (! err) {
			memcpy(buff, tmp, count);
			dev->status.data_ready = 0;
			wmb();
			err = count;
		} else {
			pr_err("hvc_p128_receive(id=0x%08x, ifno=%u, buff=%p) -> %ld\n", dev->id, dev->ifno, tmp, err);
			err = -ENODEV;
		}
	}

	spin_unlock_irqrestore(&(dev->rlock), flags);

read_exit:
	mutex_unlock(&(dev->rmux));

	return err;
}

static ssize_t op_write(struct file *file, const char *buff, size_t count, loff_t *pos)
{
	ssize_t err;
	unsigned long flags;
	struct p128_device *dev;
	uint64_t tmp[16];

	dev = file->private_data;
	err = mutex_lock_interruptible(&(dev->wmux));
	if (err) 
		return err;

	err = wait_event_interruptible(dev->wwtq, dev->status.tx_empty);
	if (err)
		goto write_exit;

	spin_lock_irqsave(&(dev->wlock), flags);

	if (count >= 128) {
		err = hvc_p128_send(dev->id, dev->ifno, buff);
		if (! err) {
			dev->status.tx_empty = 0;
			wmb();
			err = 128;
		} else {
			pr_err("hvc_p128_send(id=0x%08x, ifno=%u, buff=%p) -> %ld\n", dev->id, dev->ifno, buff, err);
			err = -ENODEV;
		}
	} else {
		memset(tmp, 0, sizeof(tmp));
		memcpy(tmp, buff, count);
		err = hvc_p128_send(dev->id, dev->ifno, tmp);
		if (! err) {
			dev->status.tx_empty = 0;
			wmb();
			err = count;
		} else {
			pr_err("hvc_p128_send(id=0x%08x, ifno=%u, buff=%p) -> %ld\n", dev->id, dev->ifno, buff, err);
			err = -ENODEV;
		}
	} 

	spin_unlock_irqrestore(&(dev->wlock), flags);

write_exit:
	mutex_unlock(&(dev->wmux));

	return err;
}

static __poll_t op_poll(struct file *filp, poll_table *wait)
{
	__poll_t flags;
	struct p128_device *dev;

	dev = filp->private_data;
	poll_wait(filp, &(dev->rwtq), wait);
	poll_wait(filp, &(dev->wwtq), wait);

	flags = 0;
	if (dev->status.data_ready)
		flags |= EPOLLIN | EPOLLRDNORM;
	if (dev->status.tx_empty)
		flags |= EPOLLOUT | EPOLLWRNORM;

	return flags;
}

static void unregister_device(struct platform_device *pdev, struct p128 *p128, int ifno)
{
	dev_t devt;
	struct p128_device *dev;

	dev = &(p128->devices[ifno]);
	if (dev->busy) {
		devt = MKDEV(p128->major, (p128->minor + ifno));
		device_destroy(p128->class, devt);
		dev->busy = 0;
	}
}

static void unregister_devices(struct platform_device *pdev, struct p128 *p128)
{
	int i;

	for (i = 0; i < p128->nr_devices; ++i) {
		unregister_device(pdev, p128, i);
	}
}

static int initialize_device(struct platform_device *pdev, struct p128 *p128, int ifno)
{
	int err;
	u32 status;
	struct p128_device *dev;

	dev = &(p128->devices[ifno]);
	if (dev->busy) {
		pr_err("%s%u is busy\n", p128->name, ifno);
		return -EBUSY;
	}

	dev->id = p128->id;
	dev->ifno = ifno;
	dev->irq = platform_get_irq(pdev, ifno);
	pr_debug("%s%u.irq=%u\n", p128->name, ifno, dev->irq);

	err = hvc_p128_get_status(p128->id, ifno, &status);
	if (err) {
		pr_err("hvc_p128_get_status(%s%u) -> %d.\n", p128->name, ifno, err);
		return err;
	}
	pr_debug("%s%u.status=0x%08x\n", p128->name, ifno, status);

	dev->status.data_ready = (status & P128_STS_DATA_READY) ? 1 : 0;
	dev->status.tx_empty = (status & P128_STS_TX_EMPTY) ? 1 : 0;

	mutex_init(&(dev->rmux));
	mutex_init(&(dev->wmux));

	init_waitqueue_head(&(dev->rwtq));
	init_waitqueue_head(&(dev->wwtq));

	spin_lock_init(&(dev->rlock));
	spin_lock_init(&(dev->wlock));

	return 0;
}

static int register_device(struct platform_device *pdev, struct p128 *p128, int ifno)
{
	int err;
	dev_t devt;
	struct p128_device *dev;

	err = initialize_device(pdev, p128, ifno);
	if (err)
		return err;

	devt = MKDEV(p128->major, (p128->minor + ifno));
	dev = &(p128->devices[ifno]);
	dev->dev = device_create(p128->class, &(pdev->dev), devt, dev, "%s%d", p128->name, ifno);
	if (IS_ERR(dev->dev)) {
		pr_err("unable to create device %s%d\n", p128->name, ifno);
		err = PTR_ERR(dev->dev);
		return err;
	}

	err = devm_request_irq(dev->dev, dev->irq, irq_handler, IRQF_TRIGGER_RISING, p128->name, dev);
	if (err) {
		pr_err("devm_request_irq(irq=%d) -> %d\n", dev->irq, err);
		return err;
	}

	dev->busy = 1;

	pr_debug("%s%d is registered.\n", p128->name, ifno);

	return 0;
}

static int register_devices(struct platform_device *pdev, struct p128 *p128)
{
	int err;
	int i;

	for (i = 0; i < p128->nr_devices; ++i) {
		err = register_device(pdev, p128, i);
		if (err) {
			unregister_devices(pdev, p128);
			break;
		}
	}

	return err;
}

static struct p128 *create_resources(struct platform_device *pdev, const char *name, u32 id, u16 nr_ifs)
{
	int err;
	dev_t devt;
	struct p128 *p128;

	/* get the parameter from the hypervisor */

	p128 = kzalloc(sizeof(*p128), GFP_KERNEL);
	if (p128 == NULL) {
		return ERR_PTR(-ENOMEM);
	}

	p128->devices = kcalloc(nr_ifs, sizeof(struct p128_device), GFP_KERNEL);
	if (p128->devices == NULL) {
		err = -ENOMEM;
		goto error0;
	}

	p128->name = name;
	p128->id = id;
	p128->nr_devices = nr_ifs;

	err = alloc_chrdev_region(&devt, BASE_MINOR, nr_ifs, name);
	if (err) {
		pr_err("alloc_chrdev_region() -> %d\n", err);
		goto error1;
	}

	p128->major = MAJOR(devt);
	p128->minor = MINOR(devt);

	p128->class = class_create(THIS_MODULE, name);
	if (IS_ERR(p128->class)) {
		err = PTR_ERR(p128->class);
		pr_err("class_create() -> %d.\n", err);
		goto error2;
	}

	cdev_init(&(p128->cdev), &file_ops);
	p128->cdev.owner = THIS_MODULE;
	err = cdev_add(&(p128->cdev), devt, nr_ifs);
	if (err) {
		pr_err("cdev_add() -> %d\n", err);
		goto error3;
	}

	return p128;

error3:
	class_destroy(p128->class);
error2:
	unregister_chrdev_region(devt, nr_ifs);
error1:
	kfree(p128->devices);
error0:
	kfree(p128);

	return ERR_PTR(err);
}

static int free_resources(struct p128 *p128)
{
	dev_t devt;

	class_destroy(p128->class);
	devt = MKDEV(p128->major, p128->minor);
	unregister_chrdev_region(devt, p128->nr_devices);

	kfree(p128->devices);
	kfree(p128);

	return 0;
}

static int probe(struct platform_device *pdev, const char *name, u32 id)
{
	int err;
	u16 nr_ifs;
	struct p128 *p128;

	err = hvc_p128_nr_interfaces(id, &nr_ifs);
	if (err) {
		pr_err("hvcs_p128_nr_interfaces(%s<0x%08x>) -> %d\n", name, id, err);
		return -ENODEV;		/* override the error code */
	}

	pr_info("%s: # of I/F = %d.\n", name, nr_ifs);
	if (nr_ifs == 0) {
		return -ENODEV;
	}

	p128 = create_resources(pdev, name, id, nr_ifs);
	if (IS_ERR(p128)) {
		return PTR_ERR(p128);
	}

	err = register_devices(pdev, p128);
	if (err) {
		free_resources(p128);
		return err;
	}

	list_add_tail(&(p128->list), &p128_list);
	pr_info("add %p to the list.\n", p128);

	return 0;
}

static int p128_probe(struct platform_device *pdev)
{
	int err;
	u32 id;
	const char *name;
	struct fwnode_handle *fh;

	pr_info("pdev=%p\n", pdev);

	fh = dev_fwnode(&(pdev->dev));
	err = fwnode_property_read_string(fh, "name", &name);
	if (err) {
		pr_err("property \"name\" is not available.\n");
		return err;
	}

	err = fwnode_property_read_u32_array(fh, "device-id", &id, 1);
	if (err) {
		pr_err("property \"device-id\" is not available.\n");
		return err;
	}

	err = probe(pdev, name, id);

	return err;
}

static int p128_remove(struct platform_device *pdev)
{
	struct p128 *p128;

	while (! list_empty(&p128_list)) {
		p128 = list_first_entry(&p128_list, struct p128, list);
		unregister_devices(pdev, p128);
		free_resources(p128);
		list_del(&(p128->list));
	}

	return 0;
}

static struct platform_driver p128_driver = {
	.probe = p128_probe,
	.remove = p128_remove,
	.driver = {
		.name = DRIVER_NAME,
		.of_match_table = of_p128,
	},
};
module_platform_driver(p128_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Hidekazu Kato");
MODULE_DESCRIPTION("p128 driver");

