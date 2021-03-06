/*
 * driver/barcode_emul Barcode emulator driver
 *
 * Copyright (C) 2012 Samsung Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307 USA
 *
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/i2c.h>
#include <linux/irq.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/workqueue.h>
#include <linux/device.h>
#include <linux/earlysuspend.h>
#include <linux/spinlock.h>
#include <linux/gpio.h>
#include <linux/uaccess.h>
#include <linux/fs.h>
#include <linux/clk.h>
#include <linux/firmware.h>
#include <linux/barcode_emul.h>
#if defined(CONFIG_IR_REMOCON_FPGA)
#include <linux/ir_remote_con.h>
#endif
#include "barcode_emul_ice4.h"
#include <linux/err.h>

#if defined(TEST_DEBUG)
#define pr_barcode	pr_emerg
#else
#define pr_barcode	pr_info
#endif

#define LOOP_BACK	48
#define TEST_CODE1	49
#define TEST_CODE2	50
#define FW_VER_ADDR	0x80
#define BEAM_STATUS_ADDR	0xFE
#define SEC_FPGA_MAX_FW_PATH	255
#define SEC_FPGA_FW_FILENAME		"i2c_top_bitmap.bin"

#define BARCODE_I2C_ADDR	0x6C

#if defined(CONFIG_IR_REMOCON_FPGA)
#define IRDA_I2C_ADDR		0x50
#define IRDA_TEST_CODE_SIZE	140
#define IRDA_TEST_CODE_ADDR	0x00
#define MAX_SIZE		2048
#define MC96_READ_LENGTH	8
#endif

#define US_TO_PATTERN		1000000

struct barcode_emul_data {
	struct i2c_client		*client;
	struct workqueue_struct		*firmware_dl;
	struct delayed_work		fw_dl;
	const struct firmware		*fw;
#if defined(CONFIG_IR_REMOCON_FPGA)
	struct mutex			mutex;
	struct {
		unsigned char		addr;
		unsigned char		data[MAX_SIZE];
	} i2c_block_transfer;
	struct mc96_platform_data	*pdata;
	int				length;
	int				count;
	int				dev_id;
	int				ir_freq;
	int				ir_sum;
	int				on_off;
#endif
};

#if defined(CONFIG_IR_REMOCON_FPGA)
static int ack_number;
static int count_number;
#endif
static struct barcode_emul_platform_data *g_pdata;
static struct clk *main_clk;
static int Is_clk_enabled;
static struct mutex		en_mutex;
static struct i2c_client *g_client;
#ifndef CONFIG_NO_GPIO_EXPANDER_FPGA
static int gpiox_state;
#endif
static bool fw_dl_complete;

static struct platform_device barcode_mclk_dev = {
	.name = "barcode_mclk_dev_pdev",
	.id = -1,
	.dev = {
		.init_name = "barcode_mclk_dev",
	},
};

static void fpga_enable(int enable)
{
	if (enable) {
		if (!Is_clk_enabled) {
			mutex_lock(&en_mutex);
			if (g_pdata->fw_type) {
				if (g_pdata->clock_en)
					g_pdata->clock_en(1);
			} else {
				clk_prepare_enable(main_clk);
			}
			gpio_set_value(g_pdata->rst_n, GPIO_LEVEL_HIGH);
			usleep_range(1000, 2000);
			Is_clk_enabled = 1;
		}
	} else {
		if (Is_clk_enabled) {
			usleep_range(20000, 25000);
			gpio_set_value(g_pdata->rst_n, GPIO_LEVEL_LOW);
			if (g_pdata->fw_type) {
				if (g_pdata->clock_en)
					g_pdata->clock_en(0);
			} else {
				clk_disable_unprepare(main_clk);
			}
			Is_clk_enabled = 0;
			mutex_unlock(&en_mutex);
		}
	}
}

/*
 * Send barcode emulator firmware data thougth spi communication
 */
static int barcode_send_firmware_data(const u8 *data, int len)
{
	unsigned int i, j;
	unsigned char spibit;

	i = 0;
	while (i < len) {
		j = 0;
		spibit = data[i];
		while (j < 8) {
			gpio_set_value_cansleep(g_pdata->spi_clk,
						GPIO_LEVEL_LOW);

			if (spibit & 0x80)
				gpio_set_value_cansleep(g_pdata->spi_si,
						GPIO_LEVEL_HIGH);
			else
				gpio_set_value_cansleep(g_pdata->spi_si,
						GPIO_LEVEL_LOW);

			j = j+1;
			gpio_set_value_cansleep(g_pdata->spi_clk,
						GPIO_LEVEL_HIGH);
			spibit = spibit<<1;
		}
		i = i+1;
	}

	i = 0;
	while (i < 200) {
		gpio_set_value_cansleep(g_pdata->spi_clk, GPIO_LEVEL_LOW);
		i = i+1;
		gpio_set_value_cansleep(g_pdata->spi_clk, GPIO_LEVEL_HIGH);
	}
	return 0;
}

static int check_fpga_cdone(void)
{
    /* Melius Device for US and EUR don't support CDONE */
	if (g_pdata->cdone == 0)
		return 0;

    /* Device in Operation when CDONE='1'; Device Failed when CDONE='0'. */
	if (gpio_get_value(g_pdata->cdone) != 1) {
		pr_barcode("CDONE_FAIL %d\n", gpio_get_value(g_pdata->cdone));
		return -EIO;
	} else {
		pr_barcode("CDONE_SUCCESS\n");
		return 0;
	}
}

static int barcode_fpga_fimrware_update_start(const u8 *data, int len)
{
	pr_barcode("%s\n", __func__);

	gpio_set_value(g_pdata->rst_n, GPIO_LEVEL_LOW);
	gpio_set_value(g_pdata->cresetb, GPIO_LEVEL_LOW);
	usleep_range(30, 50);

	gpio_set_value(g_pdata->cresetb, GPIO_LEVEL_HIGH);
	usleep_range(1000, 1300);
	barcode_send_firmware_data(data, len);
	usleep_range(50, 70);
	gpio_set_value(g_pdata->rst_n, GPIO_LEVEL_HIGH);
	gpio_set_value(g_pdata->spi_si, GPIO_LEVEL_HIGH);

	if (check_fpga_cdone()) {
		pr_barcode("barcode firmware update fail\n");
	} else {
		udelay(5);
		pr_barcode("barcode firmware update success\n");
		fw_dl_complete = true;
	}
	return 0;
}

void ice4_fpga_firmware_update(void)
{
	if (!g_pdata->fw_type)
		barcode_fpga_fimrware_update_start(spiword_legacy,
						sizeof(spiword_legacy));
	else if (g_pdata->fw_type == ICE_IRDA)
		barcode_fpga_fimrware_update_start(spiword_irda,
						sizeof(spiword_irda));
	else if (g_pdata->fw_type == ICE_IRDA_SERRANO)
		barcode_fpga_fimrware_update_start(spiword_irda_serrano,
						sizeof(spiword_irda_serrano));
}

static ssize_t barcode_emul_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int ret;
	struct barcode_emul_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	pr_barcode("%s start\n", __func__);
	if (check_fpga_cdone()) {
		pr_err("%s: cdone fail !!\n", __func__);
		return 1;
	}
	fpga_enable(1);

	client->addr = BARCODE_I2C_ADDR;
	ret = i2c_master_send(client, buf, size);
	if (ret < 0) {
		pr_err("%s: i2c err1 %d\n", __func__, ret);
		ret = i2c_master_send(client, buf, size);
		if (ret < 0)
			pr_err("%s: i2c err2 %d\n", __func__, ret);
	}
	pr_barcode("%s complete\n", __func__);

	fpga_enable(0);

	return size;
}

static ssize_t barcode_emul_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	return strlen(buf);
}
static DEVICE_ATTR(barcode_send, 0664, barcode_emul_show, barcode_emul_store);

static ssize_t barcode_emul_fw_update_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct barcode_emul_data *data = dev_get_drvdata(dev);
	if (request_firmware(&data->fw, SEC_FPGA_FW_FILENAME,
							dev)) {
		pr_err("%s: Can't open firmware file\n",
						__func__);
		goto firmwareload_fail;
	}
	barcode_fpga_fimrware_update_start(data->fw->data, data->fw->size);
	release_firmware(data->fw);
firmwareload_fail:
	return size;
}

static ssize_t barcode_emul_fw_update_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	return strlen(buf);
}
static DEVICE_ATTR(barcode_fw_update, 0664,
		barcode_emul_fw_update_show,
		barcode_emul_fw_update_store);

static int barcode_emul_read(struct i2c_client *client, u16 addr,
		u8 length, u8 *value)
{

	struct i2c_msg msg[2];
	int ret;

	msg[0].addr  = client->addr;
	msg[0].flags = 0x00;
	msg[0].len   = 1;
	msg[0].buf   = (u8 *) &addr;

	msg[1].addr  = client->addr;
	msg[1].flags = I2C_M_RD;
	msg[1].len   = length;
	msg[1].buf   = (u8 *) value;

	fpga_enable(1);

	ret = i2c_transfer(client->adapter, msg, 2);
	if  (ret == 2) {
		fpga_enable(0);
		return 0;
	} else {
		pr_barcode("%s: err1 %d\n", __func__, ret);
		fpga_enable(0);
		return -EIO;
	}
}

static ssize_t barcode_emul_test_store(struct device *dev,
		struct device_attribute *attr,
		const char *buf, size_t size)
{
	int ret, i;
	struct barcode_emul_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	struct {
			unsigned char addr;
			unsigned char data[20];
		} i2c_block_transfer;
	unsigned char barcode_data[14] = {0xFF, 0xAC, 0xDB, 0x36, 0x42, 0x85,
			0x0A, 0xA8, 0xD1, 0xA3, 0x46, 0xC5, 0xDA, 0xFF};

	client->addr = BARCODE_I2C_ADDR;
	if (check_fpga_cdone()) {
		pr_err("%s: cdone fail !!\n", __func__);
		return 1;
	}
	fpga_enable(1);

	if (buf[0] == LOOP_BACK) {
		for (i = 0; i < size; i++)
			i2c_block_transfer.data[i] = *buf++;

		i2c_block_transfer.addr = 0x01;
		pr_barcode("%s: write addr: %d, value: %d\n", __func__,
			i2c_block_transfer.addr, i2c_block_transfer.data[0]);

		ret = i2c_master_send(client,
			(unsigned char *) &i2c_block_transfer, 2);
		if (ret < 0) {
			pr_barcode("%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				(unsigned char *) &i2c_block_transfer, 2);
			if (ret < 0)
				pr_barcode("%s: err2 %d\n", __func__, ret);
		}

	} else if (buf[0] == TEST_CODE1) {
		unsigned char BSR_data[6] =\
			{0xC8, 0x00, 0x32, 0x01, 0x00, 0x32};

		pr_barcode("barcode test code start\n");

		/* send NH */
		i2c_block_transfer.addr = 0x80;
		i2c_block_transfer.data[0] = 0x05;
		ret = i2c_master_send(client,
			(unsigned char *) &i2c_block_transfer, 2);
		if (ret < 0) {
			pr_err("%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				(unsigned char *) &i2c_block_transfer, 2);
			if (ret < 0)
				pr_err("%s: err2 %d\n", __func__, ret);
		}

		/* setup BSR data */
		for (i = 0; i < 6; i++)
			i2c_block_transfer.data[i+1] = BSR_data[i];

		/* send BSR1 */
		/* NS 1= 200, ISD 1 = 100us, IPD 1 = 200us,
		   BL 1=14, BW 1=4MHZ */
		i2c_block_transfer.addr = 0x81;
		i2c_block_transfer.data[0] = 0x00;

		ret = i2c_master_send(client,
			(unsigned char *) &i2c_block_transfer, 8);
		if (ret < 0) {
			pr_err("%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				(unsigned char *) &i2c_block_transfer, 8);
			if (ret < 0)
				pr_err("%s: err2 %d\n", __func__, ret);
		}

		/* send BSR2 */
		/* NS 2= 200, ISD 2 = 100us, IPD 2 = 200us,
		   BL 2=14, BW 2=2MHZ*/
		i2c_block_transfer.addr = 0x88;
		i2c_block_transfer.data[0] = 0x01;

		ret = i2c_master_send(client,
			(unsigned char *) &i2c_block_transfer, 8);
		if (ret < 0) {
			pr_err("%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				(unsigned char *) &i2c_block_transfer, 8);
			if (ret < 0)
				pr_err("%s: err2 %d\n", __func__, ret);
		}


		/* send BSR3 */
		/* NS 3= 200, ISD 3 = 100us, IPD 3 = 200us,
		   BL 3=14, BW 3=1MHZ*/
		i2c_block_transfer.addr = 0x8F;
		i2c_block_transfer.data[0] = 0x02;

		ret = i2c_master_send(client,
			(unsigned char *) &i2c_block_transfer, 8);
		if (ret < 0) {
			pr_err("%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				(unsigned char *) &i2c_block_transfer, 8);
			if (ret < 0)
				pr_err("%s: err2 %d\n", __func__, ret);
		}

		/* send BSR4 */
		/* NS 4= 200, ISD 4 = 100us, IPD 4 = 200us,
		   BL 4=14, BW 4=500KHZ*/
		i2c_block_transfer.addr = 0x96;
		i2c_block_transfer.data[0] = 0x04;

		ret = i2c_master_send(client,
			(unsigned char *) &i2c_block_transfer, 8);
		if (ret < 0) {
			pr_err("%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				(unsigned char *) &i2c_block_transfer, 8);
			if (ret < 0)
				pr_err("%s: err2 %d\n", __func__, ret);
		}

		/* send BSR5 */
		/* NS 5= 200, ISD 5 = 100us, IPD 5 = 200us,
		   BL 5=14, BW 5=250KHZ*/
		i2c_block_transfer.addr = 0x9D;
		i2c_block_transfer.data[0] = 0x08;

		ret = i2c_master_send(client,
			(unsigned char *) &i2c_block_transfer, 8);
		if (ret < 0) {
			pr_err("%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				(unsigned char *) &i2c_block_transfer, 8);
			if (ret < 0)
				pr_err("%s: err2 %d\n", __func__, ret);
		}

		/* send barcode data */
		i2c_block_transfer.addr = 0x00;
		for (i = 0; i < 14; i++)
			i2c_block_transfer.data[i] = barcode_data[i];

		ret = i2c_master_send(client,
			(unsigned char *) &i2c_block_transfer, 15);
		if (ret < 0) {
			pr_err("%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				(unsigned char *) &i2c_block_transfer, 15);
			if (ret < 0)
				pr_err("%s: err2 %d\n", __func__, ret);
		}

		/* send START */
		i2c_block_transfer.addr = 0xFF;
		i2c_block_transfer.data[0] = 0x0E;
		ret = i2c_master_send(client,
			(unsigned char *) &i2c_block_transfer, 2);
		if (ret < 0) {
			pr_err("%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				(unsigned char *) &i2c_block_transfer, 2);
			if (ret < 0)
				pr_err("%s: err2 %d\n", __func__, ret);
		}

	} else if (buf[0] == TEST_CODE2) {
		pr_barcode("barcode test code stop\n");
		i2c_block_transfer.addr = 0xFF;
		i2c_block_transfer.data[0] = 0x00;
		ret = i2c_master_send(client,
			(unsigned char *) &i2c_block_transfer, 2);
		if (ret < 0) {
			pr_err("%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				(unsigned char *) &i2c_block_transfer, 2);
			if (ret < 0)
				pr_err("%s: err2 %d\n", __func__, ret);
		}

/*
		i2c_block_transfer.addr = 0x01;
		i2c_block_transfer.data[0] = 0x01;
		ret = i2c_master_send(client,
			(unsigned char *) &i2c_block_transfer, 2);
		if (ret < 0) {
			pr_err("%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				(unsigned char *) &i2c_block_transfer, 2);
			if (ret < 0)
				pr_err("%s: err2 %d\n", __func__, ret);
		}

		i2c_block_transfer.addr = 0x02;
		for (i = 0; i < 14; i++)
			i2c_block_transfer.data[i] = barcode_data[i];

		ret = i2c_master_send(client,
			(unsigned char *) &i2c_block_transfer, 15);
		if (ret < 0) {
			pr_err("%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				(unsigned char *) &i2c_block_transfer, 15);
			if (ret < 0)
				pr_err("%s: err2 %d\n", __func__, ret);
		}

		i2c_block_transfer.addr = 0x00;
		i2c_block_transfer.data[0] = 0x01;
		ret = i2c_master_send(client,
			(unsigned char *) &i2c_block_transfer, 2);
		if (ret < 0) {
			pr_err("%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				(unsigned char *) &i2c_block_transfer, 2);
			if (ret < 0)
				pr_err("%s: err2 %d\n", __func__, ret);
		}
 */
	}

	fpga_enable(0);

	return size;
}

static ssize_t barcode_emul_test_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	return strlen(buf);
}
static DEVICE_ATTR(barcode_test_send, 0664,
		barcode_emul_test_show, barcode_emul_test_store);

static ssize_t barcode_ver_check_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct barcode_emul_data *data = dev_get_drvdata(dev);
	u8 fw_ver;

	barcode_emul_read(data->client, FW_VER_ADDR, 1, &fw_ver);
	fw_ver = (fw_ver >> 5) & 0x7;

	return sprintf(buf, "%d\n", fw_ver+14);
}
static DEVICE_ATTR(barcode_ver_check, 0664, barcode_ver_check_show, NULL);

static ssize_t barcode_led_status_show(struct device *dev,
		struct device_attribute *attr,
		char *buf)
{
	struct barcode_emul_data *data = dev_get_drvdata(dev);
	u8 status;
	barcode_emul_read(data->client, BEAM_STATUS_ADDR, 1, &status);
	status = status & 0x1;
	return sprintf(buf, "%d\n", status);
}
static DEVICE_ATTR(barcode_led_status, 0664, barcode_led_status_show, NULL);

#if defined(CONFIG_IR_REMOCON_FPGA)
static void irda_add_checksum_length(struct barcode_emul_data *ir_data,
				int count)
{
	struct barcode_emul_data *data = ir_data;
	int i = 0, csum = 0;

	pr_barcode("%s: length: %04x\n", __func__, count);

	data->i2c_block_transfer.data[0] = count >> 8;
	data->i2c_block_transfer.data[1] = count & 0xff;

	while (i < count) {
		csum += data->i2c_block_transfer.data[i];
		i++;
	}

	pr_barcode("%s: checksum: %04x\n", __func__, csum);

	data->i2c_block_transfer.data[count] = csum >> 8;
	data->i2c_block_transfer.data[count+1] = csum & 0xff;
}
/* sysfs node ir_send */
static void ir_remocon_work(struct barcode_emul_data *ir_data, int count)
{

	struct barcode_emul_data *data = ir_data;
	struct i2c_client *client = data->client;

	int buf_size = count+2;
	int ret;
	int emission_time;
	int ack_pin_onoff;

	if (count_number >= 100)
		count_number = 0;

	count_number++;

	pr_barcode("%s: total buf_size: %d\n", __func__, buf_size);

	fpga_enable(1);

	mutex_lock(&data->mutex);

	client->addr = IRDA_I2C_ADDR;

	if (g_pdata->fw_type) {
		data->i2c_block_transfer.addr = 0x00;
		data->i2c_block_transfer.data[0] = count >> 8;
		data->i2c_block_transfer.data[1] = count & 0xff;
		buf_size++;
		ret = i2c_master_send(client,
			(unsigned char *) &(data->i2c_block_transfer),
								buf_size);
		if (ret < 0) {
			dev_err(&client->dev, "%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
			(unsigned char *) &(data->i2c_block_transfer),
								buf_size);
			if (ret < 0)
				dev_err(&client->dev, "%s: err2 %d\n",
								__func__, ret);
		}
	} else {
		irda_add_checksum_length(data, count);
		ret = i2c_master_send(client, data->i2c_block_transfer.data,
								buf_size);
		if (ret < 0) {
			dev_err(&client->dev, "%s: err1 %d\n", __func__, ret);
			ret = i2c_master_send(client,
				data->i2c_block_transfer.data, buf_size);
			if (ret < 0)
				dev_err(&client->dev, "%s: err2 %d\n",
								__func__, ret);
		}
	}
	usleep_range(10000, 12000);

	ack_pin_onoff = 0;
/* Unavailable in Lattice */
	if (gpio_get_value(g_pdata->irda_irq)) {
		pr_barcode("%s : %d Checksum NG!\n",
			__func__, count_number);
		ack_pin_onoff = 1;
	} else {
		pr_barcode("%s : %d Checksum OK!\n",
			__func__, count_number);
		ack_pin_onoff = 2;
	}
	ack_number = ack_pin_onoff;

	mutex_unlock(&data->mutex);

/*
	for (int i = 0; i < buf_size; i++) {
		printk(KERN_INFO "%s: data[%d] : 0x%02x\n", __func__, i,
				data->i2c_block_transfer.data[i]);
	}
*/
	data->count = 2;

/*
	printk(KERN_INFO "%s: sleep_timing = %d\n", __func__, sleep_timing);
*/
	emission_time = \
		(1000 * (data->ir_sum) / (data->ir_freq));
	if (emission_time > 0)
		msleep(emission_time);
		pr_barcode("%s: emission_time = %d\n",
					__func__, emission_time);

	if (gpio_get_value(g_pdata->irda_irq)) {
		pr_barcode("%s : %d Sending IR OK!\n",
				__func__, count_number);
		ack_pin_onoff = 4;
	} else {
		pr_barcode("%s : %d Sending IR NG!\n",
				__func__, count_number);
		ack_pin_onoff = 2;
	}

	ack_number += ack_pin_onoff;
#ifndef USE_STOP_MODE
	g_pdata->ir_vdd_onoff(0);
	data->on_off = 0;
	g_pdata->ir_wake_en(0);
#endif
	data->ir_freq = 0;
	data->ir_sum = 0;

	fpga_enable(0);
}

static ssize_t remocon_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t size)
{
	struct barcode_emul_data *data = dev_get_drvdata(dev);
	unsigned int _data, _tdata;
	int count, i, converting_factor = 1;

	pr_barcode("ir_send called\n");

	for (i = 0; i < MAX_SIZE; i++) {
		if (sscanf(buf++, "%u", &_data) == 1) {
			if (_data == 0 || buf == '\0')
				break;
			// 2 is the initial value of count
			if (data->count == 2) {
				data->ir_freq = _data;
				converting_factor = US_TO_PATTERN / data->ir_freq;
				if (data->on_off) {
					g_pdata->ir_wake_en(0);
					usleep_range(200, 300);
					g_pdata->ir_wake_en(1);
					msleep(30);
				} else {
					g_pdata->ir_vdd_onoff(1);
					g_pdata->ir_wake_en(1);
					msleep(60);
					data->on_off = 1;
				}
				data->i2c_block_transfer.data[2]
								= _data >> 16;
				data->i2c_block_transfer.data[3]
							= (_data >> 8) & 0xFF;
				data->i2c_block_transfer.data[4]
								= _data & 0xFF;
				data->count += 3;
			} else {
                _tdata = _data / converting_factor;
				data->ir_sum += _tdata;
				count = data->count;
				data->i2c_block_transfer.data[count]
								= _tdata >> 8;
				data->i2c_block_transfer.data[count+1]
								= _tdata & 0xFF;
				data->count += 2;
			}

			while (_data > 0) {
				buf++;
				_data /= 10;
			}
		} else {
			break;
		}
	}

	ir_remocon_work(data, data->count);
	return size;
}

static ssize_t remocon_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct barcode_emul_data *data = dev_get_drvdata(dev);
	int i;
	char *bufp = buf;

	for (i = 5; i < MAX_SIZE - 1; i++) {
		if (data->i2c_block_transfer.data[i] == 0
			&& data->i2c_block_transfer.data[i+1] == 0)
			break;
		else
			bufp += snprintf(bufp, sizeof(buf), "%u,",
					data->i2c_block_transfer.data[i]);
	}
	return strlen(buf);
}

/* sysfs node ir_send_result */
static ssize_t remocon_ack(struct device *dev, struct device_attribute *attr,
		char *buf)
{

	pr_barcode("%s : ack_number = %d\n", __func__, ack_number);

	if (ack_number == 6)
		return sprintf(buf, "1\n");
	else
		return sprintf(buf, "0\n");
}

static int irda_read_device_info(struct barcode_emul_data *ir_data)
{
	struct barcode_emul_data *data = ir_data;
	struct i2c_client *client = data->client;
	u8 buf_ir_test[8];
	int ret;

	pr_barcode("%s called\n", __func__);
	g_pdata->ir_vdd_onoff(1);
	g_pdata->ir_wake_en(1);
	msleep(60);

	fpga_enable(1);

	client->addr = IRDA_I2C_ADDR;
	ret = i2c_master_recv(client, buf_ir_test, MC96_READ_LENGTH);

	if (ret < 0)
		dev_err(&client->dev, "%s: err %d\n", __func__, ret);

	pr_barcode("%s: buf_ir dev_id: 0x%02x, 0x%02x\n", __func__,
			buf_ir_test[2], buf_ir_test[3]);
	ret = data->dev_id = (buf_ir_test[2] << 8 | buf_ir_test[3]);

	g_pdata->ir_wake_en(0);
	g_pdata->ir_vdd_onoff(0);
	data->on_off = 0;

	fpga_enable(0);

	return ret;
}

/* sysfs node check_ir */
static ssize_t check_ir_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	struct barcode_emul_data *data = dev_get_drvdata(dev);
	int ret;

	ret = irda_read_device_info(data);
	return snprintf(buf, 4, "%d\n", ret);
}

/* sysfs node irda_test */
static ssize_t irda_test_store(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	int ret, i;
	struct barcode_emul_data *data = dev_get_drvdata(dev);
	struct i2c_client *client = data->client;
	struct {
			unsigned char addr;
			unsigned char data[IRDA_TEST_CODE_SIZE];
		} i2c_block_transfer;

	unsigned char BSR_data[IRDA_TEST_CODE_SIZE] = {
		0x8D, 0x00, 0x96, 0x00, 0x01, 0x50, 0x00, 0xA8,
		0x00, 0x15, 0x00, 0x15, 0x00, 0x15, 0x00, 0x15,
		0x00, 0x15, 0x00, 0x3F, 0x00, 0x15, 0x00, 0x15,
		0x00, 0x15, 0x00, 0x15, 0x00, 0x15, 0x00, 0x15,
		0x00, 0x15, 0x00, 0x15, 0x00, 0x15, 0x00, 0x15,
		0x00, 0x15, 0x00, 0x3F, 0x00, 0x15, 0x00, 0x3F,
		0x00, 0x15, 0x00, 0x15, 0x00, 0x15, 0x00, 0x3F,
		0x00, 0x15, 0x00, 0x3F, 0x00, 0x15, 0x00, 0x3F,
		0x00, 0x15, 0x00, 0x3F, 0x00, 0x15, 0x00, 0x3F,
		0x00, 0x15, 0x00, 0x15, 0x00, 0x15, 0x00, 0x3F,
		0x00, 0x15, 0x00, 0x15, 0x00, 0x15, 0x00, 0x15,
		0x00, 0x15, 0x00, 0x15, 0x00, 0x15, 0x00, 0x15,
		0x00, 0x15, 0x00, 0x15, 0x00, 0x15, 0x00, 0x15,
		0x00, 0x15, 0x00, 0x3F, 0x00, 0x15, 0x00, 0x15,
		0x00, 0x15, 0x00, 0x3F, 0x00, 0x15, 0x00, 0x3F,
		0x00, 0x15, 0x00, 0x3F, 0x00, 0x15, 0x00, 0x3F,
		0x00, 0x15, 0x00, 0x3F, 0x00, 0x15, 0x00, 0x3F,
		0x00, 0x15, 0x00, 0x3F
	};

	if (check_fpga_cdone()) {
		pr_err("%s: cdone fail !!\n", __func__);
		return 1;
	}
	pr_barcode("IRDA test code start\n");

	/* change address for IRDA */
	client->addr = IRDA_I2C_ADDR;

	/* make data for sending */
	for (i = 0; i < IRDA_TEST_CODE_SIZE; i++)
		i2c_block_transfer.data[i] = BSR_data[i];

	fpga_enable(1);

	/* sending data by I2C */
	i2c_block_transfer.addr = IRDA_TEST_CODE_ADDR;
	ret = i2c_master_send(client, (unsigned char *) &i2c_block_transfer,
			IRDA_TEST_CODE_SIZE);
	if (ret < 0) {
		pr_err("%s: err1 %d\n", __func__, ret);
		ret = i2c_master_send(client,
		(unsigned char *) &i2c_block_transfer, IRDA_TEST_CODE_SIZE);
		if (ret < 0)
			pr_err("%s: err2 %d\n", __func__, ret);
	}

	fpga_enable(0);

	return size;
}

static ssize_t irda_test_show(struct device *dev, struct device_attribute *attr,
		char *buf)
{
	return strlen(buf);
}

static struct device_attribute ir_attrs[] = {
	__ATTR(check_ir, S_IRUGO|S_IWUSR|S_IWGRP, check_ir_show, NULL),
	__ATTR(ir_send, S_IRUGO|S_IWUSR|S_IWGRP, remocon_show, remocon_store),
	__ATTR(ir_send_result, S_IRUGO|S_IWUSR|S_IWGRP, remocon_ack, NULL),
	__ATTR(irda_test, S_IRUGO|S_IWUSR|S_IWGRP, irda_test_show, irda_test_store)
};
#endif
#ifndef CONFIG_NO_GPIO_EXPANDER_FPGA
static int ice_gpio_get(struct gpio_chip *chip, unsigned offset)
{
	return ice_gpiox_get((unsigned)offset);
}

static void ice_gpio_set(struct gpio_chip *chip, unsigned offset, int value)
{
	ice_gpiox_set((unsigned)offset, value);
}

static struct gpio_chip ice_gpio = {
	.label = "ice-fpga-gpio",
	.get = ice_gpio_get,
	.set = ice_gpio_set,
	.base = FPGA_GPIO_BASE,
	.ngpio = NR_FPGA_GPIO,
};

int ice_gpiox_get(int num)
{
	int ret;

	if (!fw_dl_complete)
		return -EBUSY;

	g_client->addr = BARCODE_I2C_ADDR;

	fpga_enable(1);
	ret = i2c_smbus_read_byte_data(g_client, num >> 3);
	if (ret) {
		gpio_set_value(g_pdata->rst_n, GPIO_LEVEL_LOW);
		udelay(5);
		gpio_set_value(g_pdata->rst_n, GPIO_LEVEL_HIGH);
		udelay(1);
		ret = i2c_smbus_read_byte_data(g_client, num >> 3);
	}

	fpga_enable(0);
	if (ret < 0) {
		pr_err("%s i2c error : %d\n", __func__, ret);
		return ret;
	} else if (ret & (1 << (num&7))) {
		pr_barcode("%s : num = %d , val = 1\n", __func__, num);
		return 1;
	} else {
		pr_barcode("%s : num = %d , val = 0\n", __func__, num);
		return 0;
	}

}
EXPORT_SYMBOL(ice_gpiox_get);

int ice_gpiox_set(int num, int val)
{
	int ret;

	if (!fw_dl_complete)
		return -EBUSY;

	g_client->addr = BARCODE_I2C_ADDR;
	if (val)
		gpiox_state |= 1 << num;
	else
		gpiox_state &= ~(1 << num);

	pr_barcode("%s : num = %d , val = %d\n", __func__, num, val);

	fpga_enable(1);
	ret = i2c_smbus_write_byte_data(g_client, num >> 3,
			(num >> 3) ? (gpiox_state >> 8) : gpiox_state);
	if (ret) {
		gpio_set_value(g_pdata->rst_n, GPIO_LEVEL_LOW);
		udelay(5);
		gpio_set_value(g_pdata->rst_n, GPIO_LEVEL_HIGH);
		udelay(1);
		ret = i2c_smbus_write_byte_data(g_client, num >> 3,
			(num >> 3) ? (gpiox_state >> 8) : gpiox_state);
	}

	fpga_enable(0);
	if (ret)
		pr_err("%s i2c error : %d\n", __func__, ret);

	return ret;
}
EXPORT_SYMBOL(ice_gpiox_set);
#endif
static void fw_work(struct work_struct *work)
{
	ice4_fpga_firmware_update();
	if (check_fpga_cdone()) {
		pr_err("%s: cdone fail !!\n", __func__);
		return;
	}

	/* set clock */
	if (!g_pdata->fw_type) {
		main_clk = clk_get(&barcode_mclk_dev.dev, "osr_clk");
		clk_set_rate(main_clk, 12288000);
	}
	Is_clk_enabled = 0;

	fpga_enable(1);
	fpga_enable(0);

}

static int __devinit barcode_emul_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct i2c_adapter *adapter = to_i2c_adapter(client->dev.parent);
	struct barcode_emul_data *data;
	struct barcode_emul_platform_data *pdata;
	struct device *barcode_emul_dev;
	int error;
#ifdef CONFIG_IR_REMOCON_FPGA
	int i;
#endif
	pr_barcode("%s probe!\n", __func__);
	
	pdata = client->dev.platform_data;
	g_pdata = pdata;
#ifdef CONFIG_IR_REMOCON_FPGA
	g_pdata->ir_remote_init();
#endif
	if (g_pdata->gpioconfig) {
		pr_barcode("%s setting gpio config.\n", __func__);
		g_pdata->gpioconfig();
	}

	if (g_pdata->poweron)
		g_pdata->poweron(1);

	if (!i2c_check_functionality(adapter, I2C_FUNC_I2C))
		return -EIO;

	data = kzalloc(sizeof(struct barcode_emul_data), GFP_KERNEL);
	if (NULL == data) {
		pr_err("Failed to data allocate %s\n", __func__);
		error = -ENOMEM;
		goto err_free_mem;
	}
	data->client = client;
	mutex_init(&en_mutex);
#ifdef CONFIG_IR_REMOCON_FPGA
	data->pdata = client->dev.platform_data;
	mutex_init(&data->mutex);
	data->count = 2;
	data->on_off = 0;
#endif
	i2c_set_clientdata(client, data);

	barcode_emul_dev = device_create(sec_class, NULL, 0,
				data, "sec_barcode_emul");

	if (IS_ERR(barcode_emul_dev))
		pr_err("Failed to create barcode_emul_dev device\n");

	if (device_create_file(barcode_emul_dev, &dev_attr_barcode_send) < 0)
		pr_err("Failed to create device file(%s)!\n",
				dev_attr_barcode_send.attr.name);
	if (device_create_file(barcode_emul_dev,
			&dev_attr_barcode_test_send) < 0)
		pr_err("Failed to create device file(%s)!\n",
				dev_attr_barcode_test_send.attr.name);
	if (device_create_file(barcode_emul_dev,
			&dev_attr_barcode_fw_update) < 0)
		pr_err("Failed to create device file(%s)!\n",
				dev_attr_barcode_fw_update.attr.name);
	if (device_create_file(barcode_emul_dev,
			&dev_attr_barcode_ver_check) < 0)
		pr_err("Failed to create device file(%s)!\n",
				dev_attr_barcode_ver_check.attr.name);
	if (device_create_file(barcode_emul_dev,
			&dev_attr_barcode_led_status) < 0)
		pr_err("Failed to create device file(%s)!\n",
				dev_attr_barcode_led_status.attr.name);
#if defined(CONFIG_IR_REMOCON_FPGA)
	barcode_emul_dev = device_create(sec_class, NULL, 0, data, "sec_ir");
	if (IS_ERR(barcode_emul_dev))
		pr_err("Failed to create barcode_emul_dev device in sec_ir\n");

	/* sysfs entries */
	for (i = 0; i < ARRAY_SIZE(ir_attrs); i++) {
		device_create_file(barcode_emul_dev,
					 &ir_attrs[i]);
	}
#endif

	/*Create dedicated thread so that
	 the delay of our work does not affect others*/
	data->firmware_dl =
		create_singlethread_workqueue("barcode_firmware_dl");
	INIT_DELAYED_WORK(&data->fw_dl, fw_work);
	/* min 1ms is needed */
	queue_delayed_work(data->firmware_dl,
			&data->fw_dl, msecs_to_jiffies(20));

	g_client = client;

	pr_err("probe complete %s\n", __func__);

	return 0;

err_free_mem:
	kfree(data);
	return error;
}

static int __devexit barcode_emul_remove(struct i2c_client *client)
{
	struct barcode_emul_data *data = i2c_get_clientdata(client);

	i2c_set_clientdata(client, NULL);
	clk_put(main_clk);
	kfree(data);
	return 0;
}

static const struct i2c_device_id ice4_id[] = {
	{"ice4", 0},
	{}
};
MODULE_DEVICE_TABLE(i2c, ice4_id);

static struct i2c_driver ice4_i2c_driver = {
	.driver = {
		.name = "ice4",
		.owner = THIS_MODULE,
	},
	.probe = barcode_emul_probe,
	.remove = __devexit_p(barcode_emul_remove),
	.id_table = ice4_id,
};

static int __init barcode_emul_init(void)
{
#ifndef CONFIG_NO_GPIO_EXPANDER_FPGA
	int ret;
	ret = gpiochip_add(&ice_gpio);
	if (ret) {
		pr_err("gpiochip_add failed ret = %d\n", ret);
		return ret;
	}
#endif
	return i2c_add_driver(&ice4_i2c_driver);
}
late_initcall(barcode_emul_init);

static void __exit barcode_emul_exit(void)
{
	i2c_del_driver(&ice4_i2c_driver);
}
module_exit(barcode_emul_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SEC Barcode emulator");

