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
#include "hvc_p128.h"

#define DRIVER_NAME		"hvcs-p128"
#define BASE_MINOR		0

#define P128_STS_DATA_READY	(1UL)
#define P128_STS_TX_EMPTY	(1UL << 1)

static int op_open(struct inode *inode, struct file *file);
static int op_release(struct inode *inode, struct file *file);
static ssize_t op_read(struct file *file, char *buff, size_t count, loff_t *pos);
static ssize_t op_write(struct file *file, const char *buff, size_t count, loff_t *pos);
//static long op_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

struct p128_device {
	struct device		*dev;
	unsigned int		busy;
	u32			id;
	u16			ifno;
	u16			irq;
	struct semaphore	rsem;
	struct semaphore	wsem;
};

struct p128 {
	struct list_head	list;

	dev_t			major;
	dev_t			minor;
	struct cdev		cdev;
	struct class		*class;

	const char	*name;
	u32		id;

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

static irqreturn_t irq_handler(int irq, void *dev_id)
{
	int ret;
	u32 event;
	struct p128_device *dev;

	dev = dev_id;
	ret = hvc_p128_get_event(dev->id, dev->ifno, &event);
	if (ret != 0) {
		pr_err("hvc_p128_get_event(id=%08x, ifno=%u) -> %d\n", dev->id, dev->ifno, ret);
		return IRQ_NONE;
	}

	if (event & P128_STS_DATA_READY) {
		up(&(dev->rsem));
	}

	if (event & P128_STS_TX_EMPTY) {
		up(&(dev->wsem));
	}

	return IRQ_HANDLED;
}

static ssize_t op_read(struct file *file, char *buff, size_t count, loff_t *pos)
{
	ssize_t ret;
	struct p128_device *dev;
	uint64_t tmp[16];

	dev = file->private_data;

	ret = down_interruptible(&(dev->rsem));
	if (ret != 0) {
		return ret;
	}

	if (count >= 128) {
		ret = hvc_p128_receive(dev->id, dev->ifno, buff);
		if (ret == 0) {
			ret = 128;
		} else {
			ret = 0;
		}
	} else {
		ret = hvc_p128_receive(dev->id, dev->ifno, tmp);
		if (ret == 0) {
			memcpy(buff, tmp, count);
			ret = count;
		} else {
			ret = 0;
		}
	}

	return ret;
}

static ssize_t op_write(struct file *file, const char *buff, size_t count, loff_t *pos)
{
	ssize_t ret;
	struct p128_device *dev;
	uint64_t tmp[16];

	dev = file->private_data;

	ret = down_interruptible(&(dev->wsem));
	if (ret != 0) {
		return ret;
	}

	if (count >= 128) {
		ret = hvc_p128_send(dev->id, dev->ifno, buff);
		if (ret != 0) {
			ret = -ENODEV;
		} else {
			ret = 128;
		}
	} else {
		memset(tmp, 0, sizeof(tmp));
		memcpy(tmp, buff, count);
		ret = hvc_p128_send(dev->id, dev->ifno, tmp);
		if (ret != 0) {
			ret = -ENODEV;
		} else {
			ret = count;
		}
	} 

	return ret;
}

static int op_open(struct inode *inode, struct file *file)
{
	int ret;
	struct p128 *p128;
	struct p128_device *dev;

	p128 = find_p128(inode->i_rdev);
	if (p128) {
		dev = p128->devices + (MINOR(inode->i_rdev) - p128->minor);
		file->private_data = dev;
		ret = 0;
	} else {
		ret = -ENODEV;
	}

	return ret;
}

static int op_release(struct inode *inode, struct file *file)
{
	return 0;
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
	int ret;
	int v;
	u32 event;
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

	ret = hvc_p128_get_status(p128->id, ifno, &status);
	if (ret != 0) {
		pr_err("hvc_p128_get_status(%s%u) -> %d.\n", p128->name, ifno, ret);
		return ret;
	}
	pr_debug("%s%u.status=0x%08x\n", p128->name, ifno, status);

	ret = hvc_p128_get_event(p128->id, ifno, &event);
	if (ret != 0) {
		pr_err("hvc_p128_get_event(%s%u) -> %d.\n", p128->name, ifno, ret);
		return ret;
	}
	pr_debug("%s%u.event=0x%08x\n", p128->name, ifno, event);
	status |= event;

	v = (status & P128_STS_DATA_READY) ? 1 : 0;
	sema_init(&(dev->rsem), v);

	v = (status & P128_STS_TX_EMPTY) ? 1 : 0;
	sema_init(&(dev->wsem), v);

    return 0;
}

static int register_device(struct platform_device *pdev, struct p128 *p128, int ifno)
{
	int ret;
	dev_t devt;
	struct p128_device *dev;

	ret = initialize_device(pdev, p128, ifno);
	if (ret != 0) {
		return ret;
	}

	devt = MKDEV(p128->major, (p128->minor + ifno));
	dev = &(p128->devices[ifno]);
	dev->dev = device_create(p128->class, &(pdev->dev), devt, dev, "%s%d", p128->name, ifno);
	if (IS_ERR(dev->dev)) {
		pr_err("unable to create device %s%d\n", p128->name, ifno);
		ret = PTR_ERR(dev->dev);
		return ret;
	}

	ret = devm_request_irq(dev->dev, dev->irq, irq_handler, IRQF_TRIGGER_RISING, p128->name, dev);
	if (ret != 0) {
		pr_err("devm_request_irq(irq=%d) -> %d\n", dev->irq, ret);
		return ret;
	}

	dev->busy = 1;

	pr_debug("%s%d is registered.\n", p128->name, ifno);

	return 0;
}

static int register_devices(struct platform_device *pdev, struct p128 *p128)
{
	int ret;
	int i;

	for (i = 0; i < p128->nr_devices; ++i) {
		ret = register_device(pdev, p128, i);
		if (ret != 0) {
			unregister_devices(pdev, p128);
			break;
		}
	}

	return ret;
}

static struct p128 *create_resources(struct platform_device *pdev, const char *name, u32 id, u16 nr_ifs)
{
	int ret;
	dev_t devt;
	struct p128 *p128;

	/* get the parameter from the hypervisor */

	p128 = kzalloc(sizeof(*p128), GFP_KERNEL);
	if (p128 == NULL) {
		return ERR_PTR(-ENOMEM);
	}

	p128->devices = kcalloc(nr_ifs, sizeof(struct p128_device), GFP_KERNEL);
	if (p128->devices == NULL) {
		ret = -ENOMEM;
		goto error0;
	}

	p128->name = name;
	p128->id = id;
	p128->nr_devices = nr_ifs;

	ret = alloc_chrdev_region(&devt, BASE_MINOR, nr_ifs, name);
	if (ret != 0) {
		pr_err("alloc_chrdev_region() -> %d\n", ret);
		goto error1;
	}

	p128->major = MAJOR(devt);
	p128->minor = MINOR(devt);

	p128->class = class_create(THIS_MODULE, name);
	if (IS_ERR(p128->class)) {
		ret = PTR_ERR(p128->class);
		pr_err("class_create() -> %d.\n", ret);
		goto error2;
	}

	cdev_init(&(p128->cdev), &file_ops);
	p128->cdev.owner = THIS_MODULE;
	ret = cdev_add(&(p128->cdev), devt, nr_ifs);
	if (ret != 0) {
		pr_err("cdev_add() -> %d\n", ret);
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

	return ERR_PTR(ret);
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
	int ret;
	u16 nr_ifs;
	struct p128 *p128;

	ret = hvc_p128_nr_interfaces(id, &nr_ifs);
	if (ret != 0) {
		pr_err("hvcs_p128_nr_interfaces(%s<0x%08x>) -> %d\n", name, id, ret);
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

	ret = register_devices(pdev, p128);
	if (ret != 0) {
		free_resources(p128);
		return ret;
	}

	list_add_tail(&(p128->list), &p128_list);
	pr_info("add %p to the list.\n", p128);

	return ret;
}

static int p128_probe(struct platform_device *pdev)
{
	int ret;
	u32 id;
	const char *name;
	struct fwnode_handle *fh;

	pr_info("pdev=%p\n", pdev);

	fh = dev_fwnode(&(pdev->dev));
	ret = fwnode_property_read_string(fh, "name", &name);
	if (ret != 0) {
		pr_err("property \"name\" is not available.\n");
		return ret;
	}

	ret = fwnode_property_read_u32_array(fh, "device-id", &id, 1);
	if (ret != 0) {
		pr_err("property \"device-id\" is not available.\n");
		return ret;
	}

	ret = probe(pdev, name, id);

	return ret;
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

