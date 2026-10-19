/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2022-2023 Qualcomm Innovation Center, Inc. All rights reserved. */

#ifndef _CNSS_PCI_PLATFORM_H
#define _CNSS_PCI_PLATFORM_H

#include "pci.h"
#include <linux/of_address.h>	/* v470: of_translate_dma_region() for cnss_pci_get_iommu_addr */

#if IS_ENABLED(CONFIG_PCI_MSM)
/**
 * _cnss_pci_enumerate() - Enumerate PCIe endpoints
 * @plat_priv: driver platform context pointer
 * @rc_num: root complex index that an endpoint connects to
 *
 * This function shall call corresponding PCIe root complex driver APIs
 * to power on root complex and enumerate the endpoint connected to it.
 *
 * Return: 0 for success, negative value for error
 */
int _cnss_pci_enumerate(struct cnss_plat_data *plat_priv, u32 rc_num);

/**
 * cnss_pci_assert_perst() - Assert PCIe PERST GPIO
 * @pci_priv: driver PCI bus context pointer
 *
 * This function shall call corresponding PCIe root complex driver APIs
 * to assert PCIe PERST GPIO.
 *
 * Return: 0 for success, negative value for error
 */
int cnss_pci_assert_perst(struct cnss_pci_data *pci_priv);

/**
 * cnss_pci_fmd_enable() - Update FMD status to PCIe
 * @pci_priv: driver PCI bus context pointer
 *
 * This function shall call corresponding PCIe root complex driver API
 * to update FMD status. The purpose of this API is to handle PERST
 * during execution of FMD recipe.
 *
 * Return: 0 for success, negative value for error
 */
int cnss_pci_fmd_enable(struct cnss_pci_data *pci_priv);

/**
 * cnss_pci_disable_pc() - Disable PCIe link power collapse from RC driver
 * @pci_priv: driver PCI bus context pointer
 * @vote: value to indicate disable (true) or enable (false)
 *
 * This function shall call corresponding PCIe root complex driver APIs
 * to disable PCIe power collapse. The purpose of this API is to avoid
 * root complex driver still controlling PCIe link from callbacks of
 * system suspend/resume. Device driver itself should take full control
 * of the link in such cases.
 *
 * Return: 0 for success, negative value for error
 */
int cnss_pci_disable_pc(struct cnss_pci_data *pci_priv, bool vote);

/**
 * cnss_pci_set_link_bandwidth() - Update number of lanes and speed of
 *                                 PCIe link
 * @pci_priv: driver PCI bus context pointer
 * @link_speed: PCIe link gen speed
 * @link_width: number of lanes for PCIe link
 *
 * This function shall call corresponding PCIe root complex driver APIs
 * to update number of lanes and speed of the link.
 *
 * Return: 0 for success, negative value for error
 */
int cnss_pci_set_link_bandwidth(struct cnss_pci_data *pci_priv,
				u16 link_speed, u16 link_width);

/**
 * cnss_pci_set_max_link_speed() - Set the maximum speed PCIe can link up with
 * @pci_priv: driver PCI bus context pointer
 * @rc_num: root complex index that an endpoint connects to
 * @link_speed: PCIe link gen speed
 *
 * This function shall call corresponding PCIe root complex driver APIs
 * to update the maximum speed that PCIe can link up with.
 *
 * Return: 0 for success, negative value for error
 */
int cnss_pci_set_max_link_speed(struct cnss_pci_data *pci_priv,
				u32 rc_num, u16 link_speed);

/**
 * cnss_reg_pci_event() - Register for PCIe events
 * @pci_priv: driver PCI bus context pointer
 *
 * This function shall call corresponding PCIe root complex driver APIs
 * to register for PCIe events like link down or WAKE GPIO toggling etc.
 * The events should be based on PCIe root complex driver's capability.
 *
 * Return: 0 for success, negative value for error
 */
int cnss_reg_pci_event(struct cnss_pci_data *pci_priv);
void cnss_dereg_pci_event(struct cnss_pci_data *pci_priv);

/**
 * cnss_wlan_adsp_pc_enable: Control ADSP power collapse setup
 * @dev: Platform driver pci private data structure
 * @control: Power collapse enable / disable
 *
 * This function controls ADSP power collapse (PC). It must be called
 * based on wlan state.  ADSP power collapse during wlan RTPM suspend state
 * results in delay during periodic QMI stats PCI link up/down. This delay
 * causes additional power consumption.
 *
 * Result: 0 Success. negative error codes.
 */
int cnss_wlan_adsp_pc_enable(struct cnss_pci_data *pci_priv,
			     bool control);
int cnss_set_pci_link(struct cnss_pci_data *pci_priv, bool link_up);
int cnss_pci_prevent_l1(struct device *dev);
int __cnss_pci_prevent_l1(struct device *dev);
void cnss_pci_allow_l1(struct device *dev);
void __cnss_pci_allow_l1(struct device *dev);
int cnss_pci_get_msi_assignment(struct cnss_pci_data *pci_priv);
int cnss_pci_get_iommu_addr(struct cnss_pci_data *pci_priv, struct device_node *of_node);
int cnss_pci_init_smmu(struct cnss_pci_data *pci_priv);
void cnss_pci_update_drv_supported(struct cnss_pci_data *pci_priv);

/**
 * _cnss_pci_get_reg_dump() - Dump PCIe RC registers for debug
 * @pci_priv: driver PCI bus context pointer
 * @buf: destination buffer pointer
 * @len: length of the buffer
 *
 * This function shall call corresponding PCIe root complex driver API
 * to dump PCIe RC registers for debug purpose.
 *
 * Return: 0 for success, negative value for error
 */
int _cnss_pci_get_reg_dump(struct cnss_pci_data *pci_priv,
			   u8 *buf, u32 len);
#else
int _cnss_pci_enumerate(struct cnss_plat_data *plat_priv, u32 rc_num)
{
	/* Literal-port note: downstream returns -EOPNOTSUPP here because their
	 * non-MSM platforms don't drive on-demand PCIe enumeration. On mainline
	 * the pcie-qcom RC enumerates the WLAN endpoint at boot (with the
	 * wcn7850-pmu pwrseq powering the chip), so enumeration is already done
	 * — return success so cnss_register_bus_driver() proceeds to
	 * pci_register_driver(), which binds the already-present endpoint.
	 */
	return 0;
}

int cnss_pci_assert_perst(struct cnss_pci_data *pci_priv)
{
	return -EOPNOTSUPP;
}

int cnss_pci_fmd_enable(struct cnss_pci_data *pci_priv)
{
	return 0;
}

int cnss_pci_disable_pc(struct cnss_pci_data *pci_priv, bool vote)
{
	return 0;
}

int cnss_pci_set_link_bandwidth(struct cnss_pci_data *pci_priv,
				u16 link_speed, u16 link_width)
{
	return 0;
}

int cnss_pci_set_max_link_speed(struct cnss_pci_data *pci_priv,
				u32 rc_num, u16 link_speed)
{
	return 0;
}

int cnss_reg_pci_event(struct cnss_pci_data *pci_priv)
{
	return 0;
}

void cnss_dereg_pci_event(struct cnss_pci_data *pci_priv) {}

int cnss_wlan_adsp_pc_enable(struct cnss_pci_data *pci_priv, bool control)
{
	return 0;
}

/* erhai: faithful cal->mission WCN cold-cycle. pcie-qcom owns PERST and pwrseq
 * owns WLAN_EN; the exported helper drops/retrains the WCN link so cnss's own
 * suspend_pci_link(DOWN)/resume_pci_link(UP) recovery power-cycles the chip
 * (matches Android msm_pcie L2/L3 + WLAN_EN toggle). Self-gated by the
 * pcie_qcom.qcom_pcie_wcn_relink param (off => returns 0 = old stub). */
int qcom_pcie_wcn_set_link(bool up);
int cnss_set_pci_link(struct cnss_pci_data *pci_priv, bool link_up)
{
	return qcom_pcie_wcn_set_link(link_up);
}

static inline int __cnss_pci_prevent_l1(struct device *dev)
{
	return 0;
}

int cnss_pci_prevent_l1(struct device *dev)
{
	return 0;
}
EXPORT_SYMBOL(cnss_pci_prevent_l1);

static inline void __cnss_pci_allow_l1(struct device *dev)
{
}

void cnss_pci_allow_l1(struct device *dev)
{
}
EXPORT_SYMBOL(cnss_pci_allow_l1);

/* Literal-port note: pci_qcom.c (PCI_MSM=y) owns the real MSI assignment. On
 * the mainline substrate replicate its default 32-vector layout (MHI/CE/WAKE/DP)
 * verbatim — this is the same layout ath12k requests on this very endpoint — so
 * cnss_pci_enable_msi() has a config to pci_alloc_irq_vectors() with. Without it
 * pci_priv->msi_config stays NULL -> "msi_config is NULL!" -> probe -EINVAL. */
static struct cnss_msi_config cnss_stub_msi_config = {
	.total_vectors = 32,
	.total_users = MSI_USERS,
	.users = (struct cnss_msi_user[]) {
		{ .name = "MHI", .num_vectors = 3, .base_vector = 0 },
		{ .name = "CE", .num_vectors = 10, .base_vector = 3 },
		{ .name = "WAKE", .num_vectors = 1, .base_vector = 13 },
		{ .name = "DP", .num_vectors = 18, .base_vector = 14 },
	},
};

int cnss_pci_get_msi_assignment(struct cnss_pci_data *pci_priv)
{
	pci_priv->msi_config = &cnss_stub_msi_config;
	return 0;
}

/* v470/v471 (LITERAL Android SMMU-S1): cnss's OWN cnss_pci_get_iommu_addr() +
 * cnss_pci_init_smmu(), from pci_qcom.c (the PCI_MSM=y file we don't compile).
 * Portable standard code — no msm_pcie / no qcom fastmap driver dependency: they
 * only READ the DT (qcom,iommu-group / qcom,iommu-dma / qcom,iommu-dma-addr-pool /
 * smmu_iova_ipa) and grab the domain via mainline iommu_get_domain_for_dev().
 *
 * v471: this is the downstream `#else` branch of cnss_pci_get_iommu_addr() — it
 * reads the S1 window straight from qcom,iommu-dma-addr-pool on the iommu-group
 * node. We use it instead of the `#if` (iommu-addresses) branch DELIBERATELY:
 * the iommu-addresses form makes mainline of_iommu_get_resv_regions() carve
 * IOMMU_RESV_RESERVED holes in the generic iommu-dma domain, which REGRESSED the
 * WCN fw (both cnss AND ath12k RDDM'd at RESPOND_MEM, vs the baseline reaching
 * cal). Android's fastmap tolerates the same reservation; mainline's generic
 * dma-iommu does not. addr-pool is a plain vendor property mainline never
 * reserves, so cnss still gets the exact same window with no domain side-effect.
 * This is picking the other real downstream branch (like the PCI_MSM=n stubs),
 * not a deviation.
 */
int cnss_pci_get_iommu_addr(struct cnss_pci_data *pci_priv,
			    struct device_node *iommu_group_node)
{
	u32 addr_win[2];
	int ret;

	ret = of_property_read_u32_array(iommu_group_node,
					 "qcom,iommu-dma-addr-pool",
					 addr_win, ARRAY_SIZE(addr_win));

	pci_priv->smmu_iova_start = addr_win[0];
	pci_priv->smmu_iova_len = addr_win[1];

	return ret;
}

int cnss_pci_init_smmu(struct cnss_pci_data *pci_priv)
{
	struct pci_dev *pci_dev = pci_priv->pci_dev;
	struct cnss_plat_data *plat_priv = pci_priv->plat_priv;
	struct device_node *of_node;
	struct resource *res;
	const char *iommu_dma_type;
	int ret = 0;

	of_node = of_parse_phandle(pci_dev->dev.of_node, "qcom,iommu-group", 0);
	if (!of_node)
		return ret;

	cnss_pr_dbg("Initializing SMMU\n");

	pci_priv->iommu_domain = iommu_get_domain_for_dev(&pci_dev->dev);
	ret = of_property_read_string(of_node, "qcom,iommu-dma",
				      &iommu_dma_type);
	if (!ret && !strcmp("fastmap", iommu_dma_type)) {
		cnss_pr_dbg("Enabling SMMU S1 stage\n");
		pci_priv->smmu_s1_enable = true;
		/* v470: fastmap fault-handler registration omitted on the
		 * literal port (debug aid only; qcom helper not compiled).
		 */
	}

	ret = cnss_pci_get_iommu_addr(pci_priv, of_node);
	if (ret) {
		cnss_pr_err("Invalid SMMU size window, err = %d\n", ret);
		of_node_put(of_node);
		return ret;
	}

	cnss_pr_dbg("smmu_iova_start: %pa, smmu_iova_len: 0x%zx\n",
		    &pci_priv->smmu_iova_start,
		    pci_priv->smmu_iova_len);

	res = platform_get_resource_byname(plat_priv->plat_dev, IORESOURCE_MEM,
					   "smmu_iova_ipa");
	if (res) {
		pci_priv->smmu_iova_ipa_start = res->start;
		pci_priv->smmu_iova_ipa_current = res->start;
		pci_priv->smmu_iova_ipa_len = resource_size(res);
		cnss_pr_dbg("smmu_iova_ipa_start: %pa, smmu_iova_ipa_len: 0x%zx\n",
			    &pci_priv->smmu_iova_ipa_start,
			    pci_priv->smmu_iova_ipa_len);
	}

	pci_priv->iommu_geometry = of_property_read_bool(of_node,
							 "qcom,iommu-geometry");
	cnss_pr_dbg("iommu_geometry: %d\n", pci_priv->iommu_geometry);

	of_node_put(of_node);

	return 0;
}

int _cnss_pci_get_reg_dump(struct cnss_pci_data *pci_priv,
			   u8 *buf, u32 len)
{
	return 0;
}

void cnss_pci_update_drv_supported(struct cnss_pci_data *pci_priv)
{
	pci_priv->drv_supported = false;
}

#endif /* CONFIG_PCI_MSM */

static inline bool cnss_pci_get_drv_supported(struct cnss_pci_data *pci_priv)
{
	return pci_priv->drv_supported;
}

#if IS_ENABLED(CONFIG_ARCH_QCOM)
int cnss_pci_of_reserved_mem_device_init(struct cnss_pci_data *pci_priv);
int cnss_pci_wake_gpio_init(struct cnss_pci_data *pci_priv);
void cnss_pci_wake_gpio_deinit(struct cnss_pci_data *pci_priv);
#endif /* CONFIG_ARCH_QCOM */
#endif /* _CNSS_PCI_PLATFORM_H*/
