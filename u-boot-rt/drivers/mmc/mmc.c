/*
 * Copyright 2008, Freescale Semiconductor, Inc
 * Andy Fleming
 *
 * Based vaguely on the Linux code
 *
 * SPDX-License-Identifier:	GPL-2.0+
 */

#include <config.h>
#include <common.h>
#include <command.h>
#include <errno.h>
#include <mmc.h>
#include <part.h>
#include <malloc.h>
#include <linux/list.h>
#include <div64.h>
#include <asm/arch/rtkemmc.h>

#define SUPPORT_HS200
#define SUPPORT_WRITE_PROT
//#define MMC_DEBUG
struct list_head mmc_devices;
int cur_dev_num = -1;
void mmc_set_mode_select(struct mmc *mmc, uint mode);
int mmc_select_hs200(struct mmc *mmc,char* ext_csd);

__weak int board_mmc_getwp(struct mmc *mmc)
{
	return -1;
}

int mmc_getwp(struct mmc *mmc)
{
	int wp;

	wp = board_mmc_getwp(mmc);

	if (wp < 0) {
#if 0
		if (mmc->cfg->ops->getwp)
			wp = mmc->cfg->ops->getwp(mmc);
		else
#endif
			wp = 0;
	}

	return wp;
}

__weak int board_mmc_getcd(struct mmc *mmc)
{
	return -1;
}

int mmc_send_cmd(struct mmc *mmc, struct mmc_cmd *cmd, struct mmc_data *data)
{
	int ret;

#ifdef CONFIG_MMC_TRACE
	int i;
	u8 *ptr;

	printf("CMD_SEND:%d\n", cmd->cmdidx);
	printf("\t\tARG\t\t\t 0x%08X\n", cmd->cmdarg);
	//ret = mmc->cfg->ops->send_cmd(mmc, cmd, data);
	ret = mmc->request(mmc, cmd, data);
	switch (cmd->resp_type) {
		case MMC_RSP_NONE:
			printf("\t\tMMC_RSP_NONE\n");
			break;
		case MMC_RSP_R1:
			printf("\t\tMMC_RSP_R1,5,6,7 \t 0x%08X \n",
				cmd->response[0]);
			break;
		case MMC_RSP_R1b:
			printf("\t\tMMC_RSP_R1b\t\t 0x%08X \n",
				cmd->response[0]);
			break;
		case MMC_RSP_R2:
			printf("\t\tMMC_RSP_R2\t\t 0x%08X \n",
				cmd->response[0]);
			printf("\t\t          \t\t 0x%08X \n",
				cmd->response[1]);
			printf("\t\t          \t\t 0x%08X \n",
				cmd->response[2]);
			printf("\t\t          \t\t 0x%08X \n",
				cmd->response[3]);
			printf("\n");
			printf("\t\t\t\t\tDUMPING DATA\n");
			for (i = 0; i < 4; i++) {
				int j;
				printf("\t\t\t\t\t%03d - ", i*4);
				ptr = (u8 *)&cmd->response[i];
				ptr += 3;
				for (j = 0; j < 4; j++)
					printf("%02X ", *ptr--);
				printf("\n");
			}
			break;
		case MMC_RSP_R3:
			printf("\t\tMMC_RSP_R3,4\t\t 0x%08X \n",
				cmd->response[0]);
			break;
		default:
			printf("\t\tERROR MMC rsp not supported\n");
			break;
	}
#else
	//ret = mmc->cfg->ops->send_cmd(mmc, cmd, data);
	ret = mmc->request(mmc, cmd, data);
#endif
	return ret;
}

int mmc_send_status(struct mmc *mmc, int timeout)
{
	struct mmc_cmd cmd;
	int err, retries = 5;
#ifdef CONFIG_MMC_TRACE
	int status;
#endif

	cmd.cmdidx = MMC_CMD_SEND_STATUS;
	cmd.resp_type = MMC_RSP_R1;
	if (!mmc_host_is_spi(mmc))
		cmd.cmdarg = mmc->rca << 16;

	while (1) {
		err = mmc_send_cmd(mmc, &cmd, NULL);
		if (!err) {
			if ((cmd.response[0] & MMC_STATUS_RDY_FOR_DATA) &&
			    (cmd.response[0] & MMC_STATUS_CURR_STATE) !=
			     MMC_STATE_PRG)
				break;
			else if (cmd.response[0] & MMC_STATUS_MASK) {
#if !defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_LIBCOMMON_SUPPORT)
				printf("Status Error: 0x%08X\n",
					cmd.response[0]);
#endif
				return COMM_ERR;
			}
		} else if (--retries < 0)
			return err;

		if (timeout-- <= 0)
			break;

		udelay(1000);
	}

#ifdef CONFIG_MMC_TRACE
	status = (cmd.response[0] & MMC_STATUS_CURR_STATE) >> 9;
	printf("CURR STATE:%d\n", status);
#endif
	if (timeout <= 0) {
#if !defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_LIBCOMMON_SUPPORT)
		printf("Timeout waiting card ready\n");
#endif
		return TIMEOUT;
	}
	if (cmd.response[0] & MMC_STATUS_SWITCH_ERROR)
		return SWITCH_ERR;

	return 0;
}

int mmc_set_blocklen(struct mmc *mmc, int len)
{
	return 0;	//in realtek platform, we do not need this command
	struct mmc_cmd cmd;

	if (mmc->ddr_mode)
		return 0;

	cmd.cmdidx = MMC_CMD_SET_BLOCKLEN;
	cmd.resp_type = MMC_RSP_R1;
	cmd.cmdarg = len;

	return mmc_send_cmd(mmc, &cmd, NULL);
}

struct mmc *find_mmc_device(int dev_num)
{
	struct mmc *m;
	struct list_head *entry;

	list_for_each(entry, &mmc_devices) {
		m = list_entry(entry, struct mmc, link);

		if (m->block_dev.dev == dev_num)
			return m;
	}

#if !defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_LIBCOMMON_SUPPORT)
	printf("MMC Device %d not found\n", dev_num);
#endif

	return NULL;
}

static int mmc_read_blocks(struct mmc *mmc, void *dst, lbaint_t start,
			   lbaint_t blkcnt)
{
	struct mmc_cmd cmd;
	struct mmc_data data;

	if (blkcnt > 1)
		cmd.cmdidx = MMC_CMD_READ_MULTIPLE_BLOCK;
	else
		cmd.cmdidx = MMC_CMD_READ_SINGLE_BLOCK;

/*	if (mmc->high_capacity)
		cmd.cmdarg = start;
	else
		cmd.cmdarg = start * mmc->read_bl_len;
*/
	cmd.cmdarg = start;
	cmd.resp_type = MMC_RSP_R1;

	data.dest = dst;
	data.blocks = blkcnt;
	data.blocksize = mmc->read_bl_len;
	data.flags = MMC_DATA_READ;

	if (mmc_send_cmd(mmc, &cmd, &data) != blkcnt) {
		printf("mmc read fail...\n");
		return 0;
	}
#ifndef CONFIG_SYS_RTK_EMMC_FLASH
	if (blkcnt > 1) {
		cmd.cmdidx = MMC_CMD_STOP_TRANSMISSION;
		cmd.cmdarg = 0;
		cmd.resp_type = MMC_RSP_R1b;
		if (mmc_send_cmd(mmc, &cmd, NULL)) {
#if !defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_LIBCOMMON_SUPPORT)
			printf("mmc fail to send stop cmd\n");
#endif
			return 0;
		}
	}
#endif
	return blkcnt;
}

static ulong mmc_bread(int dev_num, lbaint_t start, lbaint_t blkcnt, void *dst)
{
	lbaint_t cur, blocks_todo = blkcnt;

	if (blkcnt == 0)
		return 0;

	struct mmc *mmc = find_mmc_device(dev_num);
	if (!mmc)
		return 0;

	if ((start + blkcnt) > mmc->block_dev.lba) {
#if !defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_LIBCOMMON_SUPPORT)
		printf("MMC: block number 0x" LBAF " exceeds max(0x" LBAF ")\n",
			start + blkcnt, mmc->block_dev.lba);
#endif
		return 0;
	}

	if (mmc_set_blocklen(mmc, mmc->read_bl_len))
		return 0;

	do {
		//cur = (blocks_todo > mmc->cfg->b_max) ?
		//	mmc->cfg->b_max : blocks_todo;
		cur = (blocks_todo > mmc->b_max) ?
			mmc->b_max : blocks_todo;
		if(mmc_read_blocks(mmc, dst, start, cur) != cur)
			return 0;
		blocks_todo -= cur;
		start += cur;
		dst += cur * mmc->read_bl_len;
	} while (blocks_todo > 0);

	return blkcnt;
}

static int mmc_go_idle(struct mmc *mmc)
{
	struct mmc_cmd cmd;
	int err;

	udelay(1000);

	cmd.cmdidx = MMC_CMD_GO_IDLE_STATE;
	cmd.cmdarg = 0;
	cmd.resp_type = MMC_RSP_NONE;

	err = mmc_send_cmd(mmc, &cmd, NULL);

	if (err)
		return err;

	udelay(2000);

	return 0;
}

//*********************************************************************************
static ulong
mmc_write_blocks(struct mmc *mmc, ulong start, lbaint_t blkcnt, const void*src)
{
	struct mmc_cmd cmd;
	struct mmc_data data;
	int timeout = 1000;
//	int err=0;
	MY_CLR_ALIGN_BUFFER();
//	MY_ALLOC_CACHE_ALIGN_BUFFER(char, ext_csd, CSD_ARRAY_SIZE);
//	printf("Unused variable ext_csd = %s\n",ext_csd);

	if ((start + blkcnt) > mmc->block_dev.lba) {
		printf("MMC: block number 0x%lx exceeds max(0x%lx)\n",
			start + blkcnt, mmc->block_dev.lba);
		return 0;
	}

	if (blkcnt > 1)
		cmd.cmdidx = MMC_CMD_WRITE_MULTIPLE_BLOCK;
	else
		cmd.cmdidx = MMC_CMD_WRITE_SINGLE_BLOCK;

/*	if (mmc->high_capacity)
		cmd.cmdarg = start;
	else
		cmd.cmdarg = start * mmc->write_bl_len;
*/
	cmd.cmdarg = start;
	cmd.resp_type = MMC_RSP_R1;
	data.src = src;
	data.blocks = blkcnt;
	data.blocksize = mmc->write_bl_len;
	data.flags = MMC_DATA_WRITE;

	if (mmc_send_cmd(mmc, &cmd, &data) != blkcnt) {
		printf("mmc write failed\n");
		return 0;
	}
#ifndef CONFIG_SYS_RTK_EMMC_FLASH	//in realtek eMMC IP, hardware will send the stop command
	/* SPI multiblock writes terminate using a special
	 * token, not a STOP_TRANSMISSION request.
	 */
	#ifndef CONFIG_SYS_RTK_EMMC_FLASH
	if (!mmc_host_is_spi(mmc) && blkcnt > 1) {
		cmd.cmdidx = MMC_CMD_STOP_TRANSMISSION;
		cmd.cmdarg = 0;
		cmd.resp_type = MMC_RSP_R1b;
		if (mmc_send_cmd(mmc, &cmd, NULL)) {
			printf("mmc fail to send stop cmd\n");
			return 0;
		}
	}
	#endif
#endif
	/* Waiting for the ready status */
	if (mmc_send_status(mmc, timeout))
		return 0;

	return blkcnt;
}

static ulong
mmc_bwrite(int dev_num, ulong start, lbaint_t blkcnt, const void*src)
{
	lbaint_t cur, blocks_todo = blkcnt;

	struct mmc *mmc = find_mmc_device(dev_num);
	if (!mmc)
		return 0;

	if (mmc_set_blocklen(mmc, mmc->write_bl_len))
		return 0;

	do {
		cur = (blocks_todo > mmc->b_max) ?  mmc->b_max : blocks_todo;
		if(mmc_write_blocks(mmc, start, cur, src) != cur)
			return 0;
		blocks_todo -= cur;
		start += cur;
		src += cur * mmc->write_bl_len;
	} while (blocks_todo > 0);

	return blkcnt;
}

static ulong mmc_erase_t(struct mmc *mmc, ulong start, lbaint_t blkcnt)
{
	struct mmc_cmd cmd;
	ulong end;
	int err, start_cmd, end_cmd;
#if 0
	if (mmc->high_capacity)
		end = start + blkcnt - 1;
	else {
		end = (start + blkcnt - 1) * mmc->write_bl_len;
		start *= mmc->write_bl_len;
	}
#endif
	end = start + blkcnt - 1;
	if (IS_SD(mmc)) {
		start_cmd = SD_CMD_ERASE_WR_BLK_START;
		end_cmd = SD_CMD_ERASE_WR_BLK_END;
	} else {
		start_cmd = MMC_CMD_ERASE_GROUP_START;
		end_cmd = MMC_CMD_ERASE_GROUP_END;
	}
	cmd.cmdidx = start_cmd;
	cmd.cmdarg = start;
	cmd.resp_type = MMC_RSP_R1;

	err = mmc_send_cmd(mmc, &cmd, NULL);
	if (err)
		goto err_out;
	cmd.cmdidx = end_cmd;
	cmd.cmdarg = end;

	err = mmc_send_cmd(mmc, &cmd, NULL);
	if (err)
		goto err_out;
	cmd.cmdidx = MMC_CMD_ERASE;
	cmd.cmdarg = SECURE_ERASE;
	cmd.resp_type = MMC_RSP_R1B;

	err = mmc_send_cmd(mmc, &cmd, NULL);
	if (err)
		goto err_out;

	return 0;

err_out:
	puts("mmc erase failed\n");
	return err;
}


static unsigned long
mmc_berase(int dev_num, unsigned long start, lbaint_t blkcnt)
{
	int err = 0;
	struct mmc *mmc = find_mmc_device(dev_num);
	lbaint_t blk = 0, blk_r = 0;
	int timeout = 1000;

	if (!mmc)
		return -1;

	if ((start % mmc->erase_grp_size) || (blkcnt % mmc->erase_grp_size))
		printf("\n\nCaution! Your devices Erase group is 0x%x\n"
			"The erase range would be change to 0x%lx~0x%lx * <n-th group>\n\n",
		       mmc->erase_grp_size, start & ~(mmc->erase_grp_size - 1),
		       ((start + blkcnt + mmc->erase_grp_size)
		       & ~(mmc->erase_grp_size - 1)) - 1);

	while (blk < blkcnt) {
		blk_r = ((blkcnt - blk) > mmc->erase_grp_size) ?
			mmc->erase_grp_size : (blkcnt - blk);
		err = mmc_erase_t(mmc, start + blk, blk_r);
		if (err)
			break;

		blk += blk_r;

		/* Waiting for the ready status */
		if (mmc_send_status(mmc, timeout))
			return 0;
	}

	return blk;
}
//*********************************************************************************


static int mmc_send_op_cond_iter(struct mmc *mmc, int use_arg)
{
	struct mmc_cmd cmd;
	int err;

	cmd.cmdidx = MMC_CMD_SEND_OP_COND;
	cmd.resp_type = MMC_RSP_R3;
	cmd.cmdarg = 0;
	if (use_arg && !mmc_host_is_spi(mmc))
		cmd.cmdarg = OCR_HCS |
			(mmc->voltages &
			(mmc->ocr & OCR_VOLTAGE_MASK)) |
			(mmc->ocr & OCR_ACCESS_MODE);
			//(mmc->cfg->voltages &
			//(mmc->ocr & OCR_VOLTAGE_MASK)) |
			//(mmc->ocr & OCR_ACCESS_MODE);

	err = mmc_send_cmd(mmc, &cmd, NULL);
	if (err)
		return err;
	mmc->ocr = cmd.response[0];
	return 0;
}

static int mmc_send_op_cond(struct mmc *mmc)
{
	int err, i;

	/* Some cards seem to need this */
	mmc_go_idle(mmc);

 	/* Asking to the card its capabilities */
	for (i = 0; i < 2; i++) {
		err = mmc_send_op_cond_iter(mmc, i != 0);
		if (err)
			return err;

		/* exit if not busy (flag seems to be inverted) */
		if (mmc->ocr & OCR_BUSY)
			break;
	}
	mmc->op_cond_pending = 1;
	return 0;
}


static int mmc_complete_op_cond(struct mmc *mmc)
{
	struct mmc_cmd cmd;
	int timeout = 1000;
	uint start;
	int err;

	mmc->op_cond_pending = 0;
	if (!(mmc->ocr & OCR_BUSY)) {
		start = get_timer(0);
		while (1) {
			err = mmc_send_op_cond_iter(mmc, 1);
			if (err)
				return err;
			if (mmc->ocr & OCR_BUSY)
				break;
			if (get_timer(start) > timeout)
				return UNUSABLE_ERR;
			udelay(100);
		}
	}

	if (mmc_host_is_spi(mmc)) { /* read OCR for spi */
		cmd.cmdidx = MMC_CMD_SPI_READ_OCR;
		cmd.resp_type = MMC_RSP_R3;
		cmd.cmdarg = 0;

		err = mmc_send_cmd(mmc, &cmd, NULL);

		if (err)
			return err;

		mmc->ocr = cmd.response[0];
	}

	mmc->version = MMC_VERSION_UNKNOWN;
#if 0
	mmc->high_capacity = ((mmc->ocr & OCR_HCS) == OCR_HCS);
	mmc->rca = 1;
#else
	mmc->high_capacity = 0;		//force byte mode on rtk 1295 platform
	mmc->rca = 0;
#endif
	return 0;
}

static int mmc_send_ext_csd(struct mmc *mmc, u8 *ext_csd)
{
	struct mmc_cmd cmd;
	struct mmc_data data;
	int err;

	/* Get the Card Status Register */
	cmd.cmdidx = MMC_CMD_SEND_EXT_CSD;
	cmd.resp_type = MMC_RSP_R1;
	cmd.cmdarg = 0;

	data.dest = (char *)ext_csd;
	data.blocks = 1;
	data.blocksize = MMC_MAX_BLOCK_LEN;
	data.flags = MMC_DATA_READ;

	err = mmc_send_cmd(mmc, &cmd, &data);
#ifdef MMC_DEBUG
	flush_cache((unsigned long)ext_csd, CSD_ARRAY_SIZE);
	mmc_show_ext_csd(ext_csd);
#endif
	return err;
}


static int mmc_switch(struct mmc *mmc, u8 set, u8 index, u8 value)
{
	struct mmc_cmd cmd;
	int timeout = 1000;
	int ret;

	cmd.cmdidx = MMC_CMD_SWITCH;
	cmd.resp_type = MMC_RSP_R1b;
	cmd.cmdarg = (MMC_SWITCH_MODE_WRITE_BYTE << 24) |
				 (index << 16) |
				 (value << 8)| set;

	ret = mmc_send_cmd(mmc, &cmd, NULL);

	/* Waiting for the ready status */
	if (!ret)
		ret = mmc_send_status(mmc, timeout);

	return ret;

}
#if 0
static int mmc_change_freq(struct mmc *mmc)
{
	ALLOC_CACHE_ALIGN_BUFFER(u8, ext_csd, MMC_MAX_BLOCK_LEN);
	char cardtype;
	int err;

	mmc->card_caps = 0;

	if (mmc_host_is_spi(mmc))
		return 0;

	/* Only version 4 supports high-speed */
	if (mmc->version < MMC_VERSION_4)
		return 0;

	mmc->card_caps |= MMC_MODE_4BIT | MMC_MODE_8BIT;

	err = mmc_send_ext_csd(mmc, ext_csd);

	if (err)
		return err;

	cardtype = ext_csd[EXT_CSD_CARD_TYPE] & 0xf;

	err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_HS_TIMING, 1);

	if (err)
		return err == SWITCH_ERR ? 0 : err;

	/* Now check to see that it worked */
	err = mmc_send_ext_csd(mmc, ext_csd);

	if (err)
		return err;

	/* No high-speed support */
	if (!ext_csd[EXT_CSD_HS_TIMING])
		return 0;

	/* High Speed is set, there are two types: 52MHz and 26MHz */
	if (cardtype & EXT_CSD_CARD_TYPE_52) {
		if (cardtype & EXT_CSD_CARD_TYPE_DDR_1_8V)
			mmc->card_caps |= MMC_MODE_DDR_52MHz;
		mmc->card_caps |= MMC_MODE_HS_52MHz | MMC_MODE_HS;
	} else {
		mmc->card_caps |= MMC_MODE_HS;
	}

	return 0;
}
#endif

int mmc_change_freq(struct mmc *mmc,unsigned int mode,unsigned int chg_type)
{
        MY_CLR_ALIGN_BUFFER();
        int err=0;
        extern unsigned int gCurrentBootMode;

        if (mmc_host_is_spi(mmc))
                return -1;

        /* Only version 4 supports high-speed */
        if (mmc->version < MMC_VERSION_4)
                return -1;

        switch(mode)
        {
                case MODE_SD20:
                        gCurrentBootMode = MODE_SD20;
                        if (chg_type & CHANGE_FREQ_CARD)
                                err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_HS_TIMING, EXT_CSD_TIMING_HS);
                        if (chg_type & CHANGE_FREQ_HOST)
                                mmc_set_mode_select(mmc,MMC_MODE_HS_52MHz);
                        sync();
                        break;
                case MODE_DDR:
                        gCurrentBootMode = MODE_DDR;
                        if (chg_type & CHANGE_FREQ_CARD)
                        {
                                err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_HS_TIMING, EXT_CSD_TIMING_HS);
                        }
                        if (chg_type & CHANGE_FREQ_HOST)
                                mmc_set_mode_select(mmc,MMC_MODE_HSDDR_52MHz);
                        sync();
                        break;
                case MODE_SD30:
                        gCurrentBootMode = MODE_SD30;
                        if (chg_type & CHANGE_FREQ_CARD)
                                err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_HS_TIMING, EXT_CSD_TIMING_HS200);
                        if (chg_type & CHANGE_FREQ_HOST)
                                mmc_set_mode_select(mmc,MMC_MODE_HS200);
                        sync();
                        break;
        }

        if (err)
                return err;
        return 0;
}

int mmc_get_card_caps(struct mmc *mmc, char *crd_ext_csd)
{
	char cardtype;

	cardtype = crd_ext_csd[EXT_CSD_CARD_TYPE] & 0xff;

	#ifdef MMC_DEBUG
	printf("[LY] cardtype=%02x\n",cardtype);
	#endif

	if (cardtype == 0)
	{
		mmc->card_caps = 0;
		printf("cardtype is empty, set sdr/ddr as default\n");
		mmc->card_caps |= MMC_MODE_HS;
		mmc->card_caps |= MMC_MODE_HS_52MHz;
		mmc->card_caps |= MMC_MODE_HSDDR_52MHz;
		if (!((((mmc->cid[0] >> 24)&0xff)== MANU_ID_MICRON1) || (((mmc->cid[0] >> 24)&0xff)== MANU_ID_MICRON2)))
			mmc->card_caps |= MMC_MODE_HS200;
	}
	else
	{
		/* High Speed is set, there are two types: 52MHz and 26MHz */
		mmc->card_caps = 0;
		if (cardtype & MMC_HS_26MHZ)
			mmc->card_caps |= MMC_MODE_HS;
		if (cardtype & MMC_HS_52MHZ)
			mmc->card_caps |= MMC_MODE_HS_52MHz;
		if (cardtype & MMC_HS_DDR_1_8V_52MHZ)
			mmc->card_caps |= MMC_MODE_HSDDR_52MHz;
		if (cardtype & MMC_HS_200_1_8V_52MHZ)
			mmc->card_caps |= MMC_MODE_HS200;
	}
	printf("[LY] cardtype=%02x, mmc->card_caps=%02x\n",cardtype,mmc->card_caps);
	return 0;
}

static int mmc_set_capacity(struct mmc *mmc, int part_num)
{
	switch (part_num) {
	case 0:
		mmc->capacity = mmc->capacity_user;
		break;
	case 1:
	case 2:
		mmc->capacity = mmc->capacity_boot;
		break;
	case 3:
		mmc->capacity = mmc->capacity_rpmb;
		break;
	case 4:
	case 5:
	case 6:
	case 7:
		mmc->capacity = mmc->capacity_gp[part_num - 4];
		break;
	default:
		return -1;
	}

	mmc->block_dev.lba = lldiv(mmc->capacity, mmc->read_bl_len);

	return 0;
}

#ifdef CONFIG_PARTITIONS
int mmc_select_hwpart(int dev_num, int hwpart)
{
	struct mmc *mmc = find_mmc_device(dev_num);
	int ret;

	if (!mmc)
		return -ENODEV;

	if (mmc->part_num == hwpart)
		return 0;

	if (mmc->part_config == MMCPART_NOAVAILABLE) {
		printf("Card doesn't support part_switch\n");
		return -EMEDIUMTYPE;
	}

	ret = mmc_switch_part(dev_num, hwpart);
	if (ret)
		return ret;

	mmc->part_num = hwpart;

	return 0;
}
#endif //CONFIG_PARTITIONS

int mmc_switch_part(int dev_num, unsigned int part_num)
{
	struct mmc *mmc = find_mmc_device(dev_num);
	int ret;

	if (!mmc)
		return -1;

	ret = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_PART_CONF,
			 (mmc->part_config & ~PART_ACCESS_MASK)
			 | (part_num & PART_ACCESS_MASK));

	/*
	 * Set the capacity if the switch succeeded or was intended
	 * to return to representing the raw device.
	 */
	if ((ret == 0) || ((ret == -ENODEV) && (part_num == 0)))
		ret = mmc_set_capacity(mmc, part_num);

	return ret;
}

int mmc_hwpart_config(struct mmc *mmc,
		      const struct mmc_hwpart_conf *conf,
		      enum mmc_hwpart_conf_mode mode)
{
	u8 part_attrs = 0;
	u32 enh_size_mult;
	u32 enh_start_addr;
	u32 gp_size_mult[4];
	u32 max_enh_size_mult;
	u32 tot_enh_size_mult = 0;
	u8 wr_rel_set;
	int i, pidx, err;
	ALLOC_CACHE_ALIGN_BUFFER(u8, ext_csd, MMC_MAX_BLOCK_LEN);

	if (mode < MMC_HWPART_CONF_CHECK || mode > MMC_HWPART_CONF_COMPLETE)
		return -EINVAL;

	if (IS_SD(mmc) || (mmc->version < MMC_VERSION_4_41)) {
		printf("eMMC >= 4.4 required for enhanced user data area\n");
		return -EMEDIUMTYPE;
	}

	if (!(mmc->part_support & PART_SUPPORT)) {
		printf("Card does not support partitioning\n");
		return -EMEDIUMTYPE;
	}

	if (!mmc->hc_wp_grp_size) {
		printf("Card does not define HC WP group size\n");
		return -EMEDIUMTYPE;
	}

	/* check partition alignment and total enhanced size */
	if (conf->user.enh_size) {
		if (conf->user.enh_size % mmc->hc_wp_grp_size ||
		    conf->user.enh_start % mmc->hc_wp_grp_size) {
			printf("User data enhanced area not HC WP group "
			       "size aligned\n");
			return -EINVAL;
		}
		part_attrs |= EXT_CSD_ENH_USR;
		enh_size_mult = conf->user.enh_size / mmc->hc_wp_grp_size;
		if (mmc->high_capacity) {
			enh_start_addr = conf->user.enh_start;
		} else {
			enh_start_addr = (conf->user.enh_start << 9);
		}
	} else {
		enh_size_mult = 0;
		enh_start_addr = 0;
	}
	tot_enh_size_mult += enh_size_mult;

	for (pidx = 0; pidx < 4; pidx++) {
		if (conf->gp_part[pidx].size % mmc->hc_wp_grp_size) {
			printf("GP%i partition not HC WP group size "
			       "aligned\n", pidx+1);
			return -EINVAL;
		}
		gp_size_mult[pidx] = conf->gp_part[pidx].size / mmc->hc_wp_grp_size;
		if (conf->gp_part[pidx].size && conf->gp_part[pidx].enhanced) {
			part_attrs |= EXT_CSD_ENH_GP(pidx);
			tot_enh_size_mult += gp_size_mult[pidx];
		}
	}

	if (part_attrs && ! (mmc->part_support & ENHNCD_SUPPORT)) {
		printf("Card does not support enhanced attribute\n");
		return -EMEDIUMTYPE;
	}

	err = mmc_send_ext_csd(mmc, ext_csd);
	if (err)
		return err;

	max_enh_size_mult =
		(ext_csd[EXT_CSD_MAX_ENH_SIZE_MULT+2] << 16) +
		(ext_csd[EXT_CSD_MAX_ENH_SIZE_MULT+1] << 8) +
		ext_csd[EXT_CSD_MAX_ENH_SIZE_MULT];
	if (tot_enh_size_mult > max_enh_size_mult) {
		printf("Total enhanced size exceeds maximum (%u > %u)\n",
		       tot_enh_size_mult, max_enh_size_mult);
		return -EMEDIUMTYPE;
	}

	/* The default value of EXT_CSD_WR_REL_SET is device
	 * dependent, the values can only be changed if the
	 * EXT_CSD_HS_CTRL_REL bit is set. The values can be
	 * changed only once and before partitioning is completed. */
	wr_rel_set = ext_csd[EXT_CSD_WR_REL_SET];
	if (conf->user.wr_rel_change) {
		if (conf->user.wr_rel_set)
			wr_rel_set |= EXT_CSD_WR_DATA_REL_USR;
		else
			wr_rel_set &= ~EXT_CSD_WR_DATA_REL_USR;
	}
	for (pidx = 0; pidx < 4; pidx++) {
		if (conf->gp_part[pidx].wr_rel_change) {
			if (conf->gp_part[pidx].wr_rel_set)
				wr_rel_set |= EXT_CSD_WR_DATA_REL_GP(pidx);
			else
				wr_rel_set &= ~EXT_CSD_WR_DATA_REL_GP(pidx);
		}
	}

	if (wr_rel_set != ext_csd[EXT_CSD_WR_REL_SET] &&
	    !(ext_csd[EXT_CSD_WR_REL_PARAM] & EXT_CSD_HS_CTRL_REL)) {
		puts("Card does not support host controlled partition write "
		     "reliability settings\n");
		return -EMEDIUMTYPE;
	}

	if (ext_csd[EXT_CSD_PARTITION_SETTING] &
	    EXT_CSD_PARTITION_SETTING_COMPLETED) {
		printf("Card already partitioned\n");
		return -EPERM;
	}

	if (mode == MMC_HWPART_CONF_CHECK)
		return 0;

	/* Partitioning requires high-capacity size definitions */
	if (!(ext_csd[EXT_CSD_ERASE_GROUP_DEF] & 0x01)) {
		err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_ERASE_GROUP_DEF, 1);

		if (err)
			return err;

		ext_csd[EXT_CSD_ERASE_GROUP_DEF] = 1;

		/* update erase group size to be high-capacity */
		mmc->erase_grp_size =
			ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE] * 1024;

	}

	/* all OK, write the configuration */
	for (i = 0; i < 4; i++) {
		err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_ENH_START_ADDR+i,
				 (enh_start_addr >> (i*8)) & 0xFF);
		if (err)
			return err;
	}
	for (i = 0; i < 3; i++) {
		err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_ENH_SIZE_MULT+i,
				 (enh_size_mult >> (i*8)) & 0xFF);
		if (err)
			return err;
	}
	for (pidx = 0; pidx < 4; pidx++) {
		for (i = 0; i < 3; i++) {
			err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
					 EXT_CSD_GP_SIZE_MULT+pidx*3+i,
					 (gp_size_mult[pidx] >> (i*8)) & 0xFF);
			if (err)
				return err;
		}
	}
	err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
			 EXT_CSD_PARTITIONS_ATTRIBUTE, part_attrs);
	if (err)
		return err;

	if (mode == MMC_HWPART_CONF_SET)
		return 0;

	/* The WR_REL_SET is a write-once register but shall be
	 * written before setting PART_SETTING_COMPLETED. As it is
	 * write-once we can only write it when completing the
	 * partitioning. */
	if (wr_rel_set != ext_csd[EXT_CSD_WR_REL_SET]) {
		err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
				 EXT_CSD_WR_REL_SET, wr_rel_set);
		if (err)
			return err;
	}

	/* Setting PART_SETTING_COMPLETED confirms the partition
	 * configuration but it only becomes effective after power
	 * cycle, so we do not adjust the partition related settings
	 * in the mmc struct. */

	err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
			 EXT_CSD_PARTITION_SETTING,
			 EXT_CSD_PARTITION_SETTING_COMPLETED);
	if (err)
		return err;

	return 0;
}

int mmc_getcd(struct mmc *mmc)
{
	int cd;

	cd = board_mmc_getcd(mmc);

	if (cd < 0) {
		//if (mmc->cfg->ops->getcd)
			//cd = mmc->cfg->ops->getcd(mmc);
		if (mmc->getcd)
			cd = mmc->getcd(mmc);
		else
			cd = 1;
	}

	return cd;
}

static int sd_switch(struct mmc *mmc, int mode, int group, u8 value, u8 *resp)
{
	struct mmc_cmd cmd;
	struct mmc_data data;

	/* Switch the frequency */
	cmd.cmdidx = SD_CMD_SWITCH_FUNC;
	cmd.resp_type = MMC_RSP_R1;
	cmd.cmdarg = (mode << 31) | 0xffffff;
	cmd.cmdarg &= ~(0xf << (group * 4));
	cmd.cmdarg |= value << (group * 4);

	data.dest = (char *)resp;
	data.blocksize = 64;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;

	return mmc_send_cmd(mmc, &cmd, &data);
}


static int sd_change_freq(struct mmc *mmc)
{
	int err;
	struct mmc_cmd cmd;
	ALLOC_CACHE_ALIGN_BUFFER(uint, scr, 2);
	ALLOC_CACHE_ALIGN_BUFFER(uint, switch_status, 16);
	struct mmc_data data;
	int timeout;

	mmc->card_caps = 0;

	if (mmc_host_is_spi(mmc))
		return 0;

	/* Read the SCR to find out if this card supports higher speeds */
	cmd.cmdidx = MMC_CMD_APP_CMD;
	cmd.resp_type = MMC_RSP_R1;
	cmd.cmdarg = mmc->rca << 16;

	err = mmc_send_cmd(mmc, &cmd, NULL);

	if (err)
		return err;

	cmd.cmdidx = SD_CMD_APP_SEND_SCR;
	cmd.resp_type = MMC_RSP_R1;
	cmd.cmdarg = 0;

	timeout = 3;

retry_scr:
	data.dest = (char *)scr;
	data.blocksize = 8;
	data.blocks = 1;
	data.flags = MMC_DATA_READ;

	err = mmc_send_cmd(mmc, &cmd, &data);

	if (err) {
		if (timeout--)
			goto retry_scr;

		return err;
	}

	mmc->scr[0] = __be32_to_cpu(scr[0]);
	mmc->scr[1] = __be32_to_cpu(scr[1]);

	switch ((mmc->scr[0] >> 24) & 0xf) {
		case 0:
			mmc->version = SD_VERSION_1_0;
			break;
		case 1:
			mmc->version = SD_VERSION_1_10;
			break;
		case 2:
			mmc->version = SD_VERSION_2;
			if ((mmc->scr[0] >> 15) & 0x1)
				mmc->version = SD_VERSION_3;
			break;
		default:
			mmc->version = SD_VERSION_1_0;
			break;
	}

	if (mmc->scr[0] & SD_DATA_4BIT)
		mmc->card_caps |= MMC_MODE_4BIT;

	/* Version 1.0 doesn't support switching */
	if (mmc->version == SD_VERSION_1_0)
		return 0;

	timeout = 4;
	while (timeout--) {
		err = sd_switch(mmc, SD_SWITCH_CHECK, 0, 1,
				(u8 *)switch_status);

		if (err)
			return err;

		/* The high-speed function is busy.  Try again */
		if (!(__be32_to_cpu(switch_status[7]) & SD_HIGHSPEED_BUSY))
			break;
	}

	/* If high-speed isn't supported, we return */
	if (!(__be32_to_cpu(switch_status[3]) & SD_HIGHSPEED_SUPPORTED))
		return 0;

	/*
	 * If the host doesn't support SD_HIGHSPEED, do not switch card to
	 * HIGHSPEED mode even if the card support SD_HIGHSPPED.
	 * This can avoid furthur problem when the card runs in different
	 * mode between the host.
	 */
	//if (!((mmc->cfg->host_caps & MMC_MODE_HS_52MHz) &&
	//	(mmc->cfg->host_caps & MMC_MODE_HS)))
	if (!((mmc->host_caps & MMC_MODE_HS_52MHz) &&
		(mmc->host_caps & MMC_MODE_HS)))
		return 0;

	err = sd_switch(mmc, SD_SWITCH_SWITCH, 0, 1, (u8 *)switch_status);

	if (err)
		return err;

	if ((__be32_to_cpu(switch_status[4]) & 0x0f000000) == 0x01000000)
		mmc->card_caps |= MMC_MODE_HS;

	return 0;
}

/* frequency bases */
/* divided by 10 to be nice to platforms without floating point */
static const int fbase[] = {
	10000,
	100000,
	1000000,
	10000000,
};

/* Multiplier values for TRAN_SPEED.  Multiplied by 10 to be nice
 * to platforms without floating point.
 */
static const int multipliers[] = {
	0,	/* reserved */
	10,
	12,
	13,
	15,
	20,
	25,
	30,
	35,
	40,
	45,
	50,
	55,
	60,
	70,
	80,
};

static void mmc_set_ios(struct mmc *mmc, unsigned int caps)
{
	//if (mmc->cfg->ops->set_ios)
	//	mmc->cfg->ops->set_ios(mmc);
	if (mmc->set_ios)
		mmc->set_ios(mmc, caps);
}

void mmc_set_mode_select(struct mmc *mmc, uint mode)
{
        mmc->mode_sel= mode;

        mmc_set_ios(mmc,MMC_IOS_CLK);
}

void mmc_set_clock(struct mmc *mmc, uint clock)
{
	//if (clock > mmc->cfg->f_max)
	//	clock = mmc->cfg->f_max;

	//if (clock < mmc->cfg->f_min)
	//	clock = mmc->cfg->f_min;
	
	if (clock > mmc->f_max)
		clock = mmc->f_max;

	if (clock < mmc->f_min)
		clock = mmc->f_min;

	mmc->clock = clock;

	if (mmc->block_dev.if_type == IF_TYPE_SD) {
		if (mmc->set_ios) {
			mmc->set_ios(mmc, MMC_IOS_CLK/*bit1*/);
		}
		else {
			printf(VT100_LIGHT_RED "MMC: set clock not effective" VT100_NONE_NL);
		}
	}
	else {
		mmc_set_ios(mmc, MMC_IOS_CLK);
	}
}

void mmc_set_bus_width(struct mmc *mmc, uint width)
{
	mmc->bus_width = width;

	if (mmc->block_dev.if_type == IF_TYPE_SD) {
		if (mmc->set_ios) {
			mmc->set_ios(mmc, MMC_IOS_BUSWIDTH/*bit2*/);
		}
		else {
			printf(VT100_LIGHT_RED "MMC: set width not effective" VT100_NONE_NL);
		}
	}
	else {
		mmc_set_ios(mmc, MMC_IOS_BUSWIDTH);
	}
}
static int mmc_startup(struct mmc *mmc)
{
	int err, i;
	uint mult=0, freq=0;
	volatile u64 cmult=0, csize=0, capacity=0;
	struct mmc_cmd cmd;
	static volatile int cmd_retry=0,cmd_retry1=0;
	volatile uint cid_val=0;
	bool part_completed;
	bool has_parts = false;
	//struct mmc_data data;

	MY_CLR_ALIGN_BUFFER();
	MY_ALLOC_CACHE_ALIGN_BUFFER(char, ext_csd, CSD_ARRAY_SIZE);
	MY_ALLOC_CACHE_ALIGN_BUFFER(char, g_ext_csd, CSD_ARRAY_SIZE);
	int timeout = 1000;

#ifdef CONFIG_MMC_SPI_CRC_ON
	if (mmc_host_is_spi(mmc)) { /* enable CRC check for spi */
		cmd.cmdidx = MMC_CMD_SPI_CRC_ON_OFF;
		cmd.resp_type = MMC_RSP_R1;
		cmd.cmdarg = 1;
		err = mmc_send_cmd(mmc, &cmd, NULL);

		if (err)
			return err;
	}
#endif
        //set initial speed
        rtkemmc_set_wrapper_div(0);        //no wrapper divider

CMD_RETRY:
	/* Put the Card in Identify Mode */
	cmd.cmdidx = mmc_host_is_spi(mmc) ? MMC_CMD_SEND_CID :
		MMC_CMD_ALL_SEND_CID; /* cmd not supported in spi */
	cmd.resp_type = MMC_RSP_R2;
	cmd.cmdarg = 0;

	err = mmc_send_cmd(mmc, &cmd, NULL);
	if (err)
	{
		if (cmd_retry1++ > MAX_CMD_RETRY_COUNT)
			return err;
		cmd_retry=0;
		while (cmd_retry++ < MAX_CMD_RETRY_COUNT)
		{
			err = mmc_send_op_cond(mmc);
			if (err) {
				printf("Card did not respond to voltage select!\n");
				continue;
			}
			else
				break;
		}
		if (cmd_retry >= MAX_CMD_RETRY_COUNT)
			return err;
		else
		{
			goto CMD_RETRY;
		}
	}

	memcpy(mmc->cid, cmd.response, 16);
	flush_cache((unsigned long)mmc->cid, sizeof(unsigned int)*4);

	cid_val = (mmc->cid[0]>>24)&0xff;
	printf("The cid_val is %x.\n",cid_val);
#ifdef MMC_DEBUG
	printf("[LY] cid[0]=0x%02x\n",mmc->cid[0]>>24);
#endif
#ifdef DISABLE_MICRON_AUTO_STANDBY
	if ((((mmc->cid[0] >> 24)&0xff)== MANU_ID_MICRON1) || (((mmc->cid[0] >> 24)&0xff)== MANU_ID_MICRON2))
	{
		g_bMicronFlash = 1;
		//try sdr first
		rtkemmc_set_wrapper_div(0);        //no wrapper divider

		cmd_retry=0;cmd_retry1=0;
		memset(outBlk, 0x00, 512);
		memset(outTwoBlk, 0x00, 1024);
		memset(rcvBlk, 0x00, 512);
		outBlk[0] = 0x84;
		outTwoBlk[0] = 0x08;

		MPRINTF("[LY] disable procedure start -->\n");
		//step 1
		/* Reset the Card */
		err = mmc_go_idle(mmc);
		MPRINTF("[LY] <--- cmd0 sent(%d)--->\n",err);
		if (err)
			return err;

		//step 2.1
		cmd.cmdidx = MMC_CMD_MICRON_63;
		cmd.resp_type = MMC_RSP_NONE;
		cmd.cmdarg = 0x50485349;

		err = mmc_send_cmd(mmc, &cmd, NULL);
		if (err)
			return err;
		MPRINTF("[LY] <---cmd63 sent--->\n");
		//step 2.2
		cmd.cmdidx = MMC_CMD_MICRON_63;
		cmd.resp_type = MMC_RSP_NONE;
		cmd.cmdarg = 0x50485353;

		err = mmc_send_cmd(mmc, &cmd, NULL);
		if (err)
			return err;
		MPRINTF("[LY] <---cmd63 sent--->\n");

		//step 3
		/* Reset the Card */
		sync();
		err = mmc_go_idle(mmc);
		MPRINTF("[LY] <---cmd0 sent--->\n");

		//step 4
		cmd.cmdidx = MMC_CMD_MICRON_63;
		cmd.resp_type = MMC_RSP_NONE;
		cmd.cmdarg = 0x50485343;

		err = mmc_send_cmd(mmc, &cmd, NULL);
		if (err)
			return err;
		MPRINTF("[LY] <--- cmd63 sent--->\n");

		//step 5
		//cmd 0/1
		sync();
		err = mmc_send_op_cond(mmc);
		if (err) {
			printf("Card did not respond to voltage select!\n");
			return UNUSABLE_ERR;
		}

		/* Put the Card in Identify Mode */
		//cmd 2
		cmd.cmdidx = mmc_host_is_spi(mmc) ? MMC_CMD_SEND_CID :
			MMC_CMD_ALL_SEND_CID; /* cmd not supported in spi */
		cmd.resp_type = MMC_RSP_R2;
		cmd.cmdarg = 0;
		err = mmc_send_cmd(mmc, &cmd, NULL);
		if (err)
			return err;
		MPRINTF("[LY] <---cmd 0/1/2 sent--->\n");

		//step 6
		cmd.cmdidx = MMC_CMD_MICRON_62;
		cmd.resp_type = MMC_RSP_NONE;
		cmd.cmdarg = 0x5048534D;

		err = mmc_send_cmd(mmc, &cmd, NULL);
		if (err)
			return err;
		MPRINTF("[LY] <---cmd62 sent--->\n");
		sync();
		//step 7
		cmd.cmdidx = SD_CMD_SEND_RELATIVE_ADDR;
		cmd.cmdarg = mmc->rca << 16;
		cmd.resp_type = MMC_RSP_R6;

		err = mmc_send_cmd(mmc, &cmd, NULL);
		if (err)
			return err;
		MPRINTF("[LY] <--- cmd3 sent --->\n");
		sync();

		//step 8
		cmd.cmdidx = MMC_CMD_SELECT_CARD;
		cmd.resp_type = MMC_RSP_R1;
		cmd.cmdarg = mmc->rca << 16;

		err = mmc_send_cmd(mmc, &cmd, NULL);
		if (err)
			return err;
		MPRINTF("[LY] <--- cmd7 sent--->\n");
		sync();

		mmc_set_ios(mmc,MMC_IOS_NONE_DIV);
		udelay(1000);
		sync();
#if 1
    #if 1
		err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_HS_TIMING, 1);
		if (err)
		{
			printf("[LY] card : switch to hs fail\n");
			return err;
		}
    #endif
		mmc_getset_pad(mmc, MMC_IOS_GET_PAD_DRV);
		udelay(1000);
		sync();

		err = mmc_Select_SDR50_Push_Sample();
		if (err)
		{
			printf("[LY] host : switch to hs fail\n");
			return err;
		}
		mmc_getset_pad(mmc, MMC_IOS_RESTORE_PAD_DRV);
		udelay(1000);
		sync();
#endif
		//step 9 (send blk)
		//speed up, disable ip divider
		cmd.cmdidx = MMC_CMD_MICRON_60;
		cmd.cmdarg = 0x0;   //??
		cmd.resp_type = MMC_RSP_R1;

		data.src = outBlk;
		data.blocks = 1;
		data.blocksize = 0x200;
		data.resp_type = MMC_DATA_WRITE;
		MPRINTF("[LY] write blk size = %08x\n",data.blocksize);

		if (mmc_send_cmd(mmc, &cmd, &data)>0) {
			printf("mmc write failed,cmd60 fail, send stop\n");
#if 0
			cmd.opcode = MMC_CMD_STOP_TRANSMISSION;
			cmd.arg = 0;
			cmd.flags = MMC_RSP_R1b;
			if (mmc_send_cmd(mmc, &cmd, NULL)) {
				printf("mmc fail to send stop cmd\n");
				goto REINIT;
			}
#endif
			goto REINIT;
		}
		MPRINTF("[LY] <--- cmd60 sent --->\n");
		sync();
		//step 10 (recv blk)
		cmd.cmdidx = MMC_CMD_MICRON_61;
		cmd.cmdarg = 0x0;   //??
		cmd.resp_type = MMC_RSP_R1;

		data.src = rcvBlk;
		data.blocks = 1;
		data.blocksize = 0x200;
		data.resp_type = MMC_DATA_READ;
		MPRINTF("[LY] read blk size = %08x\n",data.blocksize);

		if (mmc_send_cmd(mmc, &cmd, &data)>0) {
			printf("mmc read failed,cmd61 fail\n");
			cmd.cmdidx = MMC_CMD_STOP_TRANSMISSION;
			cmd.cmdarg = 0;
			cmd.resp_type = MMC_RSP_R1b;

			if (mmc_send_cmd(mmc, &cmd, NULL)) {
				printf("mmc fail to send stop cmd\n");
				goto REINIT;
			}
			goto REINIT;
		}
		MPRINTF("[LY] <--- cmd61 sent --->\n");
		sync();

		//step 11
		cmd.cmdidx = MMC_CMD_STOP_TRANSMISSION;
		cmd.cmdarg = 0;
		cmd.resp_type = MMC_RSP_R1b;

		if (mmc_send_cmd(mmc, &cmd, NULL)) {
			printf("mmc fail to send stop cmd\n");
			goto REINIT;
		}
		MPRINTF("[LY] <--- cmd12 sent --->\n");
		sync();

		/* Waiting for the ready status */
		if (mmc_send_status(mmc, 1000))
		{
			printf("[LY] send status fail\n");
			goto REINIT;
		}
		MPRINTF("[LY] <--- cmd13 sent --->\n");
		sync();
		//step 12 (send 2 blk)
		memcpy(outTwoBlk+512, outBlk, 512);
		outTwoBlk[512+58] = 0xff;
		sync();
		flush_cache(outTwoBlk, CSD_ARRAY_SIZE*2);
		cmd.cmdidx = MMC_CMD_MICRON_60;
		cmd.cmdarg = 0x0;   //??
		cmd.resp_type = MMC_RSP_R1;

		data.src = outTwoBlk;
		data.blocks = 2;
		data.blocksize = 0x200*2;
		data.flags = MMC_DATA_WRITE;
		MPRINTF("[LY] write 2 blk size = %08x\n",data.blocksize);

		if (mmc_send_cmd(mmc, &cmd, &data)>0) {
			printf("mmc write failed\n");
			goto REINIT;
		}
		MPRINTF("[LY] <--- cmd60 sent--->\n");
		MPRINTF("[LY] disable procedure done ------->\n");
		sync();
		printf("Standby : disable micron standby mode\n");

REINIT:
		//back to original startup path
		//cmd 0/1
		mmc_set_ios(mmc,MMC_IOS_INIT_DIV);
		udelay(1000);
		sync();
		err = mmc_send_op_cond(mmc);
		if (err) {
			printf("Card did not respond to voltage select!\n");
			return UNUSABLE_ERR;
		}

		/* Put the Card in Identify Mode */

		cmd.cmdidx = mmc_host_is_spi(mmc) ? MMC_CMD_SEND_CID :
			MMC_CMD_ALL_SEND_CID; /* cmd not supported in spi */
		cmd.resp_type = MMC_RSP_R2;
		cmd.cmdarg = 0;

		err = mmc_send_cmd(mmc, &cmd, NULL);
		if (err)
			return err;
	}
#endif
	/*
	 * For MMC cards, set the Relative Address.
         * For SD cards, get the Relatvie Address.
         * This also puts the cards into Standby State
	*/
	if (!mmc_host_is_spi(mmc)) { /* cmd not supported in spi */
		cmd.cmdidx = SD_CMD_SEND_RELATIVE_ADDR;
		cmd.cmdarg = mmc->rca << 16;
		cmd.resp_type = MMC_RSP_R6;

		err = mmc_send_cmd(mmc, &cmd, NULL);

		if (err)
			return err;

		if (IS_SD(mmc))
			mmc->rca = (cmd.response[0] >> 16) & 0xffff;
	}

	/* Get the Card-Specific Data */
	cmd.cmdidx = MMC_CMD_SEND_CSD;
	cmd.resp_type = MMC_RSP_R2;
	cmd.cmdarg = mmc->rca << 16;

	err = mmc_send_cmd(mmc, &cmd, NULL);

	/* Waiting for the ready status */
	mmc_send_status(mmc, timeout);

	if (err)
		return err;

	mmc->csd[0] = cmd.response[0];
	mmc->csd[1] = cmd.response[1];
	mmc->csd[2] = cmd.response[2];
	mmc->csd[3] = cmd.response[3];

#ifdef MMC_DEBUG
	mmc_show_csd(mmc);
	mmc_decode_cid(mmc);
#endif

	flush_cache((unsigned long)cmd.response, sizeof(unsigned int)*4);
	flush_cache((unsigned long)mmc->csd, sizeof(unsigned int)*4);

	printf("mmc->version=0x%08x\n", mmc->version);
	if (mmc->version == MMC_VERSION_UNKNOWN) {
		int version = (cmd.response[0] >> 26) & 0xf;

		printf("version=0x%08x\n", version);
		switch (version) {
			case 0:
				mmc->version = MMC_VERSION_1_2;
				break;
			case 1:
				mmc->version = MMC_VERSION_1_4;
				break;
			case 2:
				mmc->version = MMC_VERSION_2_2;
				break;
			case 3:
				mmc->version = MMC_VERSION_3;
				break;
			case 4:
				mmc->version = MMC_VERSION_4;
				break;
			default:
				mmc->version = MMC_VERSION_1_2;
				break;
		}
	}

	/* divide frequency by 10, since the mults are 10x bigger */
	freq = fbase[(cmd.response[0] & 0x7)];
	mult = multipliers[((cmd.response[0] >> 3) & 0xf)];

	mmc->tran_speed = (u64)(freq * mult);
#ifdef MMC_DEBUG
	printf("0 [LY] freq=0x%08x, mult=0x%08x,mmc->tran_speed=%lld\n",freq,mult,mmc->tran_speed);
#endif
	mmc->dsr_imp = ((cmd.response[1] >> 12) & 0x1);
	mmc->read_bl_len = 1 << ((cmd.response[1] >> 16) & 0xf);

	if (IS_SD(mmc))
		mmc->write_bl_len = mmc->read_bl_len;
	else
		mmc->write_bl_len = 1 << ((cmd.response[3] >> 22) & 0xf);

	if (mmc->high_capacity) {
		csize = (mmc->csd[1] & 0x3f) << 16
			| (mmc->csd[2] & 0xffff0000) >> 16;
		cmult = 8;
		#ifdef MMC_DEBUG
		printf("1 [LY] csize=0x%lld, cmult=0x%lld\n",csize,cmult);
		#endif
	} else {
		csize = (mmc->csd[1] & 0x3ff) << 2
			| (mmc->csd[2] & 0xc0000000) >> 30;
		cmult = (mmc->csd[2] & 0x00038000) >> 15;
		#ifdef MMC_DEBUG
		printf("1.1 [LY] csize=%lld, cmult=%lld\n",csize,cmult);
		#endif
	}

	mmc->capacity_user = (csize + 1) << (cmult + 2);
	mmc->capacity_user *= mmc->read_bl_len;
	mmc->capacity_boot = 0;
	mmc->capacity_rpmb = 0;
	for (i = 0; i < 4; i++)
		mmc->capacity_gp[i] = 0;
	#ifdef MMC_DEBUG
	printf("2 [LY] read_bl_len=0x%08x, cmd.rsp[1]=0x%08x\n",mmc->read_bl_len,(cmd.response[1]>>16)&0xf);
	#endif
#if 0
        mmc->capacity = (u64)((csize + 1) << (cmult + 2));
        mmc->capacity = ((u64)(mmc->capacity)) * ((u64)(mmc->read_bl_len));
        #ifdef MMC_DEBUG
        printf("3 [LY] mmc->cap=%lld\n",mmc->capacity);
        #endif
#endif
	if (mmc->read_bl_len > MMC_MAX_BLOCK_LEN)
		mmc->read_bl_len = MMC_MAX_BLOCK_LEN;

	if (mmc->write_bl_len > MMC_MAX_BLOCK_LEN)
		mmc->write_bl_len = MMC_MAX_BLOCK_LEN;

	if ((mmc->dsr_imp) && (0xffffffff != mmc->dsr)) {
		cmd.cmdidx = MMC_CMD_SET_DSR;
		cmd.cmdarg = (mmc->dsr & 0xffff) << 16;
		cmd.resp_type = MMC_RSP_NONE;
		if (mmc_send_cmd(mmc, &cmd, NULL))
			printf("MMC: SET_DSR failed\n");
	}

	/* Select the card, and put it into Transfer Mode */
	if (!mmc_host_is_spi(mmc)) { /* cmd not supported in spi */
		cmd.cmdidx = MMC_CMD_SELECT_CARD;
		cmd.resp_type = MMC_RSP_R1;
		cmd.cmdarg = mmc->rca << 16;
		err = mmc_send_cmd(mmc, &cmd, NULL);

		if (err)
			return err;
	}

	/*
	 * For SD, its erase group is always one sector
	*/

	mmc->erase_grp_size = 1;
	mmc->part_config = MMCPART_NOAVAILABLE;
	if (!IS_SD(mmc) && (mmc->version >= MMC_VERSION_4)) {
		/* check ext_csd version and capacity */
		flush_cache((unsigned long)ext_csd, CSD_ARRAY_SIZE);
		MMCPRINTF("[%s:%d] ext_csd = 0x%p", __FILE__, __LINE__, ext_csd);
		err = mmc_send_ext_csd(mmc, (unsigned char *) ext_csd);
		flush_cache((unsigned long)ext_csd, CSD_ARRAY_SIZE);
		if (err)
			return -1;
		else
		{
			flush_cache((unsigned long)g_ext_csd, CSD_ARRAY_SIZE);
			memcpy(g_ext_csd,ext_csd, CSD_ARRAY_SIZE);
			flush_cache((unsigned long)g_ext_csd, CSD_ARRAY_SIZE);
		}

#ifdef MMC_DEBUG
		printf("[LY] ext_csd[EXT_CSD_REV] = 0x%08x, mmc->version=%08x\n", ext_csd[EXT_CSD_REV], mmc->version);
#endif
		if (!err & (ext_csd[EXT_CSD_REV] >= 2)) {
			/*
			 * According to the JEDEC Standard, the value of
			 * ext_csd's capacity is valid if the value is more
			 * than 2GB
			 */
			capacity = ext_csd[EXT_CSD_SEC_CNT] << 0
					| ext_csd[EXT_CSD_SEC_CNT + 1] << 8
					| ext_csd[EXT_CSD_SEC_CNT + 2] << 16
					| ext_csd[EXT_CSD_SEC_CNT + 3] << 24;
			capacity *= MMC_MAX_BLOCK_LEN;
			if ((capacity >> 20) > 2 * 1024)
				//mmc->capacity = capacity;
				mmc->capacity_user = capacity;
		}

		switch (ext_csd[EXT_CSD_REV]) {
		case 1:
			mmc->version = MMC_VERSION_4_1;
			break;
		case 2:
			mmc->version = MMC_VERSION_4_2;
			break;
		case 3:
			mmc->version = MMC_VERSION_4_3;
			break;
		case 5:
			mmc->version = MMC_VERSION_4_41;
			break;
		case 6:
			mmc->version = MMC_VERSION_4_5;
			break;
		case 7:
			mmc->version = MMC_VERSION_5_0;
			break;
		}
#ifdef MMC_DEBUG
		printf("5 [LY] mmc->cap=%lld, cap=%lld\n",mmc->capacity,capacity);
#endif

		/*
		 * Check whether GROUP_DEF is set, if yes, read out
		 * group size from ext_csd directly, or calculate
		 * the group size from the csd value.
		 */
		part_completed = !!(ext_csd[EXT_CSD_PARTITION_SETTING] &
				    EXT_CSD_PARTITION_SETTING_COMPLETED);

		/* store the partition info of emmc */
		mmc->part_support = ext_csd[EXT_CSD_PARTITIONING_SUPPORT];
		if ((ext_csd[EXT_CSD_PARTITIONING_SUPPORT] & PART_SUPPORT) ||
		    ext_csd[EXT_CSD_BOOT_MULT])
			mmc->part_config = ext_csd[EXT_CSD_PART_CONF];
		if (part_completed &&
		    (ext_csd[EXT_CSD_PARTITIONING_SUPPORT] & ENHNCD_SUPPORT))
			mmc->part_attr = ext_csd[EXT_CSD_PARTITIONS_ATTRIBUTE];
		mmc->capacity_boot = ext_csd[EXT_CSD_BOOT_MULT] << 17;

		mmc->capacity_rpmb = ext_csd[EXT_CSD_RPMB_MULT] << 17;

		for (i = 0; i < 4; i++) {
			int idx = EXT_CSD_GP_SIZE_MULT + i * 3;
			uint mult = (ext_csd[idx + 2] << 16) +
				(ext_csd[idx + 1] << 8) + ext_csd[idx];
			if (mult)
				has_parts = true;
			if (!part_completed)
				continue;
			mmc->capacity_gp[i] = mult;
			mmc->capacity_gp[i] *=
				ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE];
			mmc->capacity_gp[i] *= ext_csd[EXT_CSD_HC_WP_GRP_SIZE];
			mmc->capacity_gp[i] <<= 19;
		}

		if (part_completed) {
			mmc->enh_user_size =
				(ext_csd[EXT_CSD_ENH_SIZE_MULT+2] << 16) +
				(ext_csd[EXT_CSD_ENH_SIZE_MULT+1] << 8) +
				ext_csd[EXT_CSD_ENH_SIZE_MULT];
			mmc->enh_user_size *= ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE];
			mmc->enh_user_size *= ext_csd[EXT_CSD_HC_WP_GRP_SIZE];
			mmc->enh_user_size <<= 19;
			mmc->enh_user_start =
				(ext_csd[EXT_CSD_ENH_START_ADDR+3] << 24) +
				(ext_csd[EXT_CSD_ENH_START_ADDR+2] << 16) +
				(ext_csd[EXT_CSD_ENH_START_ADDR+1] << 8) +
				ext_csd[EXT_CSD_ENH_START_ADDR];
			if (mmc->high_capacity)
				mmc->enh_user_start <<= 9;
		}

		/*
		 * Host needs to enable ERASE_GRP_DEF bit if device is
		 * partitioned. This bit will be lost every time after a reset
		 * or power off. This will affect erase size.
		 */
		if (part_completed)
			has_parts = true;
		if ((ext_csd[EXT_CSD_PARTITIONING_SUPPORT] & PART_SUPPORT) &&
			(ext_csd[EXT_CSD_PARTITIONS_ATTRIBUTE] & PART_ENH_ATTRIB))
			has_parts = true;
		if (has_parts) {
			err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
				EXT_CSD_ERASE_GROUP_DEF, 1);

			if (err)
				return err;
			else
				ext_csd[EXT_CSD_ERASE_GROUP_DEF] = 1;
		}

		if (ext_csd[EXT_CSD_ERASE_GROUP_DEF] & 0x01){
			mmc->erase_grp_size =
				ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE] * 1024;
			/*
			 * if high capacity and partition setting completed
			 * SEC_COUNT is valid even if it is smaller than 2 GiB
			 * JEDEC Standard JESD84-B45, 6.2.4
			 */
			if (mmc->high_capacity && part_completed) {
				capacity = (ext_csd[EXT_CSD_SEC_CNT]) |
					(ext_csd[EXT_CSD_SEC_CNT + 1] << 8) |
					(ext_csd[EXT_CSD_SEC_CNT + 2] << 16) |
					(ext_csd[EXT_CSD_SEC_CNT + 3] << 24);
				capacity *= MMC_MAX_BLOCK_LEN;
				mmc->capacity_user = capacity;
			}
		} else {
			/* Calculate the group size from the csd value. */
			int erase_gsz, erase_gmul;
			erase_gsz = (mmc->csd[2] & 0x00007c00) >> 10;
			erase_gmul = (mmc->csd[2] & 0x000003e0) >> 5;
			mmc->erase_grp_size = (erase_gsz + 1)
				* (erase_gmul + 1);
		}
		mmc->hc_wp_grp_size = 1024
			* ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE]
			* ext_csd[EXT_CSD_HC_WP_GRP_SIZE];

		mmc->wr_rel_set = ext_csd[EXT_CSD_WR_REL_SET];
		/* store the partition info of emmc */
		if (ext_csd[EXT_CSD_PARTITIONING_SUPPORT] & PART_SUPPORT)
			mmc->part_config = ext_csd[EXT_CSD_PART_CONF];
	}
	else
		printf("[LY] mmc->version < 4\n");

	err = mmc_set_capacity(mmc, mmc->part_num);
	if (err)
		return err;

	if (IS_SD(mmc))
		err = sd_change_freq(mmc);
	else
		err = mmc_get_card_caps(mmc,ext_csd);

	if(err > 0)
		return err;

	/* Restrict card's capabilities by what the host can do */
	mmc->card_caps &= mmc->host_caps;

	if (IS_SD(mmc)) {
		if (mmc->card_caps & MMC_MODE_4BIT) {
			cmd.cmdidx = MMC_CMD_APP_CMD;
			cmd.resp_type = MMC_RSP_R1;
			cmd.cmdarg = mmc->rca << 16;

			err = mmc_send_cmd(mmc, &cmd, NULL);
			if (err)
				return err;

			cmd.cmdidx = SD_CMD_APP_SET_BUS_WIDTH;
			cmd.resp_type = MMC_RSP_R1;
			cmd.cmdarg = 2;
			err = mmc_send_cmd(mmc, &cmd, NULL);
			if (err)
				return err;

			mmc_set_bus_width(mmc, 4);
		}

		if (mmc->card_caps & MMC_MODE_HS)
			mmc->tran_speed = 50000000;
		else
			mmc->tran_speed = 25000000;
	}
#if 0
	else if (mmc->version >= MMC_VERSION_4) {
		/* Only version 4 of MMC supports wider bus widths */
		int idx;

		/* An array of possible bus widths in order of preference */
		static unsigned ext_csd_bits[] = {
			EXT_CSD_DDR_BUS_WIDTH_8,
			EXT_CSD_DDR_BUS_WIDTH_4,
			EXT_CSD_BUS_WIDTH_8,
			EXT_CSD_BUS_WIDTH_4,
			EXT_CSD_BUS_WIDTH_1,
		};

		/* An array to map CSD bus widths to host cap bits */
		static unsigned ext_to_hostcaps[] = {
			[EXT_CSD_DDR_BUS_WIDTH_4] =
				MMC_MODE_DDR_52MHz | MMC_MODE_4BIT,
			[EXT_CSD_DDR_BUS_WIDTH_8] =
				MMC_MODE_DDR_52MHz | MMC_MODE_8BIT,
			[EXT_CSD_BUS_WIDTH_4] = MMC_MODE_4BIT,
			[EXT_CSD_BUS_WIDTH_8] = MMC_MODE_8BIT,
		};

		/* An array to map chosen bus width to an integer */
		static unsigned widths[] = {
			8, 4, 8, 4, 1,
		};

		for (idx=0; idx < ARRAY_SIZE(ext_csd_bits); idx++) {
			unsigned int extw = ext_csd_bits[idx];
			unsigned int caps = ext_to_hostcaps[extw];

			/*
			 * If the bus width is still not changed,
			 * don't try to set the default again.
			 * Otherwise, recover from switch attempts
			 * by switching to 1-bit bus width.
			 */
			if (extw == EXT_CSD_BUS_WIDTH_1 &&
					mmc->bus_width == 1) {
				err = 0;
				break;
			}
			/*
			 * Check to make sure the card and controller support
			 * these capabilities
			 */
			if ((mmc->card_caps & caps) != caps)
				continue;

			err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
					EXT_CSD_BUS_WIDTH, extw);

			if (err)
				continue;

			mmc->ddr_mode = (caps & MMC_MODE_DDR_52MHz) ? 1 : 0;
			mmc_set_bus_width(mmc, widths[idx]);

			err = mmc_send_ext_csd(mmc, test_csd);

			if (err)
				continue;

			/* Only compare read only fields */
			if (ext_csd[EXT_CSD_PARTITIONING_SUPPORT]
				== test_csd[EXT_CSD_PARTITIONING_SUPPORT] &&
			    ext_csd[EXT_CSD_HC_WP_GRP_SIZE]
				== test_csd[EXT_CSD_HC_WP_GRP_SIZE] &&
			    ext_csd[EXT_CSD_REV]
				== test_csd[EXT_CSD_REV] &&
			    ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE]
				== test_csd[EXT_CSD_HC_ERASE_GRP_SIZE] &&
			    memcmp(&ext_csd[EXT_CSD_SEC_CNT],
				   &test_csd[EXT_CSD_SEC_CNT], 4) == 0)
				break;
			else
				err = SWITCH_ERR;
		}

		if (err)
			return err;

		if (mmc->card_caps & MMC_MODE_HS) {
			if (mmc->card_caps & MMC_MODE_HS_52MHz)
				mmc->tran_speed = 50000000;
			else
				mmc->tran_speed = 25000000;
		}
	}
#endif
	mmc_set_clock(mmc, mmc->tran_speed);
	mmc->boot_caps |= MMC_MODE_HS | MMC_MODE_HS_52MHz;

#ifdef MMC_DEBUG
	printf("[LY] bootcaps = %08x\n", mmc->boot_caps);
	printf("[LY] hostcaps= %08x\n", mmc->host_caps);
	printf("[LY] cardcaps = %08x\n", mmc->card_caps);
	printf("[LY] freq = %08x, clk diver = %08x\n", REG32(PLL_EMMC3),REG32(CR_EMMC_CLKDIV));
#else
	printf("[LY] freq = %08x, clk diver = %08x\n", REG32(PLL_EMMC3),REG32(CR_EMMC_CLKDIV));
#endif

#ifdef SUPPORT_HS200
	if (mmc->card_caps & MMC_MODE_HS200) {
		err = mmc_select_hs200(mmc,ext_csd);
		printf("[LY] hs200 : %d\n", err);
		if (err)
		{
//1295 not support ddr50
#if 0
		if ((err != -4)||(err != -5)||(err != -6))
		{
			err = mmc_select_ddr50(mmc,ext_csd);
			printf("[LY] ddr50 : %d\n", err);
		}
		if (err)
#endif
			{
				err = mmc_change_freq(mmc, MODE_SD20, CHANGE_FREQ_HOST);
				if (err)
				{
					printf("[LY] set spd hs200 to sdr50 fail\n");
				}
				mmc->tran_speed = 50000000;
				err = mmc_select_sdr50(mmc,ext_csd);
				printf("[LY] sdr50 : %d\n", err);
			}
		}
		else mmc->tran_speed = 200000000;
	}
	else
	{
//1295 not support ddr50
#if 0
	err = mmc_select_ddr50(mmc,ext_csd);
	printf("[LY] ddr50 : %d\n", err);
	if (err)
#endif
		{
			err = mmc_select_sdr50(mmc,ext_csd);
			printf("[LY] sdr50 : %d\n", err);
		}
	}
#else
	err = mmc_select_sdr50(mmc,ext_csd);
	printf("[LY] sdr50 : %d\n", err);
#endif

#ifdef SUPPORT_WRITE_PROT
	if ((ext_csd[EXT_CSD_ERASE_GROUP_DEF] & 1) == 0){
		err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
			EXT_CSD_ERASE_GROUP_DEF, 1);

			flush_cache((unsigned long)ext_csd, CSD_ARRAY_SIZE);
			err = mmc_send_ext_csd(mmc, (unsigned char *) ext_csd);
			flush_cache((unsigned long)ext_csd, CSD_ARRAY_SIZE);
		}
		if (err)
			printf("[ERR] Fail to set ERASE_GROUP_DEF ! \n");
		else {
			mmc->erase_grp_size =
				ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE] * 512 * 1024;	//512 Kbytes x ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE]
			printf("[HC] ERASE Unit Size = %u bytes\n", mmc->erase_grp_size);
			mmc->hc_wp_grp_size = mmc->erase_grp_size * ext_csd[EXT_CSD_HC_WP_GRP_SIZE];
			printf("[HC] WPG_SIZE = %u bytes\n", mmc->hc_wp_grp_size);
		}
#endif
	/* fill in device description */
	mmc->block_dev.lun = 0;
	mmc->block_dev.type = 0;
	mmc->block_dev.blksz = mmc->read_bl_len;
	mmc->block_dev.lba = lldiv(mmc->capacity, mmc->read_bl_len);
#if 0
	sprintf(mmc->block_dev.vendor, "Man %06x Snr %08x", mmc->cid[0] >> 8,
			(mmc->cid[2] << 8) | (mmc->cid[3] >> 24));
	sprintf(mmc->block_dev.product, "%c%c%c%c%c", mmc->cid[0] & 0xff,
			(mmc->cid[1] >> 24), (mmc->cid[1] >> 16) & 0xff,
			(mmc->cid[1] >> 8) & 0xff, mmc->cid[1] & 0xff);
	sprintf(mmc->block_dev.revision, "%d.%d", mmc->cid[2] >> 28,
			(mmc->cid[2] >> 24) & 0xf);
#endif
	sprintf(mmc->block_dev.vendor, "Man %06x Snr %04x%04x",
		mmc->cid[0] >> 24, (mmc->cid[2] & 0xffff),
		(mmc->cid[3] >> 16) & 0xffff);
	sprintf(mmc->block_dev.product, "%c%c%c%c%c%c", mmc->cid[0] & 0xff,
		(mmc->cid[1] >> 24), (mmc->cid[1] >> 16) & 0xff,
		(mmc->cid[1] >> 8) & 0xff, mmc->cid[1] & 0xff,
		(mmc->cid[2] >> 24) & 0xff);
	sprintf(mmc->block_dev.revision, "%d.%d", (mmc->cid[2] >> 20) & 0xf,
		(mmc->cid[2] >> 16) & 0xf);
	init_part(&mmc->block_dev);

	return 0;
}

#if 0
static int mmc_send_if_cond(struct mmc *mmc)
{
	struct mmc_cmd cmd;
	int err;

	cmd.cmdidx = SD_CMD_SEND_IF_COND;
	/* We set the bit if the host supports voltages between 2.7 and 3.6 V */
	//cmd.cmdarg = ((mmc->cfg->voltages & 0xff8000) != 0) << 8 | 0xaa;
	cmd.cmdarg = ((mmc->voltages & 0xff8000) != 0) << 8 | 0xaa;
	cmd.resp_type = MMC_RSP_R7;

	err = mmc_send_cmd(mmc, &cmd, NULL);
	printf("[CHECK] the error4 is %d.\n",err);

	if (err)
		return err;

	if ((cmd.response[0] & 0xff) != 0xaa)
		return UNUSABLE_ERR;
	else
		mmc->version = SD_VERSION_2;

	return 0;
}
#endif
#if 0
/* not used any more */
int __deprecated mmc_register(struct mmc *mmc)
{
#if !defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_LIBCOMMON_SUPPORT)
	printf("%s is deprecated! use mmc_create() instead.\n", __func__);
#endif
	return -1;
}
#endif
int mmc_register(struct mmc *mmc)
{
	/* Setup dsr related values */
        mmc->dsr_imp = 0;
        mmc->dsr = 0xffffffff;

	/* Setup the universal parts of the block interface just once */
	mmc->block_dev.if_type = IF_TYPE_MMC;
	mmc->block_dev.dev = cur_dev_num++;
	mmc->block_dev.removable = 1;
	mmc->block_dev.block_read = mmc_bread;
	mmc->block_dev.block_write = mmc_bwrite;
	mmc->block_dev.block_erase = mmc_berase;
	mmc->block_dev.part_type = PART_TYPE_UNKNOWN;;
	if (!mmc->b_max)
		mmc->b_max = CONFIG_SYS_MMC_MAX_BLK_COUNT;

	INIT_LIST_HEAD (&mmc->link);

	list_add_tail (&mmc->link, &mmc_devices);

	return 0;
}
#if 0
struct mmc *mmc_create(const struct mmc_config *cfg, void *priv)
{
	struct mmc *mmc;

	/* quick validation */
	if (cfg == NULL || cfg->ops == NULL || cfg->ops->send_cmd == NULL ||
			cfg->f_min == 0 || cfg->f_max == 0 || cfg->b_max == 0)
		return NULL;

	mmc = calloc(1, sizeof(*mmc));
	if (mmc == NULL)
		return NULL;

	mmc->cfg = cfg;
	mmc->priv = priv;

	/* the following chunk was mmc_register() */
	printf("Checking the MMC_CREAT!!!!!!!\n");
	/* Setup dsr related values */
	mmc->dsr_imp = 0;
	mmc->dsr = 0xffffffff;
	/* Setup the universal parts of the block interface just once */
	mmc->block_dev.if_type = IF_TYPE_MMC;
	mmc->block_dev.dev = cur_dev_num++;
	mmc->block_dev.removable = 1;
	mmc->block_dev.block_read = mmc_bread;
	mmc->block_dev.block_write = mmc_bwrite;
	mmc->block_dev.block_erase = mmc_berase;

	/* setup initial part type */
	mmc->block_dev.part_type = mmc->cfg->part_type;

	INIT_LIST_HEAD(&mmc->link);

	list_add_tail(&mmc->link, &mmc_devices);

	return mmc;
}
#endif
void mmc_destroy(struct mmc *mmc)
{
	/* only freeing memory for now */
	free(mmc);
}

#ifdef CONFIG_PARTITIONS
block_dev_desc_t *mmc_get_dev(int dev)
{
	struct mmc *mmc = find_mmc_device(dev);
	if (!mmc || mmc_init(mmc))
		return NULL;

	return &mmc->block_dev;
}
#endif

/* board-specific MMC power initializations. */
__weak void board_mmc_power_init(void)
{
}

int mmc_start_init(struct mmc *mmc)
{
	int err = 0;

	/* we pretend there's no card when init is NULL */
	//if (mmc_getcd(mmc) == 0 || mmc->cfg->ops->init == NULL) {
	if (mmc_getcd(mmc) == 0 || mmc->init == NULL) {
		mmc->has_init = 0;
#if !defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_LIBCOMMON_SUPPORT)
		printf("MMC: no card present\n");
#endif
		return NO_CARD_ERR;
	}

	if (mmc->has_init)
		return 0;

#ifdef CONFIG_FSL_ESDHC_ADAPTER_IDENT
	mmc_adapter_card_type_ident();
#endif
	board_mmc_power_init();


	/* made sure it's not NULL earlier */
	//err = mmc->cfg->ops->init(mmc);
	mmc->init();

	if (err)
		return err;

	mmc->ddr_mode = 0;
	mmc_set_bus_width(mmc, 1);
	mmc_set_clock(mmc, 1);
	mmc_set_mode_select(mmc, 0);
	
	
	mmc->part_num = 0;
	err = TIMEOUT;


#if 0
	/* Reset the Card */
	err = mmc_go_idle(mmc);

	if (err)
		return err;
	printf("[CHECK] procedure 9!!!\n");
	/* The internal partition reset to user partition(0) at every CMD0*/
	mmc->part_num = 0;

	/* Test for SD version 2 */
	err = mmc_send_if_cond(mmc);
	printf("[CHECK] the error1 is %d.\n",err);

	/* Now try to get the SD card's operating condition */
	err = sd_send_op_cond(mmc);
	printf("[CHECK] the error2 is %d.\n",err);
#endif

	/* If the command timed out, we check for an MMC card */
	if (err == TIMEOUT) {
		err = mmc_send_op_cond(mmc);

		if (err) {
#if !defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_LIBCOMMON_SUPPORT)
			printf("Card did not respond to voltage select!\n");
#endif
			return UNUSABLE_ERR;
		}
	}

	if (!err)
		mmc->init_in_progress = 1;

	return err;
}

static int mmc_complete_init(struct mmc *mmc)
{
	int err = 0;
	mmc->init_in_progress = 0;
	if (mmc->op_cond_pending)
		err = mmc_complete_op_cond(mmc);

	if (!err)
		err = mmc_startup(mmc);
	if (err)
		mmc->has_init = 0;
	else
		mmc->has_init = 1;
	
	return err;
}

int mmc_init(struct mmc *mmc)
{
	int err = 0;
	unsigned start;

	if (mmc->has_init)
		return 0;

	start = get_timer(0);
	if (!mmc->init_in_progress)
		err = mmc_start_init(mmc);
	if (!err)
		err = mmc_complete_init(mmc);
	debug("%s: %d, time %lu\n", __func__, err, get_timer(start));
	return err;
}

int mmc_set_dsr(struct mmc *mmc, u16 val)
{
	mmc->dsr = val;
	return 0;
}

/* CPU-specific MMC initializations */
__weak int cpu_mmc_init(bd_t *bis)
{
	return -1;
}

/* board-specific MMC initializations. */
__weak int board_mmc_init(bd_t *bis)
{
	return -1;
}

#if !defined(CONFIG_SPL_BUILD) || defined(CONFIG_SPL_LIBCOMMON_SUPPORT)

void print_mmc_devices(char separator)
{
	struct mmc *m;
	struct list_head *entry;
	char *mmc_type;
	
	list_for_each(entry, &mmc_devices) {
		m = list_entry(entry, struct mmc, link);
		if (m->has_init)
			mmc_type = IS_SD(m) ? "SD" : "eMMC";
		else
			mmc_type = NULL;
		
		printf("%s: %d", m->name, m->block_dev.dev);
		//printf("%s: %d", m->cfg->name, m->block_dev.dev);
		if (mmc_type)
			printf(" (%s)", mmc_type);

		if (entry->next != &mmc_devices) {
			printf("%c", separator);
			if (separator != '\n')
				puts (" ");
		}
	}
	printf("\n");
}

#else
void print_mmc_devices(char separator) { }
#endif

int get_mmc_num(void)
{
	return cur_dev_num;
}

void mmc_set_preinit(struct mmc *mmc, int preinit)
{
	mmc->preinit = preinit;
}

static void do_preinit(void)
{
	struct mmc *m;
	struct list_head *entry;

	list_for_each(entry, &mmc_devices) {
		m = list_entry(entry, struct mmc, link);

#ifdef CONFIG_FSL_ESDHC_ADAPTER_IDENT
		mmc_set_preinit(m, 1);
#endif
		if (m->preinit)
			mmc_start_init(m);
	}
}


int mmc_initialize(bd_t *bis)
{
	INIT_LIST_HEAD (&mmc_devices);
	cur_dev_num = 0;

	if (board_mmc_init(bis) < 0)
		cpu_mmc_init(bis);

#ifndef CONFIG_SPL_BUILD
	print_mmc_devices(',');
#endif

	do_preinit();
	return 0;
}

#ifdef CONFIG_SUPPORT_EMMC_BOOT
/*
 * This function changes the size of boot partition and the size of rpmb
 * partition present on EMMC devices.
 *
 * Input Parameters:
 * struct *mmc: pointer for the mmc device strcuture
 * bootsize: size of boot partition
 * rpmbsize: size of rpmb partition
 *
 * Returns 0 on success.
 */

int mmc_boot_partition_size_change(struct mmc *mmc, unsigned long bootsize,
				unsigned long rpmbsize)
{
	int err;
	struct mmc_cmd cmd;

	/* Only use this command for raw EMMC moviNAND. Enter backdoor mode */
	cmd.cmdidx = MMC_CMD_RES_MAN;
	cmd.resp_type = MMC_RSP_R1b;
	cmd.cmdarg = MMC_CMD62_ARG1;

	err = mmc_send_cmd(mmc, &cmd, NULL);
	if (err) {
		debug("mmc_boot_partition_size_change: Error1 = %d\n", err);
		return err;
	}

	/* Boot partition changing mode */
	cmd.cmdidx = MMC_CMD_RES_MAN;
	cmd.resp_type = MMC_RSP_R1b;
	cmd.cmdarg = MMC_CMD62_ARG2;

	err = mmc_send_cmd(mmc, &cmd, NULL);
	if (err) {
		debug("mmc_boot_partition_size_change: Error2 = %d\n", err);
		return err;
	}
	/* boot partition size is multiple of 128KB */
	bootsize = (bootsize * 1024) / 128;

	/* Arg: boot partition size */
	cmd.cmdidx = MMC_CMD_RES_MAN;
	cmd.resp_type = MMC_RSP_R1b;
	cmd.cmdarg = bootsize;

	err = mmc_send_cmd(mmc, &cmd, NULL);
	if (err) {
		debug("mmc_boot_partition_size_change: Error3 = %d\n", err);
		return err;
	}
	/* RPMB partition size is multiple of 128KB */
	rpmbsize = (rpmbsize * 1024) / 128;
	/* Arg: RPMB partition size */
	cmd.cmdidx = MMC_CMD_RES_MAN;
	cmd.resp_type = MMC_RSP_R1b;
	cmd.cmdarg = rpmbsize;

	err = mmc_send_cmd(mmc, &cmd, NULL);
	if (err) {
		debug("mmc_boot_partition_size_change: Error4 = %d\n", err);
		return err;
	}
	return 0;
}

/*
 * Modify EXT_CSD[177] which is BOOT_BUS_WIDTH
 * based on the passed in values for BOOT_BUS_WIDTH, RESET_BOOT_BUS_WIDTH
 * and BOOT_MODE.
 *
 * Returns 0 on success.
 */
int mmc_set_boot_bus_width(struct mmc *mmc, u8 width, u8 reset, u8 mode)
{
	int err;

	err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_BOOT_BUS_WIDTH,
			 EXT_CSD_BOOT_BUS_WIDTH_MODE(mode) |
			 EXT_CSD_BOOT_BUS_WIDTH_RESET(reset) |
			 EXT_CSD_BOOT_BUS_WIDTH_WIDTH(width));

	if (err)
		return err;
	return 0;
}

/*
 * Modify EXT_CSD[179] which is PARTITION_CONFIG (formerly BOOT_CONFIG)
 * based on the passed in values for BOOT_ACK, BOOT_PARTITION_ENABLE and
 * PARTITION_ACCESS.
 *
 * Returns 0 on success.
 */
int mmc_set_part_conf(struct mmc *mmc, u8 ack, u8 part_num, u8 access)
{
	int err;

	err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_PART_CONF,
			 EXT_CSD_BOOT_ACK(ack) |
			 EXT_CSD_BOOT_PART_NUM(part_num) |
			 EXT_CSD_PARTITION_ACCESS(access));

	if (err)
		return err;
	return 0;
}

/*
 * Modify EXT_CSD[162] which is RST_n_FUNCTION based on the given value
 * for enable.  Note that this is a write-once field for non-zero values.
 *
 * Returns 0 on success.
 */
int mmc_set_rst_n_function(struct mmc *mmc, u8 enable)
{
	return mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL, EXT_CSD_RST_N_FUNCTION,
			  enable);
}
#endif


int mmc_select_sdr50(struct mmc *mmc,char* ext_csd)
{
	extern unsigned int gCurrentBootMode;
	volatile int width=0;
	volatile int err=0, ret=0;

	MY_CLR_ALIGN_BUFFER();
	MY_ALLOC_CACHE_ALIGN_BUFFER(unsigned char, test_csd, CSD_ARRAY_SIZE*2);

		gCurrentBootMode = MODE_SD20;
		mmc->boot_caps &= ~MMC_MODE_HS200;
		//try sdr first
		rtkemmc_set_wrapper_div(0);        //no wrapper divider
		
		//restore original pad value
		//mmc_getset_pad(mmc, MMC_IOS_RESTORE_PAD_DRV);

		//let card in better quality channel
		err = mmc_change_freq(mmc,MODE_SD20, CHANGE_FREQ_CARD);
		if (err)
		{
			printf("[LY] set crd to hs fail:%d\n", err);
			return -7;
		}
		sync();
		err = mmc_change_freq(mmc,MODE_SD20, CHANGE_FREQ_HOST);
		width = BUS_WIDTH_8;
		for (; width >= 0; width--) {
			printf("[LY] SDR bus width=%d\n", width);
			sync();
			err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
					EXT_CSD_BUS_WIDTH, width);
			sync();
			if (err)
			{
				if (width == BUS_WIDTH_1)
					return -4;
				else
					continue;
			}

			if (!width)
				mmc_set_bus_width(mmc, 1);
			else
				mmc_set_bus_width(mmc, 4 * width);
			sync();

			flush_cache((unsigned long)test_csd, CSD_ARRAY_SIZE);
			err = mmc_send_ext_csd(mmc, (unsigned char *) test_csd);
			#ifdef MMC_DEBUG
    			mmc_show_ext_csd(test_csd);
			#endif

			if (!err && ext_csd[EXT_CSD_PARTITIONING_SUPPORT] \
				    == test_csd[EXT_CSD_PARTITIONING_SUPPORT]
				 && ext_csd[EXT_CSD_ERASE_GROUP_DEF] \
				    == test_csd[EXT_CSD_ERASE_GROUP_DEF] \
				 && ext_csd[EXT_CSD_REV] \
				    == test_csd[EXT_CSD_REV]
				 && ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE] \
				    == test_csd[EXT_CSD_HC_ERASE_GRP_SIZE]
				 && memcmp(&ext_csd[EXT_CSD_SEC_CNT], \
					&test_csd[EXT_CSD_SEC_CNT], 4) == 0) {

				mmc->card_caps |= width;
				mmc->boot_caps |= (width << MMC_MODE_WIDTH_BITS_SHIFT);
				printf("[LY] mmc->boot_caps = %02x\n",mmc->boot_caps);
				ret=0;
				break;
			}
			else {
				if (!width) {
					printf("MMC: set bus width 1 fail\n");
				}
				else {
					printf("MMC: set bus width %d fail, try other mode\n", (4 * width));
				}
				ret=-2;
			}
		}

		if (mmc->card_caps & MMC_MODE_HS) {
			if (mmc->card_caps & MMC_MODE_HS_52MHz)
				mmc->tran_speed = 52000000;
			else
				mmc->tran_speed = 26000000;
		}

	return ret;
}

int mmc_select_hs200(struct mmc *mmc,char* ext_csd)
{
        volatile int width=0;
        volatile int err=0,ret=0;

        MY_CLR_ALIGN_BUFFER();
        MY_ALLOC_CACHE_ALIGN_BUFFER(char, test_csd, CSD_ARRAY_SIZE*2);

        // HS-200
        if (!IS_SD(mmc)&&((mmc->host_caps & MMC_MODE_HS200) == (mmc->card_caps & MMC_MODE_HS200)))
        {
                //set max pad value
                //mmc_getset_pad(mmc, MMC_IOS_SET_PAD_DRV);

                //try hs-200, 8/4 bits
                printf("[LY] speed up emmc at HS-200 \n");
                width = BUS_WIDTH_8;
                sync();
                for (; width > 0; width--) {
                        printf("[LY] HS-200 bus width=%d\n", width);
                        sync();
                        err = mmc_switch(mmc, EXT_CSD_CMD_SET_NORMAL,
                                        EXT_CSD_BUS_WIDTH, width);
                        sync();
                        if (err)
                        {
                                //get the correct sample/push point at SDR mode
                                printf("[LY] hs200 card bus switch retry result = %d\n", err);
                                if (width == BUS_WIDTH_4)
                                        return -6;
                                continue;
                        }
                        sync();
                        mmc_set_bus_width(mmc, 4 * width);
                        sync();

                        {

                        flush_cache((unsigned long)test_csd, CSD_ARRAY_SIZE);
                        err = mmc_send_ext_csd(mmc, (unsigned char *)test_csd);
                        flush_cache((unsigned long)test_csd, CSD_ARRAY_SIZE);

			if (!err && ext_csd[EXT_CSD_PARTITIONING_SUPPORT] \
                                    == test_csd[EXT_CSD_PARTITIONING_SUPPORT]
                                 && ext_csd[EXT_CSD_ERASE_GROUP_DEF] \
                                    == test_csd[EXT_CSD_ERASE_GROUP_DEF] \
                                 && ext_csd[EXT_CSD_REV] \
                                    == test_csd[EXT_CSD_REV]
                                 && ext_csd[EXT_CSD_HC_ERASE_GRP_SIZE] \
                                    == test_csd[EXT_CSD_HC_ERASE_GRP_SIZE]
                                 && memcmp(&ext_csd[EXT_CSD_SEC_CNT], \
                                        &test_csd[EXT_CSD_SEC_CNT], 4) == 0) {

                                mmc->card_caps |= width;
                                mmc->boot_caps |= (width << MMC_MODE_WIDTH_BITS_SHIFT);
                                mmc->boot_caps |= MMC_MODE_HS200;
                                printf("[LY] mmc->boot_caps = %02x\n",mmc->boot_caps);
                                ret = 0;
                                break;
                        }
                        else {
                                if (!width) {
                                        printf("MMC: set hs200 bus width 1 fail\n");
                                }
                                else {
                                        printf("MMC: set hs200 bus width %d fail, try other mode\n", (4 * width));
                                }
                                ret = -2;
                        }
                        }
                }

        }
        else
                return -1;

	err = mmc_change_freq(mmc, MODE_SD30, CHANGE_FREQ_CARD);
        if (err)
        {
                printf("[LY] hs200 card cmd fail = %d\n", err);
                return -5;
        }
        sync();
        err = mmc_change_freq(mmc, MODE_SD30, CHANGE_FREQ_HOST);
        if (err)
        {
                printf("[LY] set host hs200 speed fail = %d\n", err);
                return -4;
        }
        //get the correct sample/push point at DDR mode
        err = mmc_Tuning_HS200();
        if (err)
        {
                printf("[LY] hs200 tuning fail(%d)\n",err);
                return err;
        }

        //hs200 at 100Mhz
        mmc->tran_speed = 100000000;
        return ret;
}
