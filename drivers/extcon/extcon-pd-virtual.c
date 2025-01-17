/*
 * Copyright (c) 2016, Fuzhou Rockchip Electronics Co., Ltd
 * 
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/extcon.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/delay.h>

struct virtual_pd {
	struct extcon_dev *extcon;
	struct gpio_desc *gpio_vbus_5v;
	struct gpio_desc *gpio_hdmi_5v;
	struct gpio_desc *gpio_irq;
	struct device *dev;
	bool flip;
	bool usb_ss;
	bool enable;
	u8 mode;
	int irq;
	int enable_irq;
	int plug_state;
    struct work_struct work;
	struct workqueue_struct *virtual_pd_wq;
	spinlock_t irq_lock;
	struct delayed_work irq_work;
	int shake_lev;
};

static const unsigned int vpd_cable[] = {
	EXTCON_USB,
	EXTCON_USB_HOST,
	EXTCON_USB_VBUS_EN,
	EXTCON_CHG_USB_SDP,
	EXTCON_CHG_USB_CDP,
	EXTCON_CHG_USB_DCP,
/*
  FIXME: There's no real pd phy, control the charging is very
  dangerous, just rely on the BC detection. We don't use slow
  and fast.
*/
	EXTCON_CHG_USB_SLOW,
	EXTCON_CHG_USB_FAST,
	EXTCON_DISP_DP,
	EXTCON_NONE,
};

enum vpd_mode {
	VPD_DFP = 0,
	VPD_UFP,
	VPD_DP,
	VPD_DP_UFP,
};

static void vpd_set_vbus_enable(struct virtual_pd *vpd, bool enable)
{
	extcon_set_state(vpd->extcon, EXTCON_USB_VBUS_EN, enable);
	extcon_sync(vpd->extcon, EXTCON_USB_VBUS_EN);
	if (vpd->gpio_vbus_5v)
		gpiod_set_raw_value(vpd->gpio_vbus_5v, enable);
}

static void vpd_extcon_notify(struct virtual_pd *vpd, bool flip, bool usb_ss,
			      bool dfp, bool ufp, bool dp)
{
	union extcon_property_value property;

	property.intval = flip;
	extcon_set_property(vpd->extcon, EXTCON_USB,
			    EXTCON_PROP_USB_TYPEC_POLARITY, property);
	extcon_set_property(vpd->extcon, EXTCON_USB_HOST,
			    EXTCON_PROP_USB_TYPEC_POLARITY, property);
	extcon_set_property(vpd->extcon, EXTCON_DISP_DP,
			    EXTCON_PROP_USB_TYPEC_POLARITY, property);

	property.intval = usb_ss;
	extcon_set_property(vpd->extcon, EXTCON_USB,
			    EXTCON_PROP_USB_SS, property);
	extcon_set_property(vpd->extcon, EXTCON_USB_HOST,
			    EXTCON_PROP_USB_SS, property);
	extcon_set_property(vpd->extcon, EXTCON_DISP_DP,
			    EXTCON_PROP_USB_SS, property);
	extcon_set_state(vpd->extcon, EXTCON_USB, ufp);
	extcon_set_state(vpd->extcon, EXTCON_USB_HOST, dfp);
	extcon_set_state(vpd->extcon, EXTCON_DISP_DP, dp);
	extcon_sync(vpd->extcon, EXTCON_USB);
	extcon_sync(vpd->extcon, EXTCON_USB_HOST);
	extcon_sync(vpd->extcon, EXTCON_DISP_DP);
}

static void vpd_extcon_notify_set(struct virtual_pd *vpd)
{
	bool flip = vpd->flip, usb_ss = vpd->usb_ss;
	bool dfp = 0, ufp = 0, dp = 0;

	switch (vpd->mode) {
	case VPD_DFP:
		dfp = 1;
		break;
	case VPD_DP:
		dp = 1;
		dfp = 1;
		break;
	case VPD_DP_UFP:
		dp = 1;
		ufp = 1;
		break;
	case VPD_UFP:
		/* fall through */
	default:
		ufp = 1;
		break;
	}

	vpd_set_vbus_enable(vpd, !ufp);
	vpd_extcon_notify(vpd, flip, usb_ss, dfp, ufp, dp);
}

static void vpd_extcon_notify_clr(struct virtual_pd *vpd)
{
	vpd_set_vbus_enable(vpd, 0);
	vpd_extcon_notify(vpd, vpd->flip, vpd->usb_ss, 0, 0, 0);
}

static ssize_t vpd_flip_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct virtual_pd *vpd = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", vpd->flip);
}

static ssize_t vpd_flip_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct virtual_pd *vpd = dev_get_drvdata(dev);

	if (vpd->enable)
		goto out;

	if (strncmp(buf, "1", 1) == 0)
		vpd->flip = true;
	else
		vpd->flip = false;
out:
	return count;
}
static DEVICE_ATTR(flip, S_IWUSR | S_IRUSR, vpd_flip_show, vpd_flip_store);

static ssize_t vpd_ss_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct virtual_pd *vpd = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", vpd->usb_ss);
}

static ssize_t vpd_ss_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct virtual_pd *vpd = dev_get_drvdata(dev);

	if (vpd->enable)
		goto out;

	if (strncmp(buf, "1", 1) == 0)
		vpd->usb_ss = true;
	else
		vpd->usb_ss = false;
out:
	return count;
}
static DEVICE_ATTR(ss, S_IWUSR | S_IRUSR, vpd_ss_show, vpd_ss_store);

static ssize_t vpd_mode_show(struct device *dev,
			   struct device_attribute *attr, char *buf)
{
	struct virtual_pd *vpd = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", vpd->mode);
}

static ssize_t vpd_mode_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct virtual_pd *vpd = dev_get_drvdata(dev);

	if (vpd->enable)
		goto out;

	if (strncmp(buf, "0", 1) == 0)
		vpd->mode = VPD_DFP;
	else if (strncmp(buf, "2", 1) == 0)
		vpd->mode = VPD_DP;
	else if (strncmp(buf, "3", 1) == 0)
		vpd->mode = VPD_DP_UFP;
	else
		vpd->mode = VPD_UFP;
out:
	return count;
}
static DEVICE_ATTR(mode, S_IWUSR | S_IRUSR, vpd_mode_show, vpd_mode_store);

static ssize_t vpd_enable_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	struct virtual_pd *vpd = dev_get_drvdata(dev);

	return sprintf(buf, "%d\n", vpd->enable);
}

static ssize_t vpd_enable_store(struct device *dev,
			      struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct virtual_pd *vpd = dev_get_drvdata(dev);

	if (strncmp(buf, "1", 1) == 0) {
		if (vpd->enable)
			goto out;
		vpd->enable = true;
		vpd_extcon_notify_set(vpd);
	} else {
		if (!vpd->enable)
			goto out;
		vpd->enable = false;
		vpd_extcon_notify_clr(vpd);
	}
out:
	return count;
}
static DEVICE_ATTR(enable, S_IWUSR | S_IRUSR, vpd_enable_show, vpd_enable_store);

static struct attribute *vpd_attributes[] = {
	&dev_attr_flip.attr,
	&dev_attr_ss.attr,
	&dev_attr_mode.attr,
	&dev_attr_enable.attr,
	NULL
};

static const struct attribute_group vpd_attr_group = {
	.attrs = vpd_attributes,
};

void vpd_irq_disable(struct virtual_pd *vpd)
{
	unsigned long irqflags = 0;

	spin_lock_irqsave(&vpd->irq_lock, irqflags);
	if (!vpd->enable_irq) {
		disable_irq_nosync(vpd->irq);
		vpd->enable_irq = 1;
	} else {
		dev_warn(vpd->dev, "irq have already disabled\n");
	}
	spin_unlock_irqrestore(&vpd->irq_lock, irqflags);
}

void vpd_irq_enable(struct virtual_pd *vpd)
{
	unsigned long irqflags = 0;

	spin_lock_irqsave(&vpd->irq_lock, irqflags);
	if (vpd->enable_irq) {
		enable_irq(vpd->irq);
		vpd->enable_irq = 0;
	}
	spin_unlock_irqrestore(&vpd->irq_lock, irqflags);
}

static void virtual_pd_work_func(struct work_struct *work)
{
	
    struct virtual_pd *vpd;
	printk("%s %d =====>\n",__FUNCTION__,__LINE__);
	vpd = container_of(work, struct virtual_pd, work);
	
}

static void extcon_pd_delay_irq_work(struct work_struct *work)
{
	struct virtual_pd *vpd =
		container_of(work, struct virtual_pd, irq_work.work);
	int lev;
	lev = gpiod_get_raw_value(vpd->gpio_irq);
	
	if(vpd->shake_lev != lev) {
		vpd_irq_enable(vpd);
		return;
	}

    switch(vpd->plug_state) {
		case 1:
			if(lev==0) {
				vpd->enable = false;
				vpd_extcon_notify_clr(vpd);
				vpd->plug_state=0;
			}
			break;
		case 0:
			if(lev==1) {
				vpd->enable = true;
				vpd_extcon_notify_set(vpd);
				vpd->plug_state=1;
			}
			break;	
		default:
			break;
	}
	vpd_irq_enable(vpd);
}

static irqreturn_t dp_det_irq_handler(int irq, void *dev_id)
{
    struct virtual_pd *vpd = dev_id;
	int lev;
	lev=gpiod_get_raw_value(vpd->gpio_irq);
	vpd->shake_lev = lev;
	schedule_delayed_work(&vpd->irq_work, msecs_to_jiffies(10));
	vpd_irq_disable(vpd);
	return IRQ_HANDLED;
}

static void vpd_extcon_init(struct virtual_pd *vpd)
{
	struct device *dev = vpd->dev;
	u32 tmp = 0;
	int ret = 0;

	ret = device_property_read_u32(dev, "vpd,init-flip", &tmp);
	if (ret < 0)
		vpd->flip = 0;
	else
		vpd->flip = tmp;
	dev_dbg(dev, "init-flip = %d\n", vpd->flip);

	ret = device_property_read_u32(dev, "vpd,init-ss", &tmp);
	if (ret < 0)
		vpd->usb_ss = 0;
	else
		vpd->usb_ss = tmp;
	dev_dbg(dev, "init-ss = %d\n", vpd->usb_ss);

	ret = device_property_read_u32(dev, "vpd,init-mode", &tmp);
	if (ret < 0)
		vpd->mode = 0;
	else
		vpd->mode = tmp;
	dev_dbg(dev, "init-mode = %d\n", vpd->flip);
	if(gpiod_get_raw_value(vpd->gpio_irq)) {
		vpd_extcon_notify_set(vpd);
		vpd->plug_state=1;
	}

	spin_lock_init(&vpd->irq_lock);
}

static int vpd_extcon_probe(struct platform_device *pdev)
{
	struct virtual_pd *vpd;
	struct device *dev = &pdev->dev;
	int ret = 0;
	

	dev_info(dev, "%s: %d start\n", __func__, __LINE__);

	vpd = devm_kzalloc(dev, sizeof(*vpd), GFP_KERNEL);
	if (!vpd)
		return -ENOMEM;

	vpd->dev = dev;
	dev_set_drvdata(dev, vpd);
	vpd->enable = 1;

	vpd->extcon = devm_extcon_dev_allocate(dev, vpd_cable);
	if (IS_ERR(vpd->extcon)) {
		dev_err(dev, "allocat extcon failed\n");
		return PTR_ERR(vpd->extcon);
	}

	ret = devm_extcon_dev_register(dev, vpd->extcon);
	if (ret) {
		dev_err(dev, "register extcon failed: %d\n", ret);
		return ret;
	}

	vpd->gpio_vbus_5v = devm_gpiod_get_optional(dev,"vbus-5v",
						    GPIOD_OUT_LOW);
	if (IS_ERR(vpd->gpio_vbus_5v)) {
		dev_warn(dev, "maybe miss named GPIO for vbus-5v\n");
		vpd->gpio_vbus_5v = NULL;
	} else
		gpiod_set_raw_value(vpd->gpio_vbus_5v, 0);

	vpd->gpio_hdmi_5v = devm_gpiod_get_optional(dev,"hdmi-5v",
						    GPIOD_OUT_LOW);
	if (IS_ERR(vpd->gpio_hdmi_5v)) {
		dev_warn(dev, "maybe miss named GPIO for gpio_hdmi_5v\n");
		vpd->gpio_hdmi_5v = NULL;
	}

	vpd->gpio_irq = devm_gpiod_get_optional(dev,"dp-det",
						    GPIOD_OUT_LOW);
	
	
	if (IS_ERR(vpd->gpio_irq)) {
		dev_warn(dev, "maybe miss named GPIO for dp-det\n");
		vpd->gpio_irq = NULL;
	} 
	

	ret = extcon_set_property_capability(vpd->extcon, EXTCON_USB,
					     EXTCON_PROP_USB_TYPEC_POLARITY);
	if (ret) {
		dev_err(dev,
			"set USB property capability failed: %d\n", ret);
		return ret;
	}

	ret = extcon_set_property_capability(vpd->extcon, EXTCON_USB_HOST,
					     EXTCON_PROP_USB_TYPEC_POLARITY);
	if (ret) {
		dev_err(dev,
			"set USB_HOST property capability failed: %d\n",
			ret);
		return ret;
	}

	ret = extcon_set_property_capability(vpd->extcon, EXTCON_DISP_DP,
					     EXTCON_PROP_USB_TYPEC_POLARITY);
	if (ret) {
		dev_err(dev,
			"set DISP_DP property capability failed: %d\n",
			ret);
		return ret;
	}

	ret = extcon_set_property_capability(vpd->extcon, EXTCON_USB,
					     EXTCON_PROP_USB_SS);
	if (ret) {
		dev_err(dev,
			"set USB USB_SS property capability failed: %d\n",
			ret);
		return ret;
	}

	ret = extcon_set_property_capability(vpd->extcon, EXTCON_USB_HOST,
					     EXTCON_PROP_USB_SS);
	if (ret) {
		dev_err(dev,
			"set USB_HOST USB_SS property capability failed: %d\n",
			ret);
		return ret;
	}

	ret = extcon_set_property_capability(vpd->extcon, EXTCON_DISP_DP,
					     EXTCON_PROP_USB_SS);
	if (ret) {
		dev_err(dev,
			"set DISP_DP USB_SS property capability failed: %d\n",
			ret);
		return ret;
	}

	ret = extcon_set_property_capability(vpd->extcon, EXTCON_CHG_USB_FAST,
					     EXTCON_PROP_USB_TYPEC_POLARITY);
	if (ret) {
		dev_err(dev,
			"set USB_PD property capability failed: %d\n", ret);
		return ret;
	}

	vpd_extcon_init(vpd);
	INIT_DELAYED_WORK(&vpd->irq_work, extcon_pd_delay_irq_work);

	vpd->irq=gpiod_to_irq(vpd->gpio_irq);
	printk("%s %d =====>%d\n",__FUNCTION__,__LINE__,vpd->irq);
    if (vpd->irq){
	 ret = devm_request_threaded_irq(dev, 
	 	                vpd->irq, 
	 	                NULL, 
                        dp_det_irq_handler,
                        //IRQF_TRIGGER_HIGH | IRQF_ONESHOT , 
                        IRQF_TRIGGER_FALLING |IRQF_TRIGGER_RISING | IRQF_ONESHOT ,
                        NULL, 
                        vpd);	
    }
	else
	dev_err(dev,"gpio can not be irq !\n");

	vpd->virtual_pd_wq = create_workqueue("virtual_pd_wq");
	INIT_WORK(&vpd->work, virtual_pd_work_func);
		
	ret = sysfs_create_group(&dev->kobj, &vpd_attr_group);
	if (ret < 0)
		dev_warn(dev, "attr group create failed\n");

	dev_info(dev, "%s: %d sussess\n", __func__, __LINE__);

	return 0;
}

static int vpd_extcon_remove(struct platform_device *pdev)
{
	return 0;
}

#ifdef CONFIG_PM_SLEEP
static int vpd_extcon_suspend(struct device *dev)
{
	struct virtual_pd *vpd = dev_get_drvdata(dev);

	int lev=0;
	lev = gpiod_get_raw_value(vpd->gpio_irq);
	cancel_delayed_work_sync(&vpd->irq_work);
	vpd_irq_disable(vpd);
	if (vpd->gpio_hdmi_5v)
		gpiod_set_raw_value(vpd->gpio_hdmi_5v, 0);
	return 0;
}

static int vpd_extcon_resume(struct device *dev)
{
	struct virtual_pd *vpd = dev_get_drvdata(dev);
	if (vpd->gpio_hdmi_5v) {
		gpiod_set_raw_value(vpd->gpio_hdmi_5v, 1);
		msleep(800);
	}
	vpd_irq_enable(vpd);
	return 0;
}
#endif

static SIMPLE_DEV_PM_OPS(vpd_extcon_pm_ops,
			 vpd_extcon_suspend, vpd_extcon_resume);

static const struct of_device_id vpd_extcon_dt_match[] = {
	{ .compatible = "linux,extcon-pd-virtual", },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, usb_extcon_dt_match);

static struct platform_driver vpd_extcon_driver = {
	.probe		= vpd_extcon_probe,
	.remove		= vpd_extcon_remove,
	.driver		= {
		.name	= "extcon-pd-virtual",
		.pm	= &vpd_extcon_pm_ops,
		.of_match_table = vpd_extcon_dt_match,
	},
};

static int __init __vpd_extcon_init(void)
{
		return platform_driver_register(&vpd_extcon_driver);
}

static void __exit __vpd_extcon_exit(void)
{
		platform_driver_unregister(&vpd_extcon_driver);
}

module_init(__vpd_extcon_init);
module_exit(__vpd_extcon_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("rockchip");
MODULE_DESCRIPTION("Virtual Typec-pd extcon driver");
