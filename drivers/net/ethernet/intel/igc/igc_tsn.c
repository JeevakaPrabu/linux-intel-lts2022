// SPDX-License-Identifier: GPL-2.0
/* Copyright (c)  2019 Intel Corporation */

#include "igc.h"
#include "igc_hw.h"
#include "igc_tsn.h"

static bool is_any_launchtime(struct igc_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_tx_queues; i++) {
		struct igc_ring *ring = adapter->tx_ring[i];

		if (ring->launchtime_enable)
			return true;
	}

	return false;
}

static bool is_cbs_enabled(struct igc_adapter *adapter)
{
	int i;

	for (i = 0; i < adapter->num_tx_queues; i++) {
		struct igc_ring *ring = adapter->tx_ring[i];

		if (ring->cbs_enable)
			return true;
	}

	return false;
}

static unsigned int igc_tsn_new_flags(struct igc_adapter *adapter)
{
	unsigned int new_flags = adapter->flags & ~IGC_FLAG_TSN_ANY_ENABLED;

	if (adapter->qbv_enable)
		new_flags |= IGC_FLAG_TSN_QBV_ENABLED;

	if (is_any_launchtime(adapter))
		new_flags |= IGC_FLAG_TSN_QBV_ENABLED;

	if (adapter->frame_preemption_active)
		new_flags |= IGC_FLAG_TSN_PREEMPT_ENABLED;

	if (is_cbs_enabled(adapter))
		new_flags |= IGC_FLAG_TSN_QAV_ENABLED;

	return new_flags;
}

/* Returns the TSN specific registers to their default values after
 * the adapter is reset.
 */
static int igc_tsn_disable_offload(struct igc_adapter *adapter)
{
	struct igc_hw *hw = &adapter->hw;
	u32 tqavctrl, rxpbs;
	int i;

	adapter->add_frag_size = IGC_I225_MIN_FRAG_SIZE_DEFAULT;

	wr32(IGC_TXPBS, I225_TXPBSIZE_DEFAULT);
	wr32(IGC_DTXMXPKTSZ, IGC_DTXMXPKTSZ_DEFAULT);

	rxpbs = rd32(IGC_RXPBS) & ~IGC_RXPBSIZE_SIZE_MASK;
	rxpbs |= I225_RXPBSIZE_DEFAULT;

	wr32(IGC_RXPBS, rxpbs);

	tqavctrl = rd32(IGC_TQAVCTRL);
	tqavctrl &= ~(IGC_TQAVCTRL_TRANSMIT_MODE_TSN |
		      IGC_TQAVCTRL_ENHANCED_QAV | IGC_TQAVCTRL_FUTSCDDIS | 
		      IGC_TQAVCTRL_PREEMPT_ENA | IGC_TQAVCTRL_MIN_FRAG_MASK);

	wr32(IGC_TQAVCTRL, tqavctrl);

	for (i = 0; i < adapter->num_tx_queues; i++) {
		wr32(IGC_TXQCTL(i), 0);
		wr32(IGC_STQT(i), 0);
		wr32(IGC_ENDQT(i), NSEC_PER_SEC);
	}

	wr32(IGC_QBVCYCLET_S, 0);
	wr32(IGC_QBVCYCLET, NSEC_PER_SEC);

	adapter->flags &= ~IGC_FLAG_TSN_ANY_ENABLED;

	return 0;
}

static int igc_tsn_enable_offload(struct igc_adapter *adapter)
{
	struct igc_hw *hw = &adapter->hw;
	bool tsn_mode_reconfig = false;
	u32 tqavctrl, baset_l, baset_h;
	u32 sec, nsec, cycle, rxpbs;
	ktime_t base_time, systim;
	u32 frag_size_mult;
	int i;

	wr32(IGC_GTXOFFSET, 1500);

	wr32(IGC_TSAUXC, 0);
	wr32(IGC_DTXMXPKTSZ, IGC_DTXMXPKTSZ_TSN);
	wr32(IGC_TXPBS, IGC_TXPBSIZE_TSN);

	rxpbs = rd32(IGC_RXPBS) & ~IGC_RXPBSIZE_SIZE_MASK;
	rxpbs |= IGC_RXPBSIZE_TSN;

	wr32(IGC_RXPBS, rxpbs);

	for (i = 0; i < adapter->num_tx_queues; i++) {
		struct igc_ring *ring = adapter->tx_ring[i];
		u32 txqctl = 0;
		u16 cbs_value;
		u32 tqavcc;

		wr32(IGC_STQT(i), ring->start_time);
		wr32(IGC_ENDQT(i), ring->end_time);

		txqctl |= IGC_TXQCTL_STRICT_CYCLE |
			IGC_TXQCTL_STRICT_END;

		if (ring->launchtime_enable)
			txqctl |= IGC_TXQCTL_QUEUE_MODE_LAUNCHT;

		if (adapter->frame_preemption_active && ring->preemptible)
			txqctl |= IGC_TXQCTL_PREEMPTABLE;

		/* Skip configuring CBS for Q2 and Q3 */
		if (i > 1)
			goto skip_cbs;

		if (ring->cbs_enable) {
			if (i == 0)
				txqctl |= IGC_TXQCTL_QAV_SEL_CBS0;
			else
				txqctl |= IGC_TXQCTL_QAV_SEL_CBS1;

			/* According to i225 datasheet section 7.5.2.7, we
			 * should set the 'idleSlope' field from TQAVCC
			 * register following the equation:
			 *
			 * value = link-speed   0x7736 * BW * 0.2
			 *         ---------- *  -----------------         (E1)
			 *          100Mbps            2.5
			 *
			 * Note that 'link-speed' is in Mbps.
			 *
			 * 'BW' is the percentage bandwidth out of full
			 * link speed which can be found with the
			 * following equation. Note that idleSlope here
			 * is the parameter from this function
			 * which is in kbps.
			 *
			 *     BW =     idleSlope
			 *          -----------------                      (E2)
			 *          link-speed * 1000
			 *
			 * That said, we can come up with a generic
			 * equation to calculate the value we should set
			 * it TQAVCC register by replacing 'BW' in E1 by E2.
			 * The resulting equation is:
			 *
			 * value = link-speed * 0x7736 * idleSlope * 0.2
			 *         -------------------------------------   (E3)
			 *             100 * 2.5 * link-speed * 1000
			 *
			 * 'link-speed' is present in both sides of the
			 * fraction so it is canceled out. The final
			 * equation is the following:
			 *
			 *     value = idleSlope * 61036
			 *             -----------------                   (E4)
			 *                  2500000
			 *
			 * NOTE: For i225, given the above, we can see
			 *       that idleslope is represented in
			 *       40.959433 kbps units by the value at
			 *       the TQAVCC register (2.5Gbps / 61036),
			 *       which reduces the granularity for
			 *       idleslope increments.
			 *
			 * In i225 controller, the sendSlope and loCredit
			 * parameters from CBS are not configurable
			 * by software so we don't do any
			 * 'controller configuration' in respect to
			 * these parameters.
			 */
			cbs_value = DIV_ROUND_UP_ULL(ring->idleslope
						     * 61036ULL, 2500000);

			tqavcc = rd32(IGC_TQAVCC(i));
			tqavcc &= ~IGC_TQAVCC_IDLESLOPE_MASK;
			tqavcc |= cbs_value | IGC_TQAVCC_KEEP_CREDITS;
			wr32(IGC_TQAVCC(i), tqavcc);

			wr32(IGC_TQAVHC(i),
			     0x80000000 + ring->hicredit * 0x7736);
		} else {
			/* Disable any CBS for the queue */
			txqctl &= ~(IGC_TXQCTL_QAV_SEL_MASK);

			/* Set idleSlope to zero. */
			tqavcc = rd32(IGC_TQAVCC(i));
			tqavcc &= ~(IGC_TQAVCC_IDLESLOPE_MASK |
				    IGC_TQAVCC_KEEP_CREDITS);
			wr32(IGC_TQAVCC(i), tqavcc);

			/* Set hiCredit to zero. */
			wr32(IGC_TQAVHC(i), 0);
		}
skip_cbs:
		wr32(IGC_TXQCTL(i), txqctl);
	}

	tqavctrl = rd32(IGC_TQAVCTRL) & ~(IGC_TQAVCTRL_FUTSCDDIS |
			IGC_TQAVCTRL_MIN_FRAG_MASK |
			IGC_TQAVCTRL_PREEMPT_ENA);

	if (tqavctrl & IGC_TQAVCTRL_TRANSMIT_MODE_TSN)
		tsn_mode_reconfig = true;

	tqavctrl |= IGC_TQAVCTRL_TRANSMIT_MODE_TSN | IGC_TQAVCTRL_ENHANCED_QAV;

	if (adapter->frame_preemption_active)
                 tqavctrl |= IGC_TQAVCTRL_PREEMPT_ENA;

        frag_size_mult = ethtool_frag_size_to_mult(adapter->add_frag_size);

        tqavctrl |= frag_size_mult << IGC_TQAVCTRL_MIN_FRAG_SHIFT;

	cycle = adapter->cycle_time;
	base_time = adapter->base_time;

	nsec = rd32(IGC_SYSTIML);
	sec = rd32(IGC_SYSTIMH);

	systim = ktime_set(sec, nsec);
	if (ktime_compare(systim, base_time) > 0) {
		s64 n = div64_s64(ktime_sub_ns(systim, base_time), cycle);

		base_time = ktime_add_ns(base_time, (n + 1) * cycle);

		/* Increase the counter if scheduling into the past while
		 * Gate Control List (GCL) is running.
		 */
		if ((rd32(IGC_BASET_H) || rd32(IGC_BASET_L)) &&
		    (adapter->tc_setup_type == TC_SETUP_QDISC_TAPRIO) &&
		    tsn_mode_reconfig)
			adapter->qbv_config_change_errors++;
	} else {
		/* According to datasheet section 7.5.2.9.3.3, FutScdDis bit
		 * has to be configured before the cycle time and base time.
		 * Tx won't hang if there is a GCL is already running,
		 * so in this case we don't need to set FutScdDis.
		 */
		if (igc_is_device_id_i226(hw) &&
		    !(rd32(IGC_BASET_H) || rd32(IGC_BASET_L)))
			tqavctrl |= IGC_TQAVCTRL_FUTSCDDIS;
	}

	wr32(IGC_TQAVCTRL, tqavctrl);

	wr32(IGC_QBVCYCLET_S, cycle);
	wr32(IGC_QBVCYCLET, cycle);

	baset_h = div_s64_rem(base_time, NSEC_PER_SEC, &baset_l);
	wr32(IGC_BASET_H, baset_h);

	/* In i226, Future base time is only supported when FutScdDis bit
	 * is enabled and only active for re-configuration.
	 * In this case, initialize the base time with zero to create
	 * "re-configuration" scenario then only set the desired base time.
	 */
	if (tqavctrl & IGC_TQAVCTRL_FUTSCDDIS)
		wr32(IGC_BASET_L, 0);
	wr32(IGC_BASET_L, baset_l);

	return 0;
}

int igc_tsn_reset(struct igc_adapter *adapter)
{
	unsigned int new_flags;
	int err = 0;

	new_flags = igc_tsn_new_flags(adapter);

	if (!(new_flags & IGC_FLAG_TSN_ANY_ENABLED))
		return igc_tsn_disable_offload(adapter);

	err = igc_tsn_enable_offload(adapter);
	if (err < 0)
		return err;

	adapter->flags = new_flags;

	return err;
}

int igc_tsn_offload_apply(struct igc_adapter *adapter)
{
	struct igc_hw *hw = &adapter->hw;

	if (netif_running(adapter->netdev) && igc_is_device_id_i225(hw)) {
		schedule_work(&adapter->reset_task);
		return 0;
	}

	igc_tsn_reset(adapter);

	return 0;
}
