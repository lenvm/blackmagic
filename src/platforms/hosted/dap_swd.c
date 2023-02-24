/*
 * This file is part of the Black Magic Debug project.
 *
 * Copyright (C) 2023 1BitSquared <info@1bitsquared.com>
 * Written by Rachel Mant <git@dragonmux.network>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * 3. Neither the name of the copyright holder nor the names of its
 *    contributors may be used to endorse or promote products derived from
 *    this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "dap.h"
#include "dap_command.h"
#include "buffer_utils.h"

static uint32_t dap_swdptap_seq_in(size_t clock_cycles);
static bool dap_swdptap_seq_in_parity(uint32_t *result, size_t clock_cycles);
static void dap_swdptap_seq_out(uint32_t tms_states, size_t clock_cycles);
static void dap_swdptap_seq_out_parity(uint32_t tms_states, size_t clock_cycles);

static bool dap_write_reg_no_check(uint16_t addr, uint32_t data);
static uint32_t dap_swd_dp_error(adiv5_debug_port_s *target_dp, bool protocol_recovery);

bool dap_swdptap_init(adiv5_debug_port_s *target_dp)
{
	if (!(dap_caps & DAP_CAP_SWD))
		return false;

	DEBUG_PROBE("-> dap_swd_init(%u)\n", target_dp->dev_index);
	dap_mode = DAP_CAP_SWD;
	dap_transfer_configure(2, 128, 128);
	dap_swd_configure(0);
	dap_connect(false);
	dap_led(0, 1);
	dap_reset_link(target_dp, false);
	/* If we have SWD sequences available, make use of them */
	if (dap_has_swd_sequence)
		target_dp->dp_low_write = dap_write_reg_no_check;
	else
		target_dp->dp_low_write = NULL;
	/* Set up the underlying SWD functions using the implementation below */
	swd_proc.seq_in = dap_swdptap_seq_in;
	swd_proc.seq_in_parity = dap_swdptap_seq_in_parity;
	swd_proc.seq_out = dap_swdptap_seq_out;
	swd_proc.seq_out_parity = dap_swdptap_seq_out_parity;
	/* Set up the accelerated SWD functions for basic target operations */
	target_dp->dp_read = dap_dp_read_reg;
	target_dp->error = dap_swd_dp_error;
	target_dp->low_access = dap_dp_low_access;
	target_dp->abort = dap_dp_abort;
	return true;
}

static void dap_swdptap_seq_out(const uint32_t tms_states, const size_t clock_cycles)
{
	/* Setup the sequence */
	dap_swd_sequence_s sequence = {
		clock_cycles,
		DAP_SWD_OUT_SEQUENCE,
	};
	write_le4(sequence.data, 0, tms_states);
	/* And perform it */
	if (!perform_dap_swd_sequences(&sequence, 1U))
		DEBUG_WARN("dap_swdptap_seq_out failed\n");
}

static void dap_swdptap_seq_out_parity(const uint32_t tms_states, const size_t clock_cycles)
{
	/* Setup the sequence */
	dap_swd_sequence_s sequence = {
		clock_cycles + 1,
		DAP_SWD_OUT_SEQUENCE,
	};
	write_le4(sequence.data, 0, tms_states);
	sequence.data[4] = __builtin_parity(tms_states);
	/* And perform it */
	if (!perform_dap_swd_sequences(&sequence, 1U))
		DEBUG_WARN("dap_swdptap_seq_out_parity failed\n");
}

static uint32_t dap_swdptap_seq_in(const size_t clock_cycles)
{
	/* Setup the sequence */
	dap_swd_sequence_s sequence = {
		clock_cycles,
		DAP_SWD_IN_SEQUENCE,
	};
	/* And perform it */
	if (!perform_dap_swd_sequences(&sequence, 1U)) {
		DEBUG_WARN("dap_swdptap_seq_in failed\n");
		return 0U;
	}

	uint32_t result = 0;
	for (size_t offset = 0; offset < clock_cycles; offset += 8U)
		result |= sequence.data[offset >> 3U] << offset;
	return result;
}

static bool dap_swdptap_seq_in_parity(uint32_t *const result, const size_t clock_cycles)
{
	/* Setup the sequence */
	dap_swd_sequence_s sequence = {
		clock_cycles + 1U,
		DAP_SWD_IN_SEQUENCE,
	};
	/* And perform it */
	if (!perform_dap_swd_sequences(&sequence, 1U)) {
		DEBUG_WARN("dap_swdptap_seq_in_parity failed\n");
		return false;
	}

	uint32_t data = 0;
	for (size_t offset = 0; offset < clock_cycles; offset += 8U)
		data |= sequence.data[offset >> 3U] << offset;
	*result = data;
	uint8_t parity = __builtin_parity(data) & 1U;
	parity ^= sequence.data[4] & 1U;
	return !parity;
}

static bool dap_write_reg_no_check(uint16_t addr, const uint32_t data)
{
	DEBUG_PROBE("dap_write_reg_no_check %04x <- %08" PRIx32 "\n", addr, data);
	/* Setup the sequences */
	dap_swd_sequence_s sequences[4] = {
		/* Write the 8 byte request */
		{
			8U,
			DAP_SWD_OUT_SEQUENCE,
			{make_packet_request(ADIV5_LOW_WRITE, addr)},
		},
		/* Perform one turn-around cycle then read the 3 bit ACK */
		{4U, DAP_SWD_IN_SEQUENCE},
		/* Perform another turnaround cycle */
		{1U, DAP_SWD_OUT_SEQUENCE, {}},
		/* Now write out the 32b of data to send and the 1b of parity */
		{
			33U,
			DAP_SWD_OUT_SEQUENCE,
			/* The 4 data bytes are filled in below with write_le4() */
			{0U, 0U, 0U, 0U, __builtin_parity(data)},
		},
	};
	write_le4(sequences[3].data, 0, data);
	/* Now perform the sequences */
	if (!perform_dap_swd_sequences(sequences, 4U)) {
		DEBUG_WARN("dap_write_reg_no_check failed\n");
		return false;
	}
	/* Check the ack state */
	const uint8_t ack = (sequences[1].data[0] >> 1U) & 7U;
	return ack != SWDP_ACK_OK;
}

static uint32_t dap_swd_dp_error(adiv5_debug_port_s *const target_dp, const bool protocol_recovery)
{
	DEBUG_PROBE("dap_swd_dp_error (protocol recovery? %s)\n", protocol_recovery ? "true" : "false");
	/* Only do the comms reset dance on DPv2+ w/ fault or to perform protocol recovery. */
	if ((target_dp->version >= 2 && target_dp->fault) || protocol_recovery) {
		/*
		 * Note that on DPv2+ devices, during a protocol error condition
		 * the target becomes deselected during line reset. Once reset,
		 * we must then re-select the target to bring the device back
		 * into the expected state.
		 */
		dap_line_reset();
		if (target_dp->version >= 2)
			dap_write_reg_no_check(ADIV5_DP_TARGETSEL, target_dp->targetsel);
		dap_read_reg(target_dp, ADIV5_DP_DPIDR);
		/* Exception here is unexpected, so do not catch */
	}
	const uint32_t err = dap_read_reg(target_dp, ADIV5_DP_CTRLSTAT) &
		(ADIV5_DP_CTRLSTAT_STICKYORUN | ADIV5_DP_CTRLSTAT_STICKYCMP | ADIV5_DP_CTRLSTAT_STICKYERR |
			ADIV5_DP_CTRLSTAT_WDATAERR);
	uint32_t clr = 0;

	if (err & ADIV5_DP_CTRLSTAT_STICKYORUN)
		clr |= ADIV5_DP_ABORT_ORUNERRCLR;
	if (err & ADIV5_DP_CTRLSTAT_STICKYCMP)
		clr |= ADIV5_DP_ABORT_STKCMPCLR;
	if (err & ADIV5_DP_CTRLSTAT_STICKYERR)
		clr |= ADIV5_DP_ABORT_STKERRCLR;
	if (err & ADIV5_DP_CTRLSTAT_WDATAERR)
		clr |= ADIV5_DP_ABORT_WDERRCLR;

	if (clr)
		dap_write_reg(target_dp, ADIV5_DP_ABORT, clr);
	target_dp->fault = 0;
	return err;
}
