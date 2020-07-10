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
	struct device	        *dev;
	unsigned int	        busy;
        u32                     status;
	u16		        ifno;
	u16		        interrupt_no;
};

struct p128 {
        struct list_head        list;

        dev_t                   major;
	struct cdev	        cdev;
	struct class		*class;

	struct {
		const char	*name;
		u32		command;
	} dtb;

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
        pr_info("private_data=%p\n", file->private_data);

	return 0;
}

static int op_release(struct inode *inode, struct file *file)
{
	pr_info("%s()\n", __func__);

	return 0;
}

static void unregister_device(struct platform_device *pdev, struct p128 *p128, int ifno)
{
        dev_t devt;
        struct p128_device *dev;

        dev = &(p128->devices[ifno]);
        if (dev->busy) {
                devt = MKDEV(p128->major, ifno);
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

static int register_device(struct platform_device *pdev, struct p128 *p128, int ifno)
{
	int ret;
        dev_t devt;
	struct p128_device *dev;

	dev = &(p128->devices[ifno]);
	if (dev->busy) {
		pr_err("%s%u is busy\n", p128->dtb.name, ifno);
		return -EBUSY;
	}

	ret = hvc_p128_get_interrupt_no(p128->dtb.command, ifno, &(dev->interrupt_no));
	if (ret) {
		pr_err("hvc_p128_get_interrupt_no(%s%u) -> %d.\n", p128->dtb.name, ifno, ret);
		return ret;
	}

        ret = hvc_p128_get_status(p128->dtb.command, ifno, &(dev->status));
	if (ret) {
		pr_err("hvc_p128_get_status(%s%u) -> %d.\n", p128->dtb.name, ifno, ret);
		return ret;
	}

        devt = MKDEV(p128->major, (BASE_MINOR + ifno));
	dev->dev = device_create(p128->class, &(pdev->dev), devt, dev, "%s%d", p128->dtb.name, ifno);
        pr_info("device_create(%s%u, %p)\n", p128->dtb.name, ifno, dev);
	if (IS_ERR(dev->dev)) {
		pr_err("unable to create device %s%d\n", p128->dtb.name, ifno);
		ret = PTR_ERR(dev->dev);
                return ret;
	}

	dev->busy = 1;
	pr_info("%s%d is added.\n", p128->dtb.name, ifno);

	return 0;
}

static int register_devices(struct platform_device *pdev, struct p128 *p128)
{
	int ret;
	int i;

	for (i = 0; i < p128->nr_devices; ++i) {
		ret = register_device(pdev, p128, i);
		if (ret) {
                        unregister_devices(pdev, p128);
                        break;
		}
	}

	return ret;
}

static struct p128 *create_resources(struct platform_device *pdev, const char *name, u32 command, u16 nr_ifs)
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

	p128->dtb.name = name;
	p128->dtb.command = command;
	p128->nr_devices = nr_ifs;

	ret = alloc_chrdev_region(&devt, BASE_MINOR, nr_ifs, name);
	if (ret != 0) {
		pr_err("alloc_chrdev_region() -> %d\n", ret);
		goto error1;
	}

        p128->major = MAJOR(devt);

	p128->class = class_create(THIS_MODULE, name);
	if (IS_ERR(p128->class)) {
		ret = PTR_ERR(p128->class);
		pr_err("class_create() -> %d.\n", ret);
		goto error2;
	}

	cdev_init(&(p128->cdev), &file_ops);
	p128->cdev.owner = THIS_MODULE;
        devt = MKDEV(p128->major, BASE_MINOR);
	ret = cdev_add(&(p128->cdev), devt, nr_ifs);
	if (ret) {
		pr_err("cdev_add() -> %d\n", ret);
		goto error3;
        }

	return p128;

error3:
        class_destroy(p128->class);
error2:
        devt = MKDEV(p128->major, BASE_MINOR);
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
	devt = MKDEV(p128->major, BASE_MINOR);
	unregister_chrdev_region(devt, p128->nr_devices);

	kfree(p128->devices);
	kfree(p128);

	return 0;
}

static int probe(struct platform_device *pdev, const char *name, u32 command)
{
	int ret;
        u16 nr_ifs;
	struct p128 *p128;

        ret = hvc_p128_nr_interfaces(command, &nr_ifs);
        if (ret) {
                pr_err("hvcs_p128_nr_interfaces(%s<0x%08x>) -> %d\n", name, command, ret);
                return -ENODEV;         /* override the error code */
        }

        pr_info("%s: # of I/F = %d.\n", name, nr_ifs);
        if (nr_ifs == 0) {
                return -ENODEV;
        }

        p128 = create_resources(pdev, name, command, nr_ifs);
        if (IS_ERR(p128)) {
                return PTR_ERR(p128);
        }

	ret = register_devices(pdev, p128);
	if (ret != 0) {
		free_resources(p128);
		return ret;
	}
        pr_info("@@@\n");

        list_add_tail(&(p128->list), &p128_list);
        pr_info("@@@\n");

	return ret;
}

static int p128_probe(struct platform_device *pdev)
{
	int ret;
        u32 command;
        const char *name;
	struct fwnode_handle *fh;

        pr_info("pdev=%p\n", pdev);

        fh = dev_fwnode(&(pdev->dev));
        ret = fwnode_property_read_string(fh, "name", &name);
        if (ret) {
		pr_err("property \"name\" is not available.\n");
                return ret;
        }

        ret = fwnode_property_read_u32_array(fh, "command", &command, 1);
        if (ret) {
		pr_err("property \"command\" is not available.\n");
                return ret;
        }

        ret = probe(pdev, name, command);

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
