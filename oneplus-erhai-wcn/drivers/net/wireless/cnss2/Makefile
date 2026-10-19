# SPDX-License-Identifier: GPL-2.0-only
#
# CNSS2 platform driver (literal downstream port). In-tree build: QMI IDL +
# cnss_utils headers live under drivers/net/wireless/cnss_utils.

ccflags-y += -I$(srctree)/drivers/net/wireless/cnss_utils

obj-$(CONFIG_CNSS2) += cnss2.o

cnss2-y := main.o
cnss2-y += bus.o
cnss2-y += debug.o
cnss2-y += pci.o
cnss2-y += power.o
cnss2-y += genl.o
cnss2-y += oplus_wifi.o
# pci_qcom.o is the downstream msm_pcie RC glue — excluded (CONFIG_PCI_MSM=n),
# the driver uses the mainline PCIe RC via pci_platform.h's #else branch.
cnss2-$(CONFIG_PCI_MSM) += pci_qcom.o
cnss2-$(CONFIG_CNSS2_QMI) += qmi.o coexistence_service_v01.o
