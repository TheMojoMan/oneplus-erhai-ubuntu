/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Compat stub for the downstream <linux/qcom-iommu-util.h>.
 *
 * cnss2 uses only the SMMU device-fault-handler registration helpers from
 * this header (a debug aid — records SMMU fault timestamps; not required for
 * cold-boot calibration). Mainline has no equivalent, so these are no-ops.
 * Documented deliberate deviation for the literal cnss2 port.
 */
#ifndef _COMPAT_LINUX_QCOM_IOMMU_UTIL_H
#define _COMPAT_LINUX_QCOM_IOMMU_UTIL_H

#include <linux/device.h>
#include <linux/iommu.h>

typedef void (*qcom_iommu_fault_handler_irq_t)(struct iommu_domain *domain,
					       void *token);

static inline int
qcom_iommu_register_device_fault_handler_irq(struct device *dev,
					     qcom_iommu_fault_handler_irq_t handler,
					     void *token)
{
	return 0;
}

static inline int
qcom_iommu_set_fault_handler_irq(struct iommu_domain *domain,
				 qcom_iommu_fault_handler_irq_t handler,
				 void *token)
{
	return 0;
}

#endif /* _COMPAT_LINUX_QCOM_IOMMU_UTIL_H */
