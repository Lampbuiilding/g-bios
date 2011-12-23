#include <stdio.h>
#include <loader.h>
#include <flash/nand.h>

// fixme
#define CONFIG_GBH_MAX_LEN   KB(512)
#define CONFIG_GSC_MAX_LEN   KB(128)

#define TIMEOUT_COUNT        (1 << 14)
#define IS_LARGE_PAGE(flash) (flash->write_size >= KB(1))
#define IS_POW2(n)           (((n) & ((n) - 1)) == 0)

static const __u8 nand_ids[] = {
	0x33, 0x35, 0x36, 0x39, 0x43, 0x45, 0x46, 0x49, 0x53, 0x55,
	0x56, 0x59, 0x71, 0x72, 0x73, 0x74, 0x75, 0x76, 0x78, 0x79,
};

// Basic operations
static void inline nand_write_cmd(struct nand_chip *flash, __u8 cmd)
{
	writeb(flash->cmmd_port, cmd);
}

static void inline nand_write_addr(struct nand_chip *flash, __u8 addr)
{
	writeb(flash->addr_port, addr);
}

#ifdef CONFIG_NAND_16BIT
static __u16 inline nand_read_data(struct nand_chip *flash)
{
	return readw(flash->data_port);
}
#else
static __u8 inline nand_read_data(struct nand_chip *flash)
{
	return readb(flash->data_port);
}
#endif

static int nand_wait_ready(struct nand_chip *flash)
{
	volatile int i = 0;

	if (flash->flash_ready) {
		while (i < TIMEOUT_COUNT) {
			if (flash->flash_ready(flash))
				return i;
			i++;
		}
#ifdef CONFIG_DEBUG
		printf("Nand Timeout!\n");
#endif
	} else {
		while (i < TIMEOUT_COUNT)
			i++;
	}

	return i;
}

static void nand_send_cmd(struct nand_chip *flash,
						__u32 cmd, int page_idx, int page_off)
{
	nand_write_cmd(flash, cmd);

	if (page_off != -1) {
		nand_write_addr(flash, page_off & 0xff);

		if (IS_LARGE_PAGE(flash))
			nand_write_addr(flash, (page_off >> 8) & 0xff);
	}

	if (page_idx != -1) {
		nand_write_addr(flash, page_idx & 0xff);
		nand_write_addr(flash, (page_idx >> 8) & 0xff);

		// if ((page_idx >> 16) & 0xff)	 // fixme
			nand_write_addr(flash, (page_idx >> 16) & 0xff); //" &0xff" could be ignored
	}

	switch (cmd) {
	case NAND_CMMD_READ0:
		if (IS_LARGE_PAGE(flash))
			nand_write_cmd(flash, NAND_CMMD_READSTART);
		break;

	default:
		break;
	}

	nand_wait_ready(flash);
}

int nand_probe(struct nand_chip *flash)
{
	__u8  dev_id, ven_id, ext_id;
	int front, end;

	//
	nand_write_cmd(flash, NAND_CMMD_RESET);
	nand_wait_ready(flash);

	nand_send_cmd(flash, NAND_CMMD_READID, -1, 0);
	ven_id = nand_read_data(flash);
	dev_id = nand_read_data(flash);

	if (dev_id == 0) {
		printf("NAND not found!\n");
		return -1;
	}

	printf("NAND = 0x%x%x\n", ven_id, dev_id);

	front = 0;
	end = ARRAY_ELEM_NUM(nand_ids);

	while (front <= end) {
		int nMid = (front + end) / 2;

		if (nand_ids[nMid] == dev_id) {
			flash->write_size = 512;
			flash->block_size = KB(16);

			goto L1;
		}

		if (dev_id < nand_ids[nMid])
			end = nMid -1;
		else
			front = nMid + 1;
	}

	ext_id = nand_read_data(flash);
	ext_id = nand_read_data(flash);

	flash->write_size = KB(1) << (ext_id & 0x3);
	flash->block_size = KB(64) << ((ext_id >> 4) & 0x03);

L1:
#ifdef CONFIG_DEBUG
	printf("(Page Size = 0x%x, Block Size = 0x%x)\n",
		flash->write_size, flash->block_size);

	if (0 == flash->block_size || !IS_POW2(flash->write_size) || !IS_POW2(flash->block_size))
		return -1;
#endif

	return 0;
}

#ifdef CONFIG_NAND_16BIT
static __u16 *nand_read_page(struct nand_chip *flash, __u32 page_idx, __u16 *buff)
{
	__u32 len;

	nand_send_cmd(flash, NAND_CMMD_READ0, page_idx, 0);

	for (len = 0; len < (flash->write_size) / 2; len++) {
		*buff = nand_read_data(flash);
		buff++;
	}

	return buff;
}
#else
static __u8 *nand_read_page(struct nand_chip *flash, __u32 page_idx, __u8 *buff)
{
	__u32 len;

	nand_send_cmd(flash, NAND_CMMD_READ0, page_idx, 0);

	for (len = 0; len < flash->write_size; len++) {
		*buff = nand_read_data(flash);
		buff++;
	}

	return buff;
}
#endif

// load bottom-half from nand with timeout checking
// fixme
int __WEAK__ nand_load(struct nand_chip *flash, __u32 start_blk, void *start_mem, __u32 size)
{
	__u32 cur_page, end_page;
	__u32 wshift = 0, eshift = 0, shift;
	__u32 bh_len = size;
#ifdef CONFIG_NAND_16BIT
	__u16 *buff;
#else
	__u8 *buff;
#endif

	buff = start_mem;

	// yes, calc shift in this way.
	for (shift = 0; shift < sizeof(long) * 8; shift++) {
		if ((1 << shift) == flash->write_size)
			wshift = shift;
		else if ((1 << shift) == flash->block_size)
			eshift = shift;
	}

	if (0 == wshift || 0 == eshift)
		return -1;

	cur_page = start_blk << (eshift - wshift);

	buff = nand_read_page(flash, cur_page, buff);

#if 0
	if (GBH_MAGIC == *(__u32 *)(start_mem + GBH_MAGIC_OFFSET)) {
		bh_len = *(__u32 *)(start_mem + GBH_SIZE_OFFSET);
#ifdef CONFIG_DEBUG
		printf("g-bios BH found:) size = 0x%x\n", bh_len);
#endif
	} else if (G_SYS_MAGIC == *(__u32 *)(start_mem + G_SYS_MAGIC_OFFSET)) {
		bh_len = *(__u32 *)(start_mem + GBH_SIZE_OFFSET);
#ifdef CONFIG_DEBUG
		printf("g-bios sysconf found:) size = 0x%x\n", bh_len);
#endif
	}
#ifdef CONFIG_DEBUG
	else
		printf("g-bios BH not found! assuming size 0x%x\n", bh_len);
#endif
#endif

	end_page = cur_page + ((bh_len - 1) >> wshift);

#ifdef CONFIG_DEBUG
	printf("Load memory = 0x%x, size = 0x%x, cur_page = 0x%x, end_page = 0x%x\n",
		start_mem, bh_len, cur_page, end_page);
#endif

	while (++cur_page <= end_page)
		buff = nand_read_page(flash, cur_page, buff);

	return (void *)buff - start_mem;
}

static int nand_loader(struct loader_opt *opt)
{
	int ret;
	struct nand_chip nand = {0}; // nand_chip must be initialized

	ret = nand_init(&nand);
	if (ret < 0)
		return ret;

	// it's safer to load sysconf first
	ret = nand_load(&nand, CONFIG_SYS_START_BLK,
			(void *)CONFIG_SYS_START_MEM, CONFIG_GSC_MAX_LEN);
	if (ret < 0)
		return ret;

	ret = nand_load(&nand, CONFIG_GBH_START_BLK,
			(void *)CONFIG_GBH_START_MEM, CONFIG_GBH_MAX_LEN);
	if (ret < 0)
		return ret;

	// TODO: check overlaop here! (and move the code to upper layer)

	return ret;
}

REGISTER_LOADER(n, nand_loader, "NAND");
