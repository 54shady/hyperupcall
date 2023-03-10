/*
 * Device driver for extensible paravirtualization QEMU device
 * 2020 Giacomo Pellicci
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

#include <linux/processor.h>
#include <linux/sched.h>
#include <asm/uaccess.h> /* put_user */
#include <asm/io.h>
#include <linux/cdev.h> /* cdev_ */
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kprobes.h>
#include <linux/bpf.h>
#include <linux/filter.h>

/* For socket etc */
#include <linux/net.h>
#include <linux/inet.h>
#include <net/sock.h>
#include <linux/tcp.h>
#include <linux/in.h>
#include <asm/uaccess.h>
#include <linux/file.h>
#include <linux/socket.h>
#include <linux/slab.h>

#include <net/sock.h>
#include <linux/netlink.h>
#include <linux/skbuff.h>

#include <asm/siginfo.h>    //siginfo
#include <linux/rcupdate.h> //rcu_read_lock
#include <linux/sched.h>    //find_task_by_pid_type

#include <linux/wait.h>		//linux wait_queue

#include "bpf_injection_msg.h"	


#define NEWDEV_REG_PCI_BAR      0
#define NEWDEV_BUF_PCI_BAR      1
#define DEV_NAME "newdev"
#define NEWDEV_DEVICE_ID 0x11ea
#define QEMU_VENDOR_ID 0x1234

#define NEWDEV_REG_STATUS_IRQ   0	//read
#define NEWDEV_REG_LOWER_IRQ    4	//write
#define NEWDEV_REG_RAISE_IRQ    8	//write (unused in this driver)
#define NEWDEV_REG_DOORBELL		8
#define NEWDEV_REG_SETAFFINITY	12

#define IOCTL_SCHED_SETAFFINITY 13
#define IOCTL_PROGRAM_INJECTION_RESULT_READY 14


MODULE_LICENSE("GPL");

static struct pci_device_id pci_ids[] = {
	{ PCI_DEVICE(QEMU_VENDOR_ID, NEWDEV_DEVICE_ID), },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, pci_ids);

static int payload_left;
static int flag;
static int pci_irq;
static int major;
static struct pci_dev *pdev;
static void __iomem *bufmmio;
static DECLARE_WAIT_QUEUE_HEAD(wq);		//wait queue static declaration


static ssize_t read(struct file *filp, char __user *buf, size_t len, loff_t *off)
{
	ssize_t ret;
	u32 kbuf;
	struct bpf_injection_msg_header myheader;

	// pr_info("READ SIZE=%ld, OFF=%lld", len, *off);
	// printk(KERN_INFO "Inside read\n");
	// printk(KERN_INFO "Scheduling Out\n");
	wait_event_interruptible(wq, flag >= 1);
	
	// printk(KERN_INFO "Woken Up flag:%d\n", flag);

	if (*off % 4 || len == 0) {
		// pr_info("read off=%lld or size=%ld error, NOT ALIGNED 4!\n", *off, len);
		ret = 0;
	} else {
		// pr_info("filp->fpos:\t%lld\n", filp->f_pos);
		kbuf = ioread32(bufmmio + *off);

		// pr_info("After ioread=>\tfilp->fpos:\t%lld\n", filp->f_pos);
		// pr_info("ioread32: %x\n", kbuf);

		if(flag == 2){
			memcpy(&myheader, &kbuf, sizeof(kbuf));
			pr_info("  Version:%u\n  Type:%u\n  Payload_len:%u\n", myheader.version, myheader.type, myheader.payload_len);
			payload_left = myheader.payload_len;// + 12;
			flag = 1;
		}
		else if(flag == 1){
			payload_left -= len;
			if(payload_left <= 0){
				// pr_info("flag reset to 0\n");
				flag = 0;
			}
		}

		if (copy_to_user(buf, (void *)&kbuf, 4)) {
			ret = -EFAULT;
		} else {
			ret = len;
			*off += 4;
		}
	}
	// pr_info("READ\n");



	return ret;
}

static ssize_t write(struct file *filp, const char __user *buf, size_t len, loff_t *off)
{
	ssize_t ret;
	u32 kbuf;
	pr_info("WRITE SIZE=%ld, OFF=%lld", len, *off);
	ret = len;
	if (!(*off % 4)) {
		if (copy_from_user((void *)&kbuf, buf, 4) ) { // || len != 4) {
			ret = -EFAULT;
		} else {
			iowrite32(kbuf, bufmmio + *off);			
			*off += 4;
		}
	}
	else{
		pr_info("write off=%lld or size=%ld error, NOT ALIGNED 4!\n", *off, len);
	}
	pr_info("WRITE\n");
	return ret;
}

static loff_t llseek(struct file *filp, loff_t off, int whence)
{
	loff_t newpos;

  	switch(whence) {
   		case 0: /* SEEK_SET */
		    newpos = off;
		    break;

   		case 1: /* SEEK_CUR */
		    newpos = filp->f_pos + off;
		    break;

   		default: /* can't happen */
    		return -EINVAL;
  }
  if (newpos<0){
  	return -EINVAL;
  }
  filp->f_pos = newpos;
  return newpos;
}

static long newdev_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {   
    switch (cmd) {
        case IOCTL_PROGRAM_INJECTION_RESULT_READY:
        	//bpf program result inserted in buffer area as bpf_injection_msg_t
        	pr_info("response is ready!!! [dev]\n");
        	iowrite32(1, bufmmio + NEWDEV_REG_DOORBELL);
        	//signal to device 
        	break;
        case IOCTL_SCHED_SETAFFINITY:
        {
        	// Retrieve the requested cpu from userspace
        	// Write in specific region in device to trigger the sched_setaffinity in host system
        	int requested_cpu;
        	if (copy_from_user(&requested_cpu, (int *)arg, sizeof(int))){
            	return -EACCES;
          	}
          	pr_info("IOCTL requested_cpu: %d\n", requested_cpu);
          	iowrite32(requested_cpu, bufmmio + NEWDEV_REG_SETAFFINITY);
        	break;
        }        	
	}
	return 0;
}

/* These fops are a bit daft since read and write interfaces don't map well to IO registers.
 * One ioctl per register would likely be the saner option. But we are lazy.
 * We use the fact that every IO is aligned to 4 bytes. Misaligned reads means EOF. */
static struct file_operations fops = {
	.owner   = THIS_MODULE,
	.llseek  = llseek,
	.read    = read,
	.write   = write,
	.unlocked_ioctl = newdev_ioctl,
};

static irqreturn_t irq_handler(int irq, void *dev){
	int devi;
	irqreturn_t ret;
	u32 irq_status;

	devi = *(int *)dev;
	if (devi == major) {
		irq_status = ioread32(bufmmio + NEWDEV_REG_STATUS_IRQ);

		//handle
		pr_info("interrupt irq = %d dev = %d irq_status = 0x%llx\n",
				irq, devi, (unsigned long long)irq_status);
		pr_info("me handling like a god?\n");

		switch(irq_status){
			case PROGRAM_INJECTION:
				pr_info("case PROGRAM_INJECTION irq handler\n");
				pr_info("waking up interruptible process...\n");
				flag = 2;
				wake_up_interruptible(&wq);
				break;
			case PROGRAM_INJECTION_AFFINITY:
				pr_info("case PROGRAM_INJECTION_AFFINITY irq handler\n");
				pr_info("waking up interruptible process...\n");
				flag = 2;
				wake_up_interruptible(&wq);
				break;
			case 22:		//init irq_handler (old raw data)
				pr_info("handling irq 22 for INIT\n");
				//init_handler();
				pr_info("waking up interruptible process...\n");
				flag = 1;
				wake_up_interruptible(&wq);
				break;
		}

		/* Must do this ACK, or else the interrupts just keeps firing. */
		iowrite32(irq_status, bufmmio + NEWDEV_REG_LOWER_IRQ);
		ret = IRQ_HANDLED;
	} else {
		ret = IRQ_NONE;
	}
	return ret;
}

/**
 * Called just after insmod if the hardware device is connected,
 * not called otherwise.
 *
 * 0: all good
 * 1: failed
 */
static int pci_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	u8 val;

	pr_info("pci_probe\n");
	major = register_chrdev(0, DEV_NAME, &fops);
	pdev = dev;
	if (pci_enable_device(dev) < 0) {
		dev_err(&(pdev->dev), "pci_enable_device\n");
		goto error;
	}
	// if (pci_request_region(dev, NEWDEV_REG_PCI_BAR, "myregion0")) {
	// 	dev_err(&(pdev->dev), "pci_request_region0\n");
	// 	goto error;
	// }
	// io = pci_iomap(pdev, NEWDEV_REG_PCI_BAR, pci_resource_len(pdev, NEWDEV_REG_PCI_BAR));

	if (pci_request_region(dev, NEWDEV_BUF_PCI_BAR, "myregion1")) {
		dev_err(&(pdev->dev), "pci_request_region1\n");
		goto error;
	}
	bufmmio = pci_iomap(pdev, NEWDEV_BUF_PCI_BAR, pci_resource_len(pdev, NEWDEV_BUF_PCI_BAR));
	

	/* IRQ setup. */
	pci_read_config_byte(dev, PCI_INTERRUPT_LINE, &val);
	pci_irq = val;
	if (request_irq(pci_irq, irq_handler, IRQF_SHARED, "pci_irq_handler0", &major) < 0) {
		dev_err(&(dev->dev), "request_irq\n");
		goto error;
	}

	flag = 0;
	pr_info("pci_probe COMPLETED SUCCESSFULLY\n");

	return 0;
error:
	return 1;
}

static void pci_remove(struct pci_dev *dev)
{
	pr_info("pci_remove\n");
	// pci_release_region(dev, NEWDEV_REG_PCI_BAR);
	pci_release_region(dev, NEWDEV_BUF_PCI_BAR);
	unregister_chrdev(major, DEV_NAME);
}

static struct pci_driver pci_driver = {
	.name     = DEV_NAME,
	.id_table = pci_ids,
	.probe    = pci_probe,
	.remove   = pci_remove,
};

static int myinit(void)
{
	if (pci_register_driver(&pci_driver) < 0) {
		return 1;
	}

	return 0;
}

static void myexit(void)
{
	pci_unregister_driver(&pci_driver);

}

module_init(myinit);
module_exit(myexit);