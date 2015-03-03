/*
 *  si5326.c - Linux kernel module for
 *  any frequency precision clock multiplier/jitter attenuator.
 *
 *  NB: this module claims NO KNOWLEDGE of how this device works,
 *  the manual:
 *  https://www.silabs.com/Support%20Documents/TechnicalDocs/Si5326.pdf
 *
 *  manages to simultaneously be
 *  a/ very long
 *  b/ completely fail to explain how to use the device
 *  By this I mean - there are 35 pages of register _definitions_ but zero pages
 *  to explain the usage of these settings
 *
 *  Si do provide a windows only "setup program". For a given setting, this
 *  generates a register/value map.
 *
 *  The strategy of this "driver" is to provide a single  hook to provide
 *  read/write access to any register. The intention is then to play the
 *  program-generated map out the the device via the hook
 *
 *  si5326_reg:
 *  echo addr value > si5326_reg   		# write value to addr
 *  echo addr > si5326_reg;cat si5326_reg	# read addr
 *
 *
 *  Copyright (c) 2014 Peter Milne <peter.milne@d-tacq.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/slab.h>
#include <linux/i2c.h>
#include <linux/mutex.h>
#include <linux/delay.h>

#define SI5326_DRV_NAME	"si5326"
#define DRIVER_VERSION		"0.02"

const u8 si5326_reset_values[] = {
	0x14, 0xe4, 0x42, 0x05
};
#define NRESETS	sizeof(si5326_reset_values)

struct si5326_data {
	struct i2c_client *client;
	struct mutex lock;
	int last_addr;
};

/*
 * register access helpers
 */

static int si5326_read_reg(struct i2c_client *client, int reg)
{
	//struct si5326_data *data = i2c_get_clientdata(client);
	return i2c_smbus_read_byte_data(client, reg);
}

static int si5326_write_reg(struct i2c_client *client, int reg, u8 value)
{
	//struct si5326_data *data = i2c_get_clientdata(client);
	return i2c_smbus_write_byte_data(client, reg, value);
}

/*
 * sysfs layer
 */

static ssize_t si5326_show_reg(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct si5326_data *data = i2c_get_clientdata(client);

	int rc = si5326_read_reg(client, data->last_addr);
	if (rc >= 0){
		return sprintf(buf, "%02x %02x", data->last_addr, rc);
	}else{
		return rc;
	}
}

static ssize_t si5326_store_reg(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct i2c_client *client = to_i2c_client(dev);
	struct si5326_data *data = i2c_get_clientdata(client);
	unsigned addr, value;

	if (buf[0] == '#'){
		return count;
	}else{
		switch(sscanf(buf, "%d 0x%x", &addr, &value)){
		case 2:
			if(si5326_write_reg(client, addr, value)){
				return -EIO;
			}else{
				return count;
			}
		case 1:
			data->last_addr = addr;
			return count;
		default:
			dev_err(dev, "si5326_store_reg: FAIL \"%s\"", buf);
			return -EIO;
		}
	}
}

static DEVICE_ATTR(si5326_reg, S_IWUSR | S_IRUGO,
		si5326_show_reg, si5326_store_reg);




static struct attribute *si5326_attributes[] = {
	&dev_attr_si5326_reg.attr,
	NULL
};

static const struct attribute_group si5326_attr_group = {
	.attrs = si5326_attributes,
};

/*
 * I2C layer
 */


int si5326_init_client(struct i2c_client *client)
{
	int rc;
	int regs[NRESETS];
	int ir;
	int fail = 0;

	for (ir = 0; ir < NRESETS; ++ir){
		if ((rc = si5326_read_reg(client, ir)) < 0){
			dev_err(&client->dev, "ERROR failed to read reg%d:%d", ir, rc);
			return -EIO;
		}else{
			regs[ir] = rc;
			if (regs[ir] != si5326_reset_values[ir]){
				dev_err(&client->dev, "ERROR: register at [%d] %02x not reset value %02x",
						ir, regs[ir], si5326_reset_values[ir]);
				fail = 1;
			}
		}
	}
	if (fail){
		return -ENODEV;
	}else{
		dev_info(&client->dev, "si5326 found with reset values in first %d regs", NRESETS);
		return 0;
	}
}
static int si5326_probe(struct i2c_client *client,
				    const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct si5326_data *data;
	int err = 0;

	if (!i2c_check_functionality(adapter, I2C_FUNC_SMBUS_BYTE))
		return -EIO;

	data = kzalloc(sizeof(struct si5326_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->client = client;
	i2c_set_clientdata(client, data);
	mutex_init(&data->lock);

	err = si5326_init_client(client);
	if (err)
		goto exit_kfree;

	/* register sysfs hooks */
	err = sysfs_create_group(&client->dev.kobj, &si5326_attr_group);
	if (err)
		goto exit_kfree;

	dev_info(&client->dev, "si5326 added device %s\n", id->name);
	return 0;

exit_kfree:
	kfree(data);
	return err;
}

static int si5326_remove(struct i2c_client *client)
{
	sysfs_remove_group(&client->dev.kobj, &si5326_attr_group);
	kfree(i2c_get_clientdata(client));
	return 0;
}



static const struct i2c_device_id si5326_id[] = {
	{ "si5326", 0 },
	{}
};
MODULE_DEVICE_TABLE(i2c, si5326_id);


static const struct of_device_id si5326_dt_ids[] = {
	{ .compatible = "si,si5326", },
	{}
};
static struct i2c_driver si5326_driver = {
	.driver = {
		.name	= SI5326_DRV_NAME,
		.owner	= THIS_MODULE,
		.of_match_table = si5326_dt_ids,
	},
/*
	.suspend = isl22313_suspend,
	.resume	= isl22313_resume,
*/
	.probe	= si5326_probe,
	.remove	= si5326_remove,
	.id_table = si5326_id,
};

static int __init si5326_init(void)
{
	printk( "si5326 driver version %s\n", DRIVER_VERSION);
	return i2c_add_driver(&si5326_driver);
}

static void __exit si5326_exit(void)
{
	i2c_del_driver(&si5326_driver);
}

MODULE_AUTHOR("Peter Milne peter.milne@d-tacq.com");
MODULE_DESCRIPTION("si5326 clock driver driver");
MODULE_LICENSE("GPL v2");
MODULE_VERSION(DRIVER_VERSION);

module_init(si5326_init);
module_exit(si5326_exit);

