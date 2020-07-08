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
#include "hvc_p128.h"

#define DRIVER_NAME	"hvcs-p128"
#define BASE_MINOR	0

static int op_open(struct inode *inode, struct file *file);
static int op_release(struct inode *inode, struct file *file);
static ssize_t op_read(struct file *file, char *buff, size_t count, loff_t *pos);
static ssize_t op_write(struct file *file, const char *buff, size_t count, loff_t *pos);
//static long op_ioctl(struct file *filp, unsigned int cmd, unsigned long arg);

struct p128_device {
	struct module	*owner;
	struct device	*dev;
	struct cdev	cdev;

	unsigned int	busy;
	u16		ifno;
	u16		interrupt_no;
};

struct p128 {
	dev_t			major;
	struct class		*class;
	struct {
		const char	*name;
		u32		reg;
	} dtb;
	u16			nr_devices;
	struct p128_device	*devices;
};

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

static ssize_t op_read(struct file *file, char *buff, size_t count, loff_t *pos)
{
	pr_info("%s()\n", __func__);

	return 0;
}

static ssize_t op_write(struct file *file, const char *buff, size_t count, loff_t *pos)
{
	pr_info("%s()\n", __func__);

	return count;
}

static int op_open(struct inode *inode, struct file *file)
{
	pr_info("%s()\n", __func__);

	return 0;
}

static int op_release(struct inode *inode, struct file *file)
{
	pr_info("%s()\n", __func__);

	return 0;
}

static int register_device(struct platform_device *pdev, struct p128 *p128, u16 ifno)
{
	int ret;
	struct p128_device *dev;
	dev_t devt;

	dev = &(p128->devices[ifno]);
	if (dev->busy) {
		pr_err("%s#%u is busy\n", p128->dtb.name, ifno);
		return -EBUSY;
	}

	ret = hvc_p128_get_interrupt_no(p128->dtb.reg, ifno, &(dev->interrupt_no));
	if (ret) {
		pr_err("hvc_p128_get_interrupt_no(%s%u) -> %d.\n", p128->dtb.name, ifno, ret);
		return ret;
	}

	cdev_init(&(dev->cdev), &file_ops);
	dev->cdev.owner = THIS_MODULE;
	devt = MKDEV(p128->major, ifno);
	ret = cdev_add(&(dev->cdev), devt, 1);
	if (ret) {
		pr_err("cdev_add() -> %d\n", ret);
		return ret;
	}

	dev->dev = device_create(p128->class, &(pdev->dev), devt, dev, "%s%d", p128->dtb.name, ifno);
	if (IS_ERR(dev->dev)) {
		pr_err("unable to create device %s%d\n", p128->dtb.name, ifno);
		ret = PTR_ERR(dev->dev);
		goto error1;
	}
	dev_set_drvdata(dev->dev, dev);

	dev->busy = 1;
	pr_info("%s%d is added\n", p128->dtb.name, ifno);

	return 0;

error1:
	cdev_del(&(dev->cdev));

	return ret;
}

static int register_devices(struct platform_device *pdev, struct p128 *p128)
{
	int ret;
	int i;

	for (i = 0; i < p128->nr_devices; ++i) {
		ret = register_device(pdev, p128, i);
		if (ret) {
			break;
		}
	}

	return ret;
}

static int unregister_device(struct p128 *p128, unsigned int ifno)
{
	struct p128_device *dev;

	if (ifno >= p128->nr_devices) {
		pr_err("Invalid I/F No.(%u)\n", ifno);
		return -EINVAL;
	}
	dev = &(p128->devices[ifno]);

	if (dev->busy == 0) {
		return 0;	/* no work */
	}

	dev->busy = 0;

	device_destroy(p128->class, dev->dev->devt);
	cdev_del(&(dev->cdev));
	dev->busy = 0;

	return 0;
}

static struct p128 *create_resources(const char *name, u32 reg)
{
	int ret;
	u16 nr_ifs;
	dev_t dev;
	struct p128 *p;

	/* get the parameter from the hypervisor */

	ret = hvc_p128_nr_interfaces(reg, &nr_ifs);
	pr_info("# of I/Fs: %d\n", nr_ifs);
	if (nr_ifs == 0) {
		return ERR_PTR(-ENODEV);
	}

	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (p == NULL) {
		return ERR_PTR(-ENOMEM);
	}

	p->devices = kcalloc(nr_ifs, sizeof(struct p128_device), GFP_KERNEL);
	if (p->devices == NULL) {
		ret = -ENOMEM;
		goto error0;
	}

	p->dtb.name = name;
	p->dtb.reg = reg;
	p->nr_devices = nr_ifs;

	ret = alloc_chrdev_region(&dev, BASE_MINOR, nr_ifs, name);
	if (ret != 0) {
		pr_err("alloc_chrdev_region() -> %d\n", ret);
		goto error0;
	}

	p->major = MAJOR(dev);

	p->class = class_create(THIS_MODULE, name);
	if (IS_ERR(p->class)) {
		ret = PTR_ERR(p->class);
		pr_err("class_create() -> %d.\n", ret);
		goto error1;
	}

	return p;

error1:
	dev = MKDEV(p->major, BASE_MINOR);
	unregister_chrdev_region(dev, nr_ifs);
	kfree(p->devices);
error0:
	kfree(p);

	return ERR_PTR(ret);
}

static int free_resources(struct p128 *p128)
{
	unsigned int i;
	dev_t dev;

	for (i = 0; i < p128->nr_devices; ++i) {
		unregister_device(p128, i);
	}

	class_destroy(p128->class);
	dev = MKDEV(p128->major, BASE_MINOR);
	unregister_chrdev_region(dev, p128->nr_devices);

	kfree(p128->devices);
	kfree(p128);

	return 0;
}

static int probe(struct platform_device *pdev)
{
	int ret;
	u32 reg;
	const char *name;
	struct device *dev;
	struct fwnode_handle *child;
	struct p128 *p128;

	ret = -ENODEV;
	dev = &(pdev->dev);
	device_for_each_child_node(dev, child) {
		if (fwnode_property_present(child, "reg")) {
			fwnode_property_read_u32(child, "reg", &reg);
			pr_info("reg=0x%08x\n", reg);
		} else {
			pr_err("property \"reg\" is not available.\n");
			ret = -EINVAL;
			break;
		}

		if (fwnode_property_present(child, "name")) {
			fwnode_property_read_string(child, "name", &name);
			pr_info("name=%s\n", name);
		} else {
			pr_err("property \"name\" is not available.\n");
			ret = -EINVAL;
			break;
		}

		p128 = create_resources(name, reg);
		if (IS_ERR(p128)) {
			ret = PTR_ERR(p128);
			break;
		}

		ret = register_devices(pdev, p128);
		if (ret != 0) {
			free_resources(p128);
			break;
		}

		platform_set_drvdata(pdev, p128);
	}

	return ret;
}

static int p128_probe(struct platform_device *pdev)
{
	int ret;

	ret = probe(pdev);

	return ret;
}

static int p128_remove(struct platform_device *pdev)
{
	free_resources(platform_get_drvdata(pdev));

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

