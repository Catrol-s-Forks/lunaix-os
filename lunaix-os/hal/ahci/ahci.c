/**
 * @file ahci.c
 * @author Lunaixsky (zelong56@gmail.com)
 * @brief A software implementation of Serial ATA AHCI 1.3.1 Specification
 * @version 0.1
 * @date 2022-06-28
 *
 * @copyright Copyright (c) 2022
 *
 */
#include <hal/ahci/ahci.h>
#include <hal/ahci/utils.h>
#include <hal/pci.h>
#include <klibc/string.h>
#include <lunaix/mm/mmio.h>
#include <lunaix/mm/pmm.h>
#include <lunaix/mm/valloc.h>
#include <lunaix/mm/vmm.h>
#include <lunaix/spike.h>
#include <lunaix/syslog.h>

#define HBA_FIS_SIZE 256
#define HBA_CLB_SIZE 1024

LOG_MODULE("AHCI")

static struct ahci_hba hba;

void
__ahci_hba_isr(isr_param param);

void
ahci_init()
{
    struct pci_device* ahci_dev = pci_get_device_by_class(AHCI_HBA_CLASS);
    assert_msg(ahci_dev, "AHCI: Not found.");

    uintptr_t bar6, size;
    size = pci_bar_sizing(ahci_dev, &bar6, 6);
    assert_msg(bar6 && PCI_BAR_MMIO(bar6), "AHCI: BAR#6 is not MMIO.");

    pci_reg_t cmd = pci_read_cspace(ahci_dev->cspace_base, PCI_REG_STATUS_CMD);

    // 禁用传统中断（因为我们使用MSI），启用MMIO访问，允许PCI设备间访问
    cmd |= (PCI_RCMD_MM_ACCESS | PCI_RCMD_DISABLE_INTR | PCI_RCMD_BUS_MASTER);

    pci_write_cspace(ahci_dev->cspace_base, PCI_REG_STATUS_CMD, cmd);

    pci_setup_msi(ahci_dev, AHCI_HBA_IV);
    intr_subscribe(AHCI_HBA_IV, __ahci_hba_isr);

    memset(&hba, 0, sizeof(hba));

    hba.base = (hba_reg_t*)ioremap(PCI_BAR_ADDR_MM(bar6), size);

    // 重置HBA
    hba.base[HBA_RGHC] |= HBA_RGHC_RESET;
    wait_until(!(hba.base[HBA_RGHC] & HBA_RGHC_RESET));

    // 启用AHCI工作模式，启用中断
    hba.base[HBA_RGHC] |= HBA_RGHC_ACHI_ENABLE;
    hba.base[HBA_RGHC] |= HBA_RGHC_INTR_ENABLE;

    // As per section 3.1.1, this is 0 based value.
    hba_reg_t cap = hba.base[HBA_RCAP];
    hba.ports_num = (cap & 0x1f) + 1;  // CAP.PI
    hba.cmd_slots = (cap >> 8) & 0x1f; // CAP.NCS
    hba.version = hba.base[HBA_RVER];

    /* ------ HBA端口配置 ------ */
    hba_reg_t pmap = hba.base[HBA_RPI];
    uintptr_t clb_pg_addr, fis_pg_addr, clb_pa, fis_pa;
    for (size_t i = 0, fisp = 0, clbp = 0; i < 32;
         i++, pmap >>= 1, fisp = (fisp + 1) % 16, clbp = (clbp + 1) % 4) {
        if (!(pmap & 0x1)) {
            continue;
        }

        struct ahci_port* port =
          (struct ahci_port*)valloc(sizeof(struct ahci_port));
        hba_reg_t* port_regs =
          (hba_reg_t*)(&hba.base[HBA_RPBASE + i * HBA_RPSIZE]);

        if (!clbp) {
            // 每页最多4个命令队列
            clb_pa = pmm_alloc_page(KERNEL_PID, PP_FGLOCKED);
            clb_pg_addr = ioremap(clb_pa, 0x1000);
            memset(clb_pg_addr, 0, 0x1000);
        }
        if (!fisp) {
            // 每页最多16个FIS
            fis_pa = pmm_alloc_page(KERNEL_PID, PP_FGLOCKED);
            fis_pg_addr = ioremap(fis_pa, 0x1000);
            memset(fis_pg_addr, 0, 0x1000);
        }

        /* 重定向CLB与FIS */
        port_regs[HBA_RPxCLB] = clb_pa + clbp * HBA_CLB_SIZE;
        port_regs[HBA_RPxFB] = fis_pa + fisp * HBA_FIS_SIZE;

        *port = (struct ahci_port){ .regs = port_regs,
                                    .ssts = port_regs[HBA_RPxSSTS],
                                    .cmdlst = clb_pg_addr + clbp * HBA_CLB_SIZE,
                                    .fis = fis_pg_addr + fisp * HBA_FIS_SIZE };

        /* 初始化端口，并置于就绪状态 */
        port_regs[HBA_RPxCI] = 0;

        // 需要通过全部置位去清空这些寄存器（相当的奇怪……）
        port_regs[HBA_RPxSERR] = -1;

        port_regs[HBA_RPxIE] |= (HBA_PxINTR_DMA);
        port_regs[HBA_RPxIE] |= (HBA_PxINTR_D2HR);

        hba.ports[i] = port;

        if (HBA_RPxSSTS_IF(port->ssts)) {
            wait_until(!(port_regs[HBA_RPxCMD] & HBA_PxCMD_CR));
            port_regs[HBA_RPxCMD] |= HBA_PxCMD_FRE;
            port_regs[HBA_RPxCMD] |= HBA_PxCMD_ST;
            if (!ahci_identify_device(port)) {
                kprintf(KERROR "fail to probe device info");
            }
        }
    }
}

char sata_ifs[][20] = { "Not detected",
                        "SATA I (1.5Gbps)",
                        "SATA II (3.0Gbps)",
                        "SATA III (6.0Gbps)" };

void
__ahci_hba_isr(isr_param param)
{
    // TODO: hba interrupt
    kprintf(KDEBUG "HBA INTR\n");
}

void
ahci_list_device()
{
    kprintf(KINFO "Version: %x; Ports: %d; Slot: %d\n",
            hba.version,
            hba.ports_num,
            hba.cmd_slots);
    struct ahci_port* port;
    for (size_t i = 0; i < 32; i++) {
        port = hba.ports[i];

        // 愚蠢的gcc似乎认为 struct ahci_port* 不可能为空
        //  所以将这个非常关键的if给优化掉了。
        //  这里将指针强制转换为整数，欺骗gcc :)
        if ((uintptr_t)port == 0) {
            continue;
        }

        int device_state = HBA_RPxSSTS_IF(port->ssts);

        kprintf("\t Port %d: %s (%x)\n",
                i,
                &sata_ifs[device_state],
                port->regs[HBA_RPxSIG]);

        struct ahci_device_info* dev_info = port->device_info;
        if (!device_state || !dev_info) {
            continue;
        }

        kprintf("\t\t capacity: %d KiB\n",
                (dev_info->max_lba * dev_info->sector_size) >> 10);
        kprintf("\t\t sector size: %dB\n", dev_info->sector_size);
        kprintf("\t\t model: %s\n", &dev_info->model);
        kprintf("\t\t serial: %s\n", &dev_info->serial_num);
    }
}

int
achi_alloc_slot(struct ahci_port* port)
{
    hba_reg_t pxsact = port->regs[HBA_RPxSACT];
    hba_reg_t pxci = port->regs[HBA_RPxCI];
    hba_reg_t free_bmp = pxsact | pxci;
    uint32_t i = 0;
    for (; i <= hba.cmd_slots && (free_bmp & 0x1); i++, free_bmp >>= 1)
        ;
    return i | -(i > hba.cmd_slots);
}

void
__ahci_create_fis(struct sata_reg_fis* cmd_fis,
                  uint8_t command,
                  uint32_t lba_lo,
                  uint32_t lba_hi,
                  uint16_t sector_count)
{
    cmd_fis->head.type = SATA_REG_FIS_H2D;
    cmd_fis->head.options = SATA_REG_FIS_COMMAND;
    cmd_fis->head.status_cmd = command;
    cmd_fis->dev = 0;

    cmd_fis->lba0 = SATA_LBA_COMPONENT(lba_lo, 0);
    cmd_fis->lba8 = SATA_LBA_COMPONENT(lba_lo, 8);
    cmd_fis->lba16 = SATA_LBA_COMPONENT(lba_lo, 16);
    cmd_fis->lba24 = SATA_LBA_COMPONENT(lba_lo, 24);

    cmd_fis->lba32 = SATA_LBA_COMPONENT(lba_hi, 0);
    cmd_fis->lba40 = SATA_LBA_COMPONENT(lba_hi, 8);

    cmd_fis->count = sector_count;
}

int
ahci_identify_device(struct ahci_port* port)
{
    int slot = achi_alloc_slot(port);
    assert_msg(slot >= 0, "No free slot");

    // 清空任何待响应的中断
    port->regs[HBA_RPxIS] = 0;

    /* 发送ATA命令，参考：SATA AHCI Spec Rev.1.3.1, section 5.5 */

    // 构建命令头（Command Header）和命令表（Command Table）
    struct ahci_hba_cmdh* cmd_header = &port->cmdlst[slot];
    struct ahci_hba_cmdt* cmd_table = valloc_dma(sizeof(struct ahci_hba_cmdt));

    memset(cmd_header, 0, sizeof(*cmd_header));
    memset(cmd_table, 0, sizeof(*cmd_table));

    // 预备DMA接收缓存，用于存放HBA传回的数据
    uint16_t* data_in = (uint16_t*)valloc_dma(512);

    cmd_table->entries[0] =
      (struct ahci_hba_prdte){ .data_base = vmm_v2p(data_in),
                               .byte_count = 511 }; // byte_count是从0开始算的

    // 在命令表中构建命令FIS
    struct sata_reg_fis* cmd_fis = (struct sata_reg_fis*)cmd_table->command_fis;

    // 根据设备类型使用合适的命令
    if (port->regs[HBA_RPxSIG] == HBA_DEV_SIG_ATA) {
        // ATA 一般为硬盘
        __ahci_create_fis(cmd_fis, ATA_IDENTIFY_DEVICE, 0, 0, 0);
    } else {
        // ATAPI 一般为光驱，软驱，或者磁带机
        __ahci_create_fis(cmd_fis, ATA_IDENTIFY_PAKCET_DEVICE, 0, 0, 0);
    }

    // 将命令表挂到命令头上
    cmd_header->cmd_table_base = vmm_v2p(cmd_table);
    cmd_header->prdt_len = 1;
    cmd_header->options |=
      HBA_CMDH_FIS_LEN(sizeof(*cmd_fis)) | HBA_CMDH_CLR_BUSY;

    // PxCI寄存器置位，告诉HBA这儿有个数据需要发送到SATA端口
    port->regs[HBA_RPxCI] = (1 << slot);

    wait_until(!(port->regs[HBA_RPxCI] & (1 << slot)));

    /*
        等待数据到达内存
        解析IDENTIFY DEVICE传回来的数据。
          参考：
            * ATA/ATAPI Command Set - 3 (ACS-3), Section 7.12.7

        注意：ATAPI无法通过IDENTIFY PACKET DEVICE 获取容量信息。
        这需要另外使用特殊的SCSI命令中的READ CAPACITY(16)
        来获取，这种命令需要使用ATA的PACKET命令发出。
          参考：
            * ATA/ATAPI Command Set - 3 (ACS-3), Section 7.18
            * SATA AHCI HBA Spec, Section 5.3.7
            * SCSI Command Reference Manual, Section 3.26
    */
    port->device_info = valloc(sizeof(struct ahci_device_info));
    ahci_parse_dev_info(port->device_info, data_in);

    vfree_dma(data_in);
    vfree_dma(cmd_table);

    return 1;
}

// TODO: Support ATAPI Device.