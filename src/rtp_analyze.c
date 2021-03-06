/*
 * Copyright (c) 2009 Sippy Software, Inc., http://www.sippysoft.com
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <sys/types.h>
#include <netinet/in.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rtpp_log.h"
#include "rtp_info.h"
#include "rtp.h"
#include "rtp_analyze.h"
#include "rtpp_math.h"
#include "rtpp_util.h"

static double
rtp_ts2dtime(uint32_t ts)
{

    return ((double)ts) / ((double)8000);
}

int
update_rtpp_stats(rtpp_log_t rlog, struct rtpp_session_stat *stat, rtp_hdr_t *header,
  struct rtp_info *rinfo, double rtime)
{
    uint32_t seq;
    uint16_t idx;
    uint32_t mask;

    seq = rinfo->seq;
    if (stat->ssrc_changes == 0) {
        assert(stat->last.pcount == 0);
        assert(stat->psent == 0);
        assert(stat->precvd == 0);
        stat->last.ssrc = rinfo->ssrc;
        stat->last.max_seq = stat->last.min_seq = seq;
        stat->last.base_ts = rinfo->ts;
        stat->last.base_rtime = rtime;
        stat->last.pcount = 1;
        stat->ssrc_changes = 1;
        idx = (seq % 131072) >> 5;
        stat->last.seen[idx] |= 1 << (rinfo->seq & 31);
        return (0);
    }
    if (stat->last.ssrc != rinfo->ssrc) {
        update_rtpp_totals(stat, stat);
        stat->last.duplicates = 0;
        memset(stat->last.seen, '\0', sizeof(stat->last.seen));
        stat->last.ssrc = rinfo->ssrc;
        stat->last.max_seq = stat->last.min_seq = seq;
        stat->last.base_ts = rinfo->ts;
        stat->last.base_rtime = rtime;
        stat->last.pcount = 1;
        stat->ssrc_changes += 1;
        if (stat->psent > 0 || stat->precvd > 0) {
            rtpp_log_write(RTPP_LOG_DBUG, rlog, "0x%.8X/%d: ssrc_changes=%u, psent=%u, precvd=%u\n",
              rinfo->ssrc, rinfo->seq, stat->ssrc_changes, stat->psent, stat->precvd);
        }
        idx = (seq % 131072) >> 5;
        stat->last.seen[idx] |= 1 << (rinfo->seq & 31);
        return (0);
    }
    seq += stat->last.seq_offset;
    if (header->m && (seq < stat->last.max_seq && (stat->last.max_seq & 0xffff) != 65535)) {
        rtpp_log_write(RTPP_LOG_DBUG, rlog, "0x%.8X/%d: seq reset last->max_seq=%u, seq=%u, m=%u\n",
          rinfo->ssrc, rinfo->seq, stat->last.max_seq, seq, header->m);
        /* Seq reset has happened. Treat it as a ssrc change */
        update_rtpp_totals(stat, stat);
        stat->last.duplicates = 0;
        memset(stat->last.seen, '\0', sizeof(stat->last.seen));
        stat->last.max_seq = stat->last.min_seq = seq;
        stat->last.base_ts = rinfo->ts;
        stat->last.base_rtime = rtime;
        stat->last.pcount = 1;
        stat->seq_res_count += 1;
        idx = (seq % 131072) >> 5;
        stat->last.seen[idx] |= 1 << (rinfo->seq & 31);
        return (0);
    }
    if (ABS(rtime - stat->last.base_rtime - rtp_ts2dtime(rinfo->ts - stat->last.base_ts)) > 0.1) {
        rtpp_log_write(RTPP_LOG_DBUG, rlog, "0x%.8X/%d: delta rtime=%f, delta ts=%f\n",
          rinfo->ssrc, rinfo->seq, rtime - stat->last.base_rtime,
          rtp_ts2dtime(rinfo->ts - stat->last.base_ts));
        stat->last.base_rtime = rtime;
    }
    if (stat->last.max_seq % 65536 < 536 && rinfo->seq > 65000) {
        /* Pre-wrap packet received after a wrap */
        seq -= 65536;
    } else if (stat->last.max_seq > 65000 && seq < stat->last.max_seq - 65000) {
        rtpp_log_write(RTPP_LOG_DBUG, rlog, "0x%.8X/%d: wrap last->max_seq=%u, seq=%u\n",
          rinfo->ssrc, rinfo->seq, stat->last.max_seq, seq);
        /* Wrap up has happened */
        stat->last.seq_offset += 65536;
        seq += 65536;
        if (stat->last.seq_offset % 131072 == 65536) {
            memset(stat->last.seen + 2048, '\0', sizeof(stat->last.seen) / 2);
        } else {
            memset(stat->last.seen, '\0', sizeof(stat->last.seen) / 2);
        }
    } else if (seq + 536 < stat->last.max_seq || seq > stat->last.max_seq + 536) {
        rtpp_log_write(RTPP_LOG_DBUG, rlog, "0x%.8X/%d: desync last->max_seq=%u, seq=%u, m=%u\n",
          rinfo->ssrc, rinfo->seq, stat->last.max_seq, seq, header->m);
        /* Desynchronization has happened. Treat it as a ssrc change */
        update_rtpp_totals(stat, stat);
        stat->last.duplicates = 0;
        memset(stat->last.seen, '\0', sizeof(stat->last.seen));
        stat->last.max_seq = stat->last.min_seq = seq;
        stat->last.pcount = 1;
        stat->desync_count += 1;
        idx = (seq % 131072) >> 5;
        stat->last.seen[idx] |= 1 << (rinfo->seq & 31);
        return (0);
    }
        /* printf("last->max_seq=%u, seq=%u, m=%u\n", stat->last.max_seq, seq, header->m);*/
    idx = (seq % 131072) >> 5;
    mask = stat->last.seen[idx];
    if (((mask >> (seq & 31)) & 1) != 0) {
        rtpp_log_write(RTPP_LOG_DBUG, rlog, "0x%.8X/%d: DUP\n",
          rinfo->ssrc, rinfo->seq);
        stat->last.duplicates += 1;
        return (0);
    }
    stat->last.seen[idx] |= 1 << (rinfo->seq & 31);
    if (seq - stat->last.max_seq != 1)
        rtpp_log_write(RTPP_LOG_DBUG, rlog, "0x%.8X/%d: delta = %d\n",
          rinfo->ssrc, rinfo->seq, seq - stat->last.max_seq);
    if (seq >= stat->last.max_seq) {
        stat->last.max_seq = seq;
        stat->last.pcount += 1;
        return (0);
    }
    if (seq >= stat->last.min_seq) {
        stat->last.pcount += 1;
        return (0);
    }
    if (stat->last.seq_offset == 0 && seq < stat->last.min_seq) {
        stat->last.min_seq = seq;
        stat->last.pcount += 1;
        rtpp_log_write(RTPP_LOG_DBUG, rlog, "0x%.8X/%d: last->min_seq=%u\n",
          rinfo->ssrc, rinfo->seq, stat->last.min_seq);
        return (0);
    }
    /* XXX something wrong with the stream */
    return (-1);
}

void
update_rtpp_totals(struct rtpp_session_stat *wstat, struct rtpp_session_stat *ostat)
{

    if (ostat != wstat) {
        ostat->psent = wstat->psent;
        ostat->precvd = wstat->precvd;
        ostat->duplicates = wstat->duplicates;
    }
    if (wstat->last.pcount == 0)
        return;
    ostat->psent += wstat->last.max_seq - wstat->last.min_seq + 1;
    ostat->precvd += wstat->last.pcount;
    ostat->duplicates += wstat->last.duplicates;
}
