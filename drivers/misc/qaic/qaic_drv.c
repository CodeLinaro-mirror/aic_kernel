// SPDX-License-Identifier: GPL-2.0-only

/* Copyright (c) 2019, The Linux Foundation. All rights reserved. */

#include <linux/module.h>
#include <linux/pci.h>

#define PCI_VENDOR_ID_QUALCOMM		0x17cb

#define PCI_DEV_AIC100			0xa100

static int qaic_pci_probe(struct pci_dev *pdev,
			  const struct pci_device_id *id)
{
	return 0;
}

static void qaic_pci_remove(struct pci_dev *pdev)
{
}

static const struct pci_device_id ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_QUALCOMM, PCI_DEV_AIC100), },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, ids);

static struct pci_driver qaic_pci_driver = {
	.name = "Qualcomm Cloud AI 100",
	.id_table = ids,
	.probe = qaic_pci_probe,
	.remove = qaic_pci_remove,
};

static int __init qaic_init(void)
{
	return pci_register_driver(&qaic_pci_driver);
}

static void __exit qaic_exit(void)
{
	pci_unregister_driver(&qaic_pci_driver);
}

module_init(qaic_init);
module_exit(qaic_exit);

MODULE_AUTHOR("Qualcomm Cloud AI 100 Accelerator Kernel Driver Team");
MODULE_DESCRIPTION("Qualcomm Cloud 100 AI Accelerators Driver");
MODULE_LICENSE("GPL v2");
