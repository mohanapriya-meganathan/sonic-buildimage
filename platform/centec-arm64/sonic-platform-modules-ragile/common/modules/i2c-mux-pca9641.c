/*
 * I2C multiplexer driver for PCA9541 bus master selector
 *
 * Copyright (c) 2010 Ericsson AB.
 *
 * Author: Guenter Roeck <linux@roeck-us.net>
 *
 * Derived from:
 *  pca954x.c
 *
 *  Copyright (c) 2008-2009 Rodolfo Giometti <giometti@linux.it>
 *  Copyright (c) 2008-2009 Eurotech S.p.A. <info@eurotech.it>
 *
 * This file is licensed under the terms of the GNU General Public
 * License version 2. This program is licensed "as is" without any
 * warranty of any kind, whether express or implied.
 */

#include <linux/version.h>
#include <linux/module.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/i2c-mux.h>
#include <linux/bitops.h>

/*
 * The PCA9541 is a bus master selector. It supports two I2C masters connected
 * to a single slave bus.
 *
 * Before each bus transaction, a master has to acquire bus ownership. After the
 * transaction is complete, bus ownership has to be released. This fits well
 * into the I2C multiplexer framework, which provides select and release
 * functions for this purpose. For this reason, this driver is modeled as
 * single-channel I2C bus multiplexer.
 *
 * This driver assumes that the two bus masters are controlled by two different
 * hosts. If a single host controls both masters, platform code has to ensure
 * that only one of the masters is instantiated at any given time.
 */

#define PCA9541_CONTROL		0x01
#define PCA9541_ISTAT		0x02

#define PCA9541_CTL_MYBUS	(1 << 0)
#define PCA9541_CTL_NMYBUS	(1 << 1)
#define PCA9541_CTL_BUSON	(1 << 2)
#define PCA9541_CTL_NBUSON	(1 << 3)
#define PCA9541_CTL_BUSINIT	(1 << 4)
#define PCA9541_CTL_TESTON	(1 << 6)
#define PCA9541_CTL_NTESTON	(1 << 7)
#define PCA9541_ISTAT_INTIN	(1 << 0)
#define PCA9541_ISTAT_BUSINIT	(1 << 1)
#define PCA9541_ISTAT_BUSOK	(1 << 2)
#define PCA9541_ISTAT_BUSLOST	(1 << 3)
#define PCA9541_ISTAT_MYTEST	(1 << 6)
#define PCA9541_ISTAT_NMYTEST	(1 << 7)
#define PCA9641_ID             0x00
#define PCA9641_ID_MAGIC       0x38
#define PCA9641_CONTROL        0x01
#define PCA9641_STATUS         0x02
#define PCA9641_TIME           0x03
#define PCA9641_CTL_LOCK_REQ           BIT(0)
#define PCA9641_CTL_LOCK_GRANT         BIT(1)
#define PCA9641_CTL_BUS_CONNECT        BIT(2)
#define PCA9641_CTL_BUS_INIT           BIT(3)
#define PCA9641_CTL_SMBUS_SWRST        BIT(4)
#define PCA9641_CTL_IDLE_TIMER_DIS     BIT(5)
#define PCA9641_CTL_SMBUS_DIS          BIT(6)
#define PCA9641_CTL_PRIORITY           BIT(7)
#define PCA9641_STS_OTHER_LOCK         BIT(0)
#define PCA9641_STS_BUS_INIT_FAIL      BIT(1)
#define PCA9641_STS_BUS_HUNG           BIT(2)
#define PCA9641_STS_MBOX_EMPTY         BIT(3)
#define PCA9641_STS_MBOX_FULL          BIT(4)
#define PCA9641_STS_TEST_INT           BIT(5)
#define PCA9641_STS_SCL_IO             BIT(6)
#define PCA9641_STS_SDA_IO             BIT(7)
#define PCA9641_RES_TIME       0x03
#define BUSON		(PCA9541_CTL_BUSON | PCA9541_CTL_NBUSON)
#define MYBUS		(PCA9541_CTL_MYBUS | PCA9541_CTL_NMYBUS)
#define mybus(x)	(!((x) & MYBUS) || ((x) & MYBUS) == MYBUS)
#define busoff(x)	(!((x) & BUSON) || ((x) & BUSON) == BUSON)
#define BUSOFF(x, y)   (!((x) & PCA9641_CTL_LOCK_GRANT) && \
                       !((y) & PCA9641_STS_OTHER_LOCK))
#define other_lock(x)  ((x) & PCA9641_STS_OTHER_LOCK)
#define lock_grant(x)  ((x) & PCA9641_CTL_LOCK_GRANT)

#define PCA9641_RETRY_TIME 8

typedef struct i2c_muxs_struct_flag
{
	int nr;
	char name[48];
	struct mutex	update_lock;
	int flag;
}i2c_mux_flag;

i2c_mux_flag pca_flag = {
	.flag = -1,
};

int pca9641_setmuxflag(int nr, int flag)
{
	if (pca_flag.nr == nr) {
	    pca_flag.flag = flag;
	}
	return 0;
}
EXPORT_SYMBOL(pca9641_setmuxflag);

int g_debug = 0;
module_param(g_debug, int, S_IRUGO | S_IWUSR);

#define PCA_DEBUG(fmt, args...) do {                                        \
    if (g_debug) { \
        printk(KERN_ERR "[pca9641][VER][func:%s line:%d]\r\n"fmt, __func__, __LINE__, ## args); \
    } \
} while (0)

/* arbitration timeouts, in jiffies */
#define ARB_TIMEOUT	(HZ / 8)	/* 125 ms until forcing bus ownership */
#define ARB2_TIMEOUT	(HZ / 4)	/* 250 ms until acquisition failure */

/* arbitration retry delays, in us */
#define SELECT_DELAY_SHORT	50
#define SELECT_DELAY_LONG	1000

struct pca9541 {
	struct i2c_client *client;
	unsigned long select_timeout;
	unsigned long arb_timeout;
};

static const struct i2c_device_id pca9541_id[] = {
	{"pca9541", 0},
	{"pca9641", 1},
	{}
};

MODULE_DEVICE_TABLE(i2c, pca9541_id);

#ifdef CONFIG_OF
static const struct of_device_id pca9541_of_match[] = {
    { .compatible = "nxp,pca9541" },
    { .compatible = "nxp,pca9641" },
    {}
};
MODULE_DEVICE_TABLE(of, pca9541_of_match);
#endif

/*
 * Write to chip register. Don't use i2c_transfer()/i2c_smbus_xfer()
 * as they will try to lock the adapter a second time.
 */
static int pca9541_reg_write(struct i2c_client *client, u8 command, u8 val)
{
	struct i2c_adapter *adap = client->adapter;
	int ret;

	if (adap->algo->master_xfer) {
		struct i2c_msg msg;
		char buf[2];

		msg.addr = client->addr;
		msg.flags = 0;
		msg.len = 2;
		buf[0] = command;
		buf[1] = val;
		msg.buf = buf;
		ret = __i2c_transfer(adap, &msg, 1);
	} else {
		union i2c_smbus_data data;

		data.byte = val;
		ret = adap->algo->smbus_xfer(adap, client->addr,
					     client->flags,
					     I2C_SMBUS_WRITE,
					     command,
					     I2C_SMBUS_BYTE_DATA, &data);
	}

	return ret;
}

/*
 * Read from chip register. Don't use i2c_transfer()/i2c_smbus_xfer()
 * as they will try to lock adapter a second time.
 */
static int pca9541_reg_read(struct i2c_client *client, u8 command)
{
	struct i2c_adapter *adap = client->adapter;
	int ret;
	u8 val;

	if (adap->algo->master_xfer) {
		struct i2c_msg msg[2] = {
			{
				.addr = client->addr,
				.flags = 0,
				.len = 1,
				.buf = &command
			},
			{
				.addr = client->addr,
				.flags = I2C_M_RD,
				.len = 1,
				.buf = &val
			}
		};
		ret = __i2c_transfer(adap, msg, 2);
		if (ret == 2)
			ret = val;
		else if (ret >= 0)
			ret = -EIO;
	} else {
		union i2c_smbus_data data;

		ret = adap->algo->smbus_xfer(adap, client->addr,
					     client->flags,
					     I2C_SMBUS_READ,
					     command,
					     I2C_SMBUS_BYTE_DATA, &data);
		if (!ret)
			ret = data.byte;
	}
	return ret;
}

/*
 * Arbitration management functions
 */

/* Release bus. Also reset NTESTON and BUSINIT if it was set. */
static void pca9541_release_bus(struct i2c_client *client)
{
	int reg;

	reg = pca9541_reg_read(client, PCA9541_CONTROL);
	if (reg >= 0 && !busoff(reg) && mybus(reg))
		pca9541_reg_write(client, PCA9541_CONTROL,
				  (reg & PCA9541_CTL_NBUSON) >> 1);
}

/*
 * Arbitration is defined as a two-step process. A bus master can only activate
 * the slave bus if it owns it; otherwise it has to request ownership first.
 * This multi-step process ensures that access contention is resolved
 * gracefully.
 *
 * Bus	Ownership	Other master	Action
 * state		requested access
 * ----------------------------------------------------
 * off	-		yes		wait for arbitration timeout or
 *					for other master to drop request
 * off	no		no		take ownership
 * off	yes		no		turn on bus
 * on	yes		-		done
 * on	no		-		wait for arbitration timeout or
 *					for other master to release bus
 *
 * The main contention point occurs if the slave bus is off and both masters
 * request ownership at the same time. In this case, one master will turn on
 * the slave bus, believing that it owns it. The other master will request
 * bus ownership. Result is that the bus is turned on, and master which did
 * _not_ own the slave bus before ends up owning it.
 */

/* Control commands per PCA9541 datasheet */
static const u8 pca9541_control[16] = {
	4, 0, 1, 5, 4, 4, 5, 5, 0, 0, 1, 1, 0, 4, 5, 1
};

/*
 * Channel arbitration
 *
 * Return values:
 *  <0: error
 *  0 : bus not acquired
 *  1 : bus acquired
 */
static int pca9541_arbitrate(struct i2c_client *client)
{
	struct i2c_mux_core *muxc = i2c_get_clientdata(client);
	struct pca9541 *data = i2c_mux_priv(muxc);
	int reg;

	reg = pca9541_reg_read(client, PCA9541_CONTROL);
	if (reg < 0)
		return reg;

	if (busoff(reg)) {
		int istat;
		/*
		 * Bus is off. Request ownership or turn it on unless
		 * other master requested ownership.
		 */
		istat = pca9541_reg_read(client, PCA9541_ISTAT);
		if (!(istat & PCA9541_ISTAT_NMYTEST)
		    || time_is_before_eq_jiffies(data->arb_timeout)) {
			/*
			 * Other master did not request ownership,
			 * or arbitration timeout expired. Take the bus.
			 */
			pca9541_reg_write(client,
					  PCA9541_CONTROL,
					  pca9541_control[reg & 0x0f]
					  | PCA9541_CTL_NTESTON);
			data->select_timeout = SELECT_DELAY_SHORT;
		} else {
			/*
			 * Other master requested ownership.
			 * Set extra long timeout to give it time to acquire it.
			 */
			data->select_timeout = SELECT_DELAY_LONG * 2;
		}
	} else if (mybus(reg)) {
		/*
		 * Bus is on, and we own it. We are done with acquisition.
		 * Reset NTESTON and BUSINIT, then return success.
		 */
		if (reg & (PCA9541_CTL_NTESTON | PCA9541_CTL_BUSINIT))
			pca9541_reg_write(client,
					  PCA9541_CONTROL,
					  reg & ~(PCA9541_CTL_NTESTON
						  | PCA9541_CTL_BUSINIT));
		return 1;
	} else {
		/*
		 * Other master owns the bus.
		 * If arbitration timeout has expired, force ownership.
		 * Otherwise request it.
		 */
		data->select_timeout = SELECT_DELAY_LONG;
		if (time_is_before_eq_jiffies(data->arb_timeout)) {
			/* Time is up, take the bus and reset it. */
			pca9541_reg_write(client,
					  PCA9541_CONTROL,
					  pca9541_control[reg & 0x0f]
					  | PCA9541_CTL_BUSINIT
					  | PCA9541_CTL_NTESTON);
		} else {
			/* Request bus ownership if needed */
			if (!(reg & PCA9541_CTL_NTESTON))
				pca9541_reg_write(client,
						  PCA9541_CONTROL,
						  reg | PCA9541_CTL_NTESTON);
		}
	}
	return 0;
}

static int pca9541_select_chan(struct i2c_mux_core *muxc, u32 chan)
{
	struct pca9541 *data = i2c_mux_priv(muxc);
	struct i2c_client *client = data->client;
	int ret;
	unsigned long timeout = jiffies + ARB2_TIMEOUT;
		/* give up after this time */

	data->arb_timeout = jiffies + ARB_TIMEOUT;
		/* force bus ownership after this time */

	do {
		ret = pca9541_arbitrate(client);
		if (ret)
			return ret < 0 ? ret : 0;

		if (data->select_timeout == SELECT_DELAY_SHORT)
			udelay(data->select_timeout);
		else
			msleep(data->select_timeout / 1000);
	} while (time_is_after_eq_jiffies(timeout));

	return -ETIMEDOUT;
}

static int pca9541_release_chan(struct i2c_mux_core *muxc, u32 chan)
{
    struct pca9541 *data = i2c_mux_priv(muxc);
	struct i2c_client *client = data->client;
	pca9541_release_bus(client);
	return 0;
}

/*
* Arbitration management functions
*/
static void pca9641_release_bus(struct i2c_client *client)
{
   pca9541_reg_write(client, PCA9641_CONTROL, 0x80);  /* master 0x80 */
}

/*
* Channel arbitration
*
* Return values:
*  <0: error
*  0 : bus not acquired
*  1 : bus acquired
*/
static int pca9641_arbitrate(struct i2c_client *client)
{
	struct i2c_mux_core *muxc = i2c_get_clientdata(client);
	struct pca9541 *data = i2c_mux_priv(muxc);
   int reg_ctl, reg_sts;

   reg_ctl = pca9541_reg_read(client, PCA9641_CONTROL);
   if (reg_ctl < 0)
		   return reg_ctl;
   reg_sts = pca9541_reg_read(client, PCA9641_STATUS);

   if (BUSOFF(reg_ctl, reg_sts)) {
		   /*
			* Bus is off. Request ownership or turn it on unless
			* other master requested ownership.
			*/
		   reg_ctl |= PCA9641_CTL_LOCK_REQ;
		   pca9541_reg_write(client, PCA9641_CONTROL, reg_ctl);
		   reg_ctl = pca9541_reg_read(client, PCA9641_CONTROL);

		   if (lock_grant(reg_ctl)) {
				   /*
					* Other master did not request ownership,
					* or arbitration timeout expired. Take the bus.
					*/
				   reg_ctl |= PCA9641_CTL_BUS_CONNECT
								   | PCA9641_CTL_LOCK_REQ;
				   pca9541_reg_write(client, PCA9641_CONTROL, reg_ctl);
				   data->select_timeout = SELECT_DELAY_SHORT;

				   return 1;
		   } else {
				   /*
					* Other master requested ownership.
					* Set extra long timeout to give it time to acquire it.
					*/
				   data->select_timeout = SELECT_DELAY_LONG * 2;
		   }
   } else if (lock_grant(reg_ctl)) {
		   /*
			* Bus is on, and we own it. We are done with acquisition.
			*/
		   reg_ctl |= PCA9641_CTL_BUS_CONNECT | PCA9641_CTL_LOCK_REQ;
		   pca9541_reg_write(client, PCA9641_CONTROL, reg_ctl);

		   return 1;
   } else if (other_lock(reg_sts)) {
		   /*
			* Other master owns the bus.
			* If arbitration timeout has expired, force ownership.
			* Otherwise request it.
			*/
		   data->select_timeout = SELECT_DELAY_LONG;
		   reg_ctl |= PCA9641_CTL_LOCK_REQ;
		   pca9541_reg_write(client, PCA9641_CONTROL, reg_ctl);
   }
   return 0;
}

int pca9641_select_chan(struct i2c_mux_core *muxc, u32 chan)
{
	struct pca9541 *data = i2c_mux_priv(muxc);
    struct i2c_client *client = data->client;
    int ret;
    int result;
    unsigned long timeout = jiffies + ARB2_TIMEOUT;
    /* give up after this time */
    data->arb_timeout = jiffies + ARB_TIMEOUT;
    /* force bus ownership after this time */
   	for (result = 0 ; result < PCA9641_RETRY_TIME ; result ++) {
   	   do {
   		   ret = pca9641_arbitrate(client);
   		   if (ret == 1) {
   				return 0;
   			}
   		   if (data->select_timeout == SELECT_DELAY_SHORT)
   			   udelay(data->select_timeout);
   		   else
   			   msleep(data->select_timeout / 1000);
   	   } while (time_is_after_eq_jiffies(timeout));
       timeout = jiffies + ARB2_TIMEOUT;
   	}
    return -ETIMEDOUT;
}
EXPORT_SYMBOL(pca9641_select_chan);

static int pca9641_release_chan(struct i2c_mux_core *muxc, u32 chan)
{
	struct pca9541 *data = i2c_mux_priv(muxc);
	struct i2c_client *client = data->client;
	if (pca_flag.flag) {
		pca9641_release_bus(client);
	}
	return 0;
}

static int pca9641_detect_id(struct i2c_client *client)
{
   int reg;

   reg = pca9541_reg_read(client, PCA9641_ID);
   if (reg == PCA9641_ID_MAGIC)
           return 1;
   else
           return 0;
}

/**
 **  Limited: 20180827 supports one PCA9641
 **/
static int pca9641_recordflag(struct i2c_adapter *adap) {
    if (pca_flag.flag != -1) {
        pr_err(" %s %d has init already!!!", __func__, __LINE__);
        return -1 ;
    }
    pca_flag.nr = adap->nr;
	PCA_DEBUG(" adap->nr:%d\n",  adap->nr);
	snprintf(pca_flag.name, sizeof(pca_flag.name),adap->name);
    return 0;
}

static void i2c_lock_adapter(struct i2c_adapter *adapter){
    struct i2c_adapter *parent = i2c_parent_is_i2c_adapter(adapter);
    if (parent)
        i2c_lock_adapter(parent);
    else
        rt_mutex_lock(&adapter->bus_lock);
}

void i2c_unlock_adapter(struct i2c_adapter *adapter)
{
    struct i2c_adapter *parent = i2c_parent_is_i2c_adapter(adapter);

    if (parent)
        i2c_unlock_adapter(parent);
    else
        rt_mutex_unlock(&adapter->bus_lock);
}
/*
 * I2C init/probing/exit functions
 */
static int pca9541_probe(struct i2c_client *client,
			 const struct i2c_device_id *id)
{
	struct i2c_adapter *adap = client->adapter;
    struct i2c_mux_core *muxc;
	struct pca9541 *data;
	int force;
	int ret = -ENODEV;
    int detect_id;

	if (!i2c_check_functionality(adap, I2C_FUNC_SMBUS_BYTE_DATA))
		return -ENODEV;

    detect_id = pca9641_detect_id(client);

	/*
	 * I2C accesses are unprotected here.
	 * We have to lock the adapter before releasing the bus.
	 */
    #if 0
    i2c_lock_adapter(adap);
	pca9541_release_bus(client);
	i2c_unlock_adapter(adap);
    #endif
    if (detect_id == 0) {
        i2c_lock_adapter(adap);
        pca9541_release_bus(client);
        i2c_unlock_adapter(adap);
    } else {
        i2c_lock_adapter(adap);
        pca9641_release_bus(client);
        i2c_unlock_adapter(adap);
    }

	/* Create mux adapter */

    if (detect_id == 0) {
        muxc = i2c_mux_alloc(adap, &client->dev, 1, sizeof(*data),
    			     I2C_MUX_ARBITRATOR,
    			     pca9541_select_chan, pca9541_release_chan);
    	if (!muxc)
    		return -ENOMEM;

    	data = i2c_mux_priv(muxc);
    	data->client = client;

    	i2c_set_clientdata(client, muxc);
        ret = i2c_mux_add_adapter(muxc, 0, 0, 0);

    	if (ret)
    		return ret;
    } else {
    muxc = i2c_mux_alloc(adap, &client->dev, 1, sizeof(*data),
                 I2C_MUX_ARBITRATOR,
                 pca9641_select_chan, pca9641_release_chan);
    if (!muxc)
        return -ENOMEM;

    data = i2c_mux_priv(muxc);
    data->client = client;

    i2c_set_clientdata(client, muxc);

    ret = i2c_mux_add_adapter(muxc, force, 0, 0);
    if (ret)
        return ret;
    }
	pca9641_recordflag(muxc->adapter[0]);

	dev_info(&client->dev, "registered master selector for I2C %s\n",
		 client->name);

	return 0;

}

static int pca9541_remove(struct i2c_client *client)
{
	struct i2c_mux_core *muxc = i2c_get_clientdata(client);

	i2c_mux_del_adapters(muxc);
	return 0;
}

static struct i2c_driver pca9641_driver = {
	.driver = {
		   .name = "pca9641",
           .of_match_table = of_match_ptr(pca9541_of_match),
		   },
	.probe = pca9541_probe,
	.remove = pca9541_remove,
	.id_table = pca9541_id,
};

module_i2c_driver(pca9641_driver);

MODULE_AUTHOR("Guenter Roeck <linux@roeck-us.net>");
MODULE_DESCRIPTION("PCA9541 I2C master selector driver");
MODULE_LICENSE("GPL v2");
