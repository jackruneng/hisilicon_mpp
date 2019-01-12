/*
 * Source code of this file largely depends on Hi3532
 * architecture.
 */

#if defined SLV_ARCH_HI3531
#include "hi3531_dev.h"
#elif defined SLV_ARCH_HI3532
#include "hi3532_dev.h"
#elif defined SLV_ARCH_HI3535
#include "hi3535_dev.h"
#else
#error "Error: No proper host arch selected!"
#endif

#include "../../dma_trans/dma_trans.h"

#define PCIE0_DMA_LOCAL_IRQ		19

#define PCIE0_LOCAL_CFG			0x20800000

#define PCIE0_MEM_BASE			0x30000000
#define PCIE0_MEM_SIZE			0x10000000

#define RAM_BASE			0x80000000
#define RAM_END				0xffffffff

/*
 ************************************************
 **** Base address: PCIe configuration space ****
 */

/* For move PCIe window */
#define ATU_BAR_NUM_BIT(x)		(x<<0x8)
#define ATU_REG_INDEX_BIT(x)		(x<<0x0)

/* This bit will be set after a dma transferred */
#define DMA_DONE_INTERRUPT_BIT		(1<<0)

/* This bit will be set abort accurred in dma transfer */
#define DMA_ABORT_INTERRUPT_BIT		(1<<16)

#define DMA_CHANNEL_DONE_BIT		(11<<5)
#define DMA_LOCAL_INTERRUPT_ENABLE_BIT	(1<<3)
#define DMA_WRITE_CHANNEL_BIT		(0<<31)
#define DMA_READ_CHANNEL_BIT		(1<<31)

#define HI3532_DEBUG            4
#define HI3532_INFO             3
#define HI3532_ERR              2
#define HI3532_FATAL            1
#define HI3532_CURRENT_LEVEL    2
#define HI3532_TRACE(level, s, params...)   do{ \
	        if(level <= HI3532_CURRENT_LEVEL) \
	        printk("[%s, %d]: " s "\n", __func__, __LINE__, ##params); \
}while (0)

static struct pci_operation *s_pcie_opt;

/*
 * If address is a valid RAM address, return 1;
 * else return 0.
 */
static int is_valid_ram_address(unsigned int address)
{
	if ((address >= RAM_BASE) && (address < RAM_END))
		return 1;

	return 0;
}

/*
 * Normally, this function is called for starting a dma task.
 */
static unsigned int get_pcie_controller(int bar)
{

#ifdef __IS_PCI_HOST__
	/* Hi3532 as host, only controller0 is used. */
	return 0;	/* device <----> controller0 */
#else
	int controller = 0;

	/* Hi3532 as slv, get host side controller */
	if (s_pcie_opt->sysctl_reg_virt)
		controller = readl(s_pcie_opt->sysctl_reg_virt
				+ DEV_CONTROLLER_REG_OFFSET);

	return controller;
#endif
}

#ifndef __IS_PCI_HOST__
static unsigned int read_index_reg(struct hi35xx_dev *hi_dev)
{
	unsigned int val = 0xffffffff;

	if (!s_pcie_opt->sysctl_reg_virt) {
		HI3532_TRACE(HI3532_ERR, "read index register failed,"
				" sysctl_base is NULL!");
		return 0xffffffff;
	}
	val = readl(s_pcie_opt->sysctl_reg_virt + DEV_INDEX_REG_OFFSET);
	return val;
}
#endif

static unsigned int get_local_slot_number(void)
{
	unsigned int slot = 0;

#ifndef __IS_PCI_HOST__
	slot = read_index_reg(0);
	slot &= 0x1f;
	if (0 == slot)
		HI3532_TRACE(HI3532_ERR, "invalid local slot"
				" number[0x%x]!", slot);
#endif

	return slot;
}

static unsigned int pcie_vendor_device_id(struct hi35xx_dev *hi_dev)
{
	u32 vendor_device_id = 0x0;

	s_pcie_opt->pci_config_read(hi_dev,
			CFG_VENDORID_REG,
			&vendor_device_id);

	return vendor_device_id;
}

static int init_hidev(struct hi35xx_dev *hi_dev)
{
	s_pcie_opt->move_pf_window_in(hi_dev,
			s_pcie_opt->sysctl_reg_phys,
			0x1000,
			1);

	return 0;
}

static int get_hiirq_number(void)
{
	int irq = GLOBAL_SOFT_IRQ;
	return irq;
}

static int get_pcie_dma_local_irq_number(unsigned int controller)
{
	return PCIE0_DMA_LOCAL_IRQ;
}

static int is_dma_supported(void)
{
#ifdef __IS_PCI_HOST__
	return 0;
#else
	return 1;
#endif

}

/* Only slv will enable pcie dma */
static void enable_pcie_dma_local_irq(unsigned int controller)
{
	unsigned int dma_channel_status;

	dma_channel_status = readl(s_pcie_opt->local_controller[0]->config_virt
			+ DMA_CHANNEL_CONTROL);

	dma_channel_status |= DMA_CHANNEL_DONE_BIT;
	dma_channel_status |= DMA_LOCAL_INTERRUPT_ENABLE_BIT;

	writel(dma_channel_status, s_pcie_opt->local_controller[0]->config_virt
			+ DMA_CHANNEL_CONTROL);
}

static void disable_pcie_dma_local_irq(unsigned int controller)
{
	unsigned int dma_channel_status;

	dma_channel_status = readl(s_pcie_opt->local_controller[0]->config_virt
			+ DMA_CHANNEL_CONTROL);

	dma_channel_status &= (~DMA_LOCAL_INTERRUPT_ENABLE_BIT);

	writel(dma_channel_status, s_pcie_opt->local_controller[0]->config_virt
			+ DMA_CHANNEL_CONTROL);
}

void __start_dma_task(void *task)
{
	unsigned long pcie_conf_virt = 0x0;
	struct pcit_dma_task *new_task = &((struct pcit_dma_ptask *)task)->task;

	pcie_conf_virt = s_pcie_opt->local_controller[0]->config_virt;

#if 0
	if (((0x9ff200c0 == new_task->src)
			&& (0x40 == new_task->len)) ||
			((0x9ff00040 == new_task->src)
				&& (0x40 == new_task->len)))
		;
	else {
		printk(KERN_ERR ".func %s, src 0x%x dest 0x%x len 0x%x ;",
				__func__, new_task->src, new_task->dest, new_task->len);
		printk(KERN_ERR "dma type %s, dir %s\n\n",
		      (task->type == 1) ? "msg":"data",
		      (new_task->dir == PCI_DMA_READ) ? "read":"write");
	}
#endif

	if (new_task->dir == PCI_DMA_WRITE) {
		writel(0x0,		pcie_conf_virt + DMA_WRITE_INTERRUPT_MASK);
		writel(0x0,		pcie_conf_virt + DMA_CHANNEL_CONTEXT_INDEX);
		writel(0x68,		pcie_conf_virt + DMA_CHANNEL_CONTROL);
		writel(new_task->len,	pcie_conf_virt + DMA_TRANSFER_SIZE);
		writel(new_task->src,	pcie_conf_virt + DMA_SAR_LOW);
		writel(new_task->dest,	pcie_conf_virt + DMA_DAR_LOW);

		/* start DMA transfer */
		writel(0x0,		pcie_conf_virt + DMA_WRITE_DOORBELL);

	} else if (new_task->dir == PCI_DMA_READ) {
		writel(0x0,		pcie_conf_virt + DMA_READ_INTERRUPT_MASK);
		writel(0x80000000,	pcie_conf_virt + DMA_CHANNEL_CONTEXT_INDEX);
		writel(0x68,		pcie_conf_virt + DMA_CHANNEL_CONTROL);
		writel(new_task->len,	pcie_conf_virt + DMA_TRANSFER_SIZE);
		writel(new_task->src,	pcie_conf_virt + DMA_SAR_LOW);
		writel(new_task->dest,	pcie_conf_virt + DMA_DAR_LOW);

		/* start DMA transfer */
		writel(0x0,		pcie_conf_virt + DMA_READ_DOORBELL);

	} else {
		HI3532_TRACE(HI3532_ERR, "Wrong dma task data![dir 0x%x]!",
				new_task->dir);
		HI3532_TRACE(HI3532_ERR, "Start_dma_task failed!");
	}
}

/*
 * return:
 * -1: err;
 *  0: Not DMA read irq;
 *  1: DMA done and clear successfully.
 *  2: DMA abort and clear successfully.
 */
static int clear_dma_read_local_irq(unsigned int controller)
{
	/*
	 * PCIe DMA will be started in slave side only
	 * for Hi3531, so DMA local irq will be generated
	 * in slave side only. No need to implement this
	 * function for host side.
	 */
	unsigned int read_status;
	int ret = 1;

	read_status = readl(s_pcie_opt->local_controller[0]->config_virt + DMA_READ_INTERRUPT_STATUS);
	HI3532_TRACE(HI3532_INFO, "PCIe DMA irq status: read[0x%x]", read_status);

	if (0x0 == read_status) {
		HI3532_TRACE(HI3532_INFO, "No PCIe DMA read irq triggerred!");
		return 0;
	}

	if (unlikely(DMA_ABORT_INTERRUPT_BIT & read_status)) {
		HI3532_TRACE(HI3532_ERR, "DMA read abort !!!");
		ret = 2;
	}

	if ((DMA_ABORT_INTERRUPT_BIT & read_status) || (DMA_DONE_INTERRUPT_BIT & read_status))
		writel(DMA_ABORT_INTERRUPT_BIT | DMA_DONE_INTERRUPT_BIT,
				s_pcie_opt->local_controller[0]->config_virt + DMA_READ_INTERRUPT_CLEAR);

	/* return clear done */
	return ret;
}

/*
 * return:
 * -1: err;
 *  0: Not DMA write irq;
 *  1: DMA done and clear successfully.
 *  2: DMA abort and clear successfully.
 */
static int clear_dma_write_local_irq(unsigned int controller)
{
	/*
	 * PCIe DMA will be started in slave side only
	 * for Hi3531, so DMA local irq will be generated
	 * in slave side only. No need to implement this
	 * function for host side.
	 */
	unsigned int write_status;
	int ret = 1;

	write_status = readl(s_pcie_opt->local_controller[0]->config_virt + DMA_WRITE_INTERRUPT_STATUS);
	if (0x0 == write_status) {
		HI3532_TRACE(HI3532_INFO, "No PCIe DMA write irq triggerred!");
		return 0;
	}
	if (unlikely(DMA_ABORT_INTERRUPT_BIT & write_status)) {
		HI3532_TRACE(HI3532_ERR, "DMA write abort !!!");
		ret = 2;
	}

	if ((DMA_DONE_INTERRUPT_BIT & write_status) || (DMA_ABORT_INTERRUPT_BIT & write_status))
		writel(DMA_ABORT_INTERRUPT_BIT | DMA_DONE_INTERRUPT_BIT,
				s_pcie_opt->local_controller[0]->config_virt + DMA_WRITE_INTERRUPT_CLEAR);

	/* return clear done */
	return ret;
}

static int dma_controller_init(void)
{
	return 0;
}

static void dma_controller_exit(void)
{
}

static int request_dma_resource(irqreturn_t (*handler)(int irq, void *id))
{
	int ret = 0;

#ifdef __IS_PCI_HOST__
	return ret;
#else

	/* parameter[0] means nothing here for slave */
	clear_dma_read_local_irq(0);
	clear_dma_write_local_irq(0);
	ret = request_irq(s_pcie_opt->local_controller[0]->dma_local_irq, handler,
			0, "PCIe DMA local-irq", NULL);
	if (ret) {
		HI3532_TRACE(HI3532_ERR, "request PCIe DMA irq failed!");
		return -1;
	}

	/* parameter[0] means nothing here for slave */
	enable_pcie_dma_local_irq(0);

	return 0;
#endif
}

static void release_dma_resource(void)
{
	free_irq(s_pcie_opt->local_controller[0]->dma_local_irq, NULL);
}

static int request_message_irq(irqreturn_t (*handler)(int irq, void *id))
{
	return 0;
}

static void release_message_irq(void)
{
}

static int clear_hiirq(void)
{
	unsigned int status = 0;

	writel(0x0, s_pcie_opt->sysctl_reg_virt + GLB_SOFT_IRQ_CMD_REG);

	status = readl(s_pcie_opt->sysctl_reg_virt + HIIRQ_STATUS_REG_OFFSET);
	if (0x0 == status) {
		HI3532_TRACE(HI3532_ERR, "Hi-irq mis-triggerred!");
		return 0;
	}

	writel(0x0, s_pcie_opt->sysctl_reg_virt + HIIRQ_STATUS_REG_OFFSET);

	return 1;
}

/*
 * return:
 * -1: err;
 *  0: Not message irq;
 *  1: clear successfully.
 */
static int clear_message_irq(struct hi35xx_dev *hi_dev)
{
	return clear_hiirq();
}

/* report an int-x irq to host */
static void pcie_intx_irq(void)
{
	unsigned int val = 0x0;

	if (s_pcie_opt->local_controller[0]->id == 0x0) {
		val = readl(s_pcie_opt->sysctl_reg_virt + PERIPHCTRL30);
		val |= PCIE_INT_X_BIT;
		writel(val, s_pcie_opt->sysctl_reg_virt + PERIPHCTRL30);
		return;
	}
}

static void trigger_message_irq(unsigned long hi_dev)
{
	pcie_intx_irq();
}

static void move_pf_window_in(struct hi35xx_dev *hi_dev,
		unsigned int target_address,
		unsigned int size,
		unsigned int index)
{
		unsigned int local_config_virt = s_pcie_opt->local_controller[0]->config_virt;
		writel(0x00100147, local_config_virt + CFG_COMMAND_REG);

		/*
		 * bit 31 indicates the mapping mode: inbound(host looks at slave);
		 * bit 0 indicates the mapping viewport number;
		 */
		writel(0x80000000 | ATU_REG_INDEX_BIT(index), local_config_virt + ATU_VIEW_PORT);

		/* No need to configure cfg_lower_base in inbound mode */
		writel(0x0, local_config_virt + ATU_BASE_LOWER);
		writel(0x0, local_config_virt + ATU_BASE_UPPER);
		writel(0xffffffff, local_config_virt + ATU_LIMIT);
		writel(target_address, local_config_virt + ATU_TARGET_LOWER);
		writel(0x0, local_config_virt + ATU_TARGET_UPPER);
		writel(0x0, local_config_virt + ATU_REGION_CONTROL1);

		/* bit 11~9 indicates the right BAR this window bind to */
		writel(0xc0000000 | ATU_BAR_NUM_BIT(index), local_config_virt + ATU_REGION_CONTROL2);
}

/*
 * inbound: move_pf_window_out(0x2, 0x82000000, 0x800000, 0x0);
 * outbound: move_pf_window_out(0x31000000, 0x82000000, 0x800000, 0x0);
 */
static void move_pf_window_out(struct hi35xx_dev *hi_dev,
		unsigned int target_address,
		unsigned int size,
		unsigned int index)
{
}

static void clear_dma_registers(void)
{

	/* clear dma registers */
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + DMA_CHANNEL_CONTEXT_INDEX);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + DMA_SAR_HIGH);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + DMA_SAR_LOW);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + DMA_TRANSFER_SIZE);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + DMA_DAR_LOW);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + DMA_DAR_HIGH);

	/* clear not used DMA registers */
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0x9d0);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0x9d4);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0x9d8);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0x9dc);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0x9e0);

	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0xa8c);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0xa90);

	writel(0x80000000, s_pcie_opt->local_controller[0]->config_virt + DMA_CHANNEL_CONTEXT_INDEX);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + DMA_SAR_HIGH);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + DMA_SAR_LOW);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + DMA_TRANSFER_SIZE);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + DMA_DAR_LOW);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + DMA_DAR_HIGH);

	/* clear registers not used */
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0xa3c);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0xa40);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0xa44);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0xa48);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0xa4c);

	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0xa8c);
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + 0xa90);

	/* clear DMA channel controll register */
	writel(0x0, s_pcie_opt->local_controller[0]->config_virt + DMA_CHANNEL_CONTROL);

	/* enable dma write engine */
	writel(0x1, s_pcie_opt->local_controller[0]->config_virt + DMA_WRITE_ENGINE_ENABLE);

	/* enable dma read engine */
	writel(0x1, s_pcie_opt->local_controller[0]->config_virt + DMA_READ_ENGINE_ENABLE);
}

#ifdef __IS_PCI_HOST__
static int host_handshake_step0(struct hi35xx_dev *hi_dev)
{
	return 0;
}

static int host_handshake_step1(struct hi35xx_dev *hi_dev)
{
	return 0;
}

static int host_handshake_step2(struct hi35xx_dev *hi_dev)
{
	return 0;
}
#endif

static int slv_handshake_step0(void)
{
	unsigned int val = 0;
	unsigned int check_remote_timeout = 1000;
	unsigned int remote_shm_base = 0x0;

	while (1) {
		val = readl(s_pcie_opt->sysctl_reg_virt + DEV_INDEX_REG_OFFSET);
		if (val)
			break;

		HI3532_TRACE(HI3532_ERR, "Host is not ready yet!");

		if (0 == check_remote_timeout--) {
			HI3532_TRACE(HI3532_ERR, "Handshake step0 timeout!");
			return -1;
		}

		msleep(100);
	}

	if (val & (0x1 << SLAVE_SHAKED_BIT)) {
		HI3532_TRACE(HI3532_ERR, "Already checked, or error!");
		return -1;
	}
	/* Message shared buffer should always be page align */

	/*
	 *		Host shm		slave window0
	 *
	 * [shm_base]-->######## <------------- *********<--[dev->shm_base]
	 *		########     Mapping    *********
	 *		########                *********
	 *		######## <------------- *********
	 *
	 */
	remote_shm_base = (val & 0xfffff000);
	g_hi35xx_dev_map[0]->shm_base = remote_shm_base;

	/* Get slave slot_index [0x1 ~ 0x1f] */
	s_pcie_opt->local_devid = val & 0x1f;
	if (0 == s_pcie_opt->local_devid) {
		HI3532_TRACE(HI3532_ERR, "invalid local id[0],"
				" Handshake step0 failed");
		return -1;
	}

	printk("check_remote: local slot_index:0x%x, remote_shm:0x%08x\n",
			s_pcie_opt->local_devid, remote_shm_base);

	move_pf_window_out(NULL, remote_shm_base, g_hi35xx_dev_map[0]->shm_size, 0);

	val |= (0x1 << SLAVE_SHAKED_BIT);
	writel(val, s_pcie_opt->sysctl_reg_virt + DEV_INDEX_REG_OFFSET);

	return 0;
}

static int slv_handshake_step1(void)
{
	return 0;
}

static int slv_pcie_controller_init(void)
{
	int ret = 0;
	unsigned int controller_id = 0;
	s_pcie_opt->local_controller[1] = NULL;
	s_pcie_opt->local_controller[0] = kmalloc(sizeof(struct pci_controller),GFP_KERNEL);
	if (NULL == s_pcie_opt->local_controller[0]) {
		HI3532_TRACE(HI3532_ERR, "kmalloc for pcie_controller%d failed!", controller_id);
		return -ENOMEM;
	}

	s_pcie_opt->local_controller[0]->id = controller_id;
	s_pcie_opt->local_controller[0]->dma_local_irq = PCIE0_DMA_LOCAL_IRQ;
	s_pcie_opt->local_controller[0]->config_base = PCIE0_LOCAL_CFG;

	s_pcie_opt->local_controller[0]->win_base[0] = PCIE_WIN0_BASE;
	s_pcie_opt->local_controller[0]->win_size[0] = PCIE_WIN0_SIZE;
	s_pcie_opt->local_controller[0]->win_base[1] = PCIE_WIN1_BASE;
	s_pcie_opt->local_controller[0]->win_size[1] = PCIE_WIN1_SIZE;
	s_pcie_opt->local_controller[0]->win_base[2] = PCIE_WIN2_BASE;
	s_pcie_opt->local_controller[0]->win_size[2] = PCIE_WIN2_SIZE;

	s_pcie_opt->local_controller[0]->config_virt
		= (unsigned int)ioremap_nocache(s_pcie_opt->local_controller[0]->config_base,
				0x1000);
	if (!s_pcie_opt->local_controller[0]->config_virt) {
		HI3532_TRACE(HI3532_ERR, "ioremap for pcie%d_cfg failed!",
				controller_id);
		ret = -ENOMEM;
		goto alloc_config_err;
	}

	s_pcie_opt->local_controller[0]->win_virt[0]
		= (unsigned int)ioremap_nocache(s_pcie_opt->local_controller[0]->win_base[0],
				s_pcie_opt->local_controller[0]->win_size[0]);
	if (!s_pcie_opt->local_controller[0]->win_virt[0]) {
		HI3532_TRACE(HI3532_ERR, "ioremap for win0 failed!");
		ret = -ENOMEM;
		goto alloc_win0_err;
	}

	s_pcie_opt->local_controller[0]->win_virt[1]
		= (unsigned int)ioremap_nocache(s_pcie_opt->local_controller[0]->win_base[1],
				s_pcie_opt->local_controller[0]->win_size[1]);
	if (!s_pcie_opt->local_controller[0]->win_virt[1]) {
		HI3532_TRACE(HI3532_ERR, "ioremap for win1 failed!");
		ret = -ENOMEM;
		goto alloc_win1_err;
	}

	s_pcie_opt->local_controller[0]->win_virt[2]
		= (unsigned int)ioremap_nocache(s_pcie_opt->local_controller[0]->win_base[2],
				s_pcie_opt->local_controller[0]->win_size[2]);
	if (!s_pcie_opt->local_controller[0]->win_virt[2]) {
		HI3532_TRACE(HI3532_ERR, "ioremap for win2 failed!");
		ret = -ENOMEM;
		goto alloc_win2_err;
	}

	/* The initial value of these registers are not zero, clear them */
	clear_dma_registers();

	s_pcie_opt->local_controller[0]->used_flag = 0x1;

	return 0;

alloc_win2_err:
	iounmap((void *)s_pcie_opt->local_controller[0]->win_virt[1]);
alloc_win1_err:
	iounmap((void *)s_pcie_opt->local_controller[0]->win_virt[0]);
alloc_win0_err:
	iounmap((void *)s_pcie_opt->local_controller[0]->config_virt);
alloc_config_err:
	kfree(s_pcie_opt->local_controller[0]);

	return -1;
}

static void slv_pcie_controller_exit(void)
{
	if (s_pcie_opt->local_controller[0]) {
		if (s_pcie_opt->local_controller[0]->win_virt[2])
			iounmap((void *)s_pcie_opt->local_controller[0]->win_virt[2]);
		if (s_pcie_opt->local_controller[0]->win_virt[1])
			iounmap((void *)s_pcie_opt->local_controller[0]->win_virt[1]);
		if (s_pcie_opt->local_controller[0]->win_virt[0])
			iounmap((void *)s_pcie_opt->local_controller[0]->win_virt[0]);
		if (s_pcie_opt->local_controller[0]->config_virt)
			iounmap((void *)s_pcie_opt->local_controller[0]->config_virt);

		kfree(s_pcie_opt->local_controller[0]);
	}
}

int pci_arch_init(struct pci_operation *handler)
{
	int ret = -1;

	s_pcie_opt = handler;

	s_pcie_opt->sysctl_reg_phys		= SYS_CTRL_BASE;

	s_pcie_opt->move_pf_window_in		= move_pf_window_in;
	s_pcie_opt->move_pf_window_out		= move_pf_window_out;

	s_pcie_opt->init_hidev			= init_hidev;
	s_pcie_opt->local_slot_number		= get_local_slot_number;
	s_pcie_opt->pci_vendor_id		= pcie_vendor_device_id;
	s_pcie_opt->pcie_controller		= get_pcie_controller;

	s_pcie_opt->hiirq_num			= get_hiirq_number;
	s_pcie_opt->clear_msg_irq		= clear_message_irq;
	s_pcie_opt->trigger_msg_irq		= trigger_message_irq;
	s_pcie_opt->request_msg_irq		= request_message_irq;
	s_pcie_opt->release_msg_irq		= release_message_irq;

	s_pcie_opt->is_ram_address		= is_valid_ram_address;
	s_pcie_opt->dma_controller_init		= dma_controller_init;
	s_pcie_opt->dma_controller_exit		= dma_controller_exit;
	s_pcie_opt->request_dma_resource	= request_dma_resource;
	s_pcie_opt->release_dma_resource	= release_dma_resource;
	s_pcie_opt->dma_local_irq_num		= get_pcie_dma_local_irq_number;
	s_pcie_opt->has_dma			= is_dma_supported;
	s_pcie_opt->start_dma_task		= __start_dma_task;
	s_pcie_opt->clear_dma_write_local_irq	= clear_dma_write_local_irq;
	s_pcie_opt->clear_dma_read_local_irq	= clear_dma_read_local_irq;
	s_pcie_opt->enable_dma_local_irq	= enable_pcie_dma_local_irq;
	s_pcie_opt->disable_dma_local_irq	= disable_pcie_dma_local_irq;

#ifdef __IS_PCI_HOST__
	s_pcie_opt->host_handshake_step0	= host_handshake_step0;
	s_pcie_opt->host_handshake_step1	= host_handshake_step1;
	s_pcie_opt->host_handshake_step2	= host_handshake_step2;
#endif
	s_pcie_opt->slv_handshake_step0		= slv_handshake_step0;
	s_pcie_opt->slv_handshake_step1		= slv_handshake_step1;

	s_pcie_opt->sysctl_reg_virt = (unsigned int)ioremap_nocache(s_pcie_opt->sysctl_reg_phys, 0x1000);
	if (!s_pcie_opt->sysctl_reg_virt) {
		HI3532_TRACE(HI3532_ERR, "ioremap for sysctl_reg failed!");
		return -ENOMEM;
	}

	if (slv_pcie_controller_init()) {
		HI3532_TRACE(HI3532_ERR, "Slave side PCIe controller init failed!");
		ret = -1;
		goto pcie_controller_init_err;

	}
	/* return success */
	return 0;

pcie_controller_init_err:
	if (s_pcie_opt->sysctl_reg_virt)
		iounmap((void *)s_pcie_opt->sysctl_reg_virt);

	return -1;
}

void pci_arch_exit(void)
{
	slv_pcie_controller_exit();

	if (s_pcie_opt->sysctl_reg_virt)
		iounmap((void *)s_pcie_opt->sysctl_reg_virt);
}

