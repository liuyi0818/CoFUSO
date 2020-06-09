/*
 *	MPTCP implementation - Sending side
 *
 *	Initial Design & Implementation:
 *	Sébastien Barré <sebastien.barre@uclouvain.be>
 *
 *	Current Maintainer & Author:
 *	Christoph Paasch <christoph.paasch@uclouvain.be>
 *
 *	Additional authors:
 *	Jaakko Korkeaniemi <jaakko.korkeaniemi@aalto.fi>
 *	Gregory Detal <gregory.detal@uclouvain.be>
 *	Fabien Duchêne <fabien.duchene@uclouvain.be>
 *	Andreas Seelinger <Andreas.Seelinger@rwth-aachen.de>
 *	Lavkesh Lahngir <lavkesh51@gmail.com>
 *	Andreas Ripke <ripke@neclab.eu>
 *	Vlad Dogaru <vlad.dogaru@intel.com>
 *	Octavian Purdila <octavian.purdila@intel.com>
 *	John Ronan <jronan@tssg.org>
 *	Catalin Nicutar <catalin.nicutar@gmail.com>
 *	Brandon Heller <brandonh@stanford.edu>
 *
 *
 *	This program is free software; you can redistribute it and/or
 *      modify it under the terms of the GNU General Public License
 *      as published by the Free Software Foundation; either version
 *      2 of the License, or (at your option) any later version.
 */

#include <linux/kconfig.h>
#include <linux/skbuff.h>
#include <linux/tcp.h>

#include <net/mptcp.h>
#include <net/mptcp_v4.h>
#include <net/mptcp_v6.h>
#include <net/sock.h>
 /*CG add declaration*/
static struct sock *find_worst_subflow(struct sock *meta_sk);
static struct sock *find_best_subflow(struct sock *meta_sk,struct sk_buff *skb, u8 worstPathIndex);
//ly add start
static struct CodeInfo get_DataM_CodeK(struct sock *worst_sk);
static struct CodeInfo get_DataM_CodeK_for_test(struct sock *worst_sk);
static struct sk_buff *return_XOR_skb(struct sock *worst_sk, struct sock *meta_sk,struct CodeInfo codeinfo,bool *ifLastPacket);
static void __rmt_mptcp_reinject_coding(struct sk_buff *orig_skb, struct sock *meta_sk, struct sock *sk, int clone_it, struct sock *dest_sk);
static void set_flag(struct sock *worst_sk, struct CodeInfo codeinfo);
static struct sk_buff *no_coding_simple_retransmission_worstsubf_skb(struct sock *worst_sk, struct sock *meta_sk, bool *ifLastPacket);
static struct sock *find_worst_subflow_coding(struct sock *meta_sk,bool *allfusoed_flag);
static struct sock *find_worst_subflow_lossrate_coding(struct sock *meta_sk,bool *allfusoed_flag);
//ly add end
#define DEVISION_SCALER ((u32)0xFFFF)
/*CG add*/

static const int mptcp_dss_len = MPTCP_SUB_LEN_DSS_ALIGN +
				 MPTCP_SUB_LEN_ACK_ALIGN +
				 MPTCP_SUB_LEN_SEQ_ALIGN;

static inline int mptcp_sub_len_remove_addr(u16 bitfield)
{
	unsigned int c;
	for (c = 0; bitfield; c++)
		bitfield &= bitfield - 1;
	return MPTCP_SUB_LEN_REMOVE_ADDR + c - 1;
}

int mptcp_sub_len_remove_addr_align(u16 bitfield)
{
	return ALIGN(mptcp_sub_len_remove_addr(bitfield), 4);
}
EXPORT_SYMBOL(mptcp_sub_len_remove_addr_align);

/* get the data-seq and end-data-seq and store them again in the
 * tcp_skb_cb
 */
static int mptcp_reconstruct_mapping(struct sk_buff *skb)
{
	const struct mp_dss *mpdss = (struct mp_dss *)TCP_SKB_CB(skb)->dss;
	u32 *p32;
	u16 *p16;

	if (!mpdss->M)
		return 1;

	/* Move the pointer to the data-seq */
	p32 = (u32 *)mpdss;
	p32++;
	if (mpdss->A) {
		p32++;
		if (mpdss->a)
			p32++;
	}

	TCP_SKB_CB(skb)->seq = ntohl(*p32);

	/* Get the data_len to calculate the end_data_seq */
	p32++;
	p32++;
	p16 = (u16 *)p32;
	//ly add
	if(skb&&skb->ifProtectionPacket==2)
	{
		__u16 a=(__u16)skb->len + (__u16)(mptcp_is_data_fin(skb) ? 1 : 0);
		*p16=htons(a);
		//printk(KERN_EMERG "coding skb 's skb->len1 is %d\n",skb->len);
		//printk(KERN_EMERG "coding skb 's skb->len2 is %u\n",ntohs(*p16));
	}
	//ly add

	TCP_SKB_CB(skb)->end_seq = ntohs(*p16) + TCP_SKB_CB(skb)->seq;
    
    //ly add
	// if(skb&&skb->ifProtectionPacket==2)
	// {
	// 	printk(KERN_EMERG "coding skb 's seq is %u,end_seq is %u\n",TCP_SKB_CB(skb)->seq,TCP_SKB_CB(skb)->end_seq);
	// }
	//ly add
	return 0;
}

static void mptcp_find_and_set_pathmask(const struct sock *meta_sk, struct sk_buff *skb)
{
	struct sk_buff *skb_it;

	skb_it = tcp_write_queue_head(meta_sk);

	tcp_for_write_queue_from(skb_it, meta_sk) {
		if (skb_it == tcp_send_head(meta_sk))
			break;

		if (TCP_SKB_CB(skb_it)->seq == TCP_SKB_CB(skb)->seq) {
			TCP_SKB_CB(skb)->path_mask = TCP_SKB_CB(skb_it)->path_mask;
			break;
		}
	}
}

/* Reinject data from one TCP subflow to the meta_sk. If sk == NULL, we are
 * coming from the meta-retransmit-timer
 */
static void __mptcp_reinject_data(struct sk_buff *orig_skb, struct sock *meta_sk,
				  struct sock *sk, int clone_it)
{
	struct sk_buff *skb, *skb1;
	const struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct mptcp_cb *mpcb = meta_tp->mpcb;
	u32 seq, end_seq;

	if (clone_it) {
		/* pskb_copy is necessary here, because the TCP/IP-headers
		 * will be changed when it's going to be reinjected on another
		 * subflow.
		 */
		skb = pskb_copy_for_clone(orig_skb, GFP_ATOMIC);
	} else {
		__skb_unlink(orig_skb, &sk->sk_write_queue);
		sock_set_flag(sk, SOCK_QUEUE_SHRUNK);
		sk->sk_wmem_queued -= orig_skb->truesize;
		sk_mem_uncharge(sk, orig_skb->truesize);
		skb = orig_skb;
	}
	if (unlikely(!skb))
		return;

	if (sk && mptcp_reconstruct_mapping(skb)) {
		__kfree_skb(skb);
		return;
	}

	skb->sk = meta_sk;

	/* If it reached already the destination, we don't have to reinject it */
	if (!after(TCP_SKB_CB(skb)->end_seq, meta_tp->snd_una)||skb->ifProtectionPacket||skb->ifRMTProtected) {
		__kfree_skb(skb);
		return;
	}

	/* Only reinject segments that are fully covered by the mapping */
	if (skb->len + (mptcp_is_data_fin(skb) ? 1 : 0) !=
	    TCP_SKB_CB(skb)->end_seq - TCP_SKB_CB(skb)->seq) {
		u32 seq = TCP_SKB_CB(skb)->seq;
		u32 end_seq = TCP_SKB_CB(skb)->end_seq;
		
		__kfree_skb(skb);

		/* Ok, now we have to look for the full mapping in the meta
		 * send-queue :S
		 */
		tcp_for_write_queue(skb, meta_sk) {
			/* Not yet at the mapping? */
			if (before(TCP_SKB_CB(skb)->seq, seq))
				continue;
			/* We have passed by the mapping */
			if (after(TCP_SKB_CB(skb)->end_seq, end_seq))
				return;
			
			__mptcp_reinject_data(skb, meta_sk, NULL, 1);
		}
		return;
	}

	/* Segment goes back to the MPTCP-layer. So, we need to zero the
	 * path_mask/dss.
	 */
	memset(TCP_SKB_CB(skb)->dss, 0 , mptcp_dss_len);

	/* We need to find out the path-mask from the meta-write-queue
	 * to properly select a subflow.
	 */
	mptcp_find_and_set_pathmask(meta_sk, skb);

	/* If it's empty, just add */
	if (skb_queue_empty(&mpcb->reinject_queue)) {
		skb_queue_head(&mpcb->reinject_queue, skb);
		return;
	}

	/* Find place to insert skb - or even we can 'drop' it, as the
	 * data is already covered by other skb's in the reinject-queue.
	 *
	 * This is inspired by code from tcp_data_queue.
	 */

	skb1 = skb_peek_tail(&mpcb->reinject_queue);
	seq = TCP_SKB_CB(skb)->seq;
	while (1) {
		if (!after(TCP_SKB_CB(skb1)->seq, seq))
			break;
		if (skb_queue_is_first(&mpcb->reinject_queue, skb1)) {
			skb1 = NULL;
			break;
		}
		skb1 = skb_queue_prev(&mpcb->reinject_queue, skb1);
	}

	/* Do skb overlap to previous one? */
	end_seq = TCP_SKB_CB(skb)->end_seq;
	if (skb1 && before(seq, TCP_SKB_CB(skb1)->end_seq)) {
		if (!after(end_seq, TCP_SKB_CB(skb1)->end_seq)) {
			/* All the bits are present. Don't reinject */
			__kfree_skb(skb);
			return;
		}
		if (seq == TCP_SKB_CB(skb1)->seq) {
			if (skb_queue_is_first(&mpcb->reinject_queue, skb1))
				skb1 = NULL;
			else
				skb1 = skb_queue_prev(&mpcb->reinject_queue, skb1);
		}
	}
	if (!skb1)
		__skb_queue_head(&mpcb->reinject_queue, skb);
	else
		__skb_queue_after(&mpcb->reinject_queue, skb1, skb);

	/* And clean segments covered by new one as whole. */
	while (!skb_queue_is_last(&mpcb->reinject_queue, skb)) {
		skb1 = skb_queue_next(&mpcb->reinject_queue, skb);

		if (!after(end_seq, TCP_SKB_CB(skb1)->seq))
			break;

		__skb_unlink(skb1, &mpcb->reinject_queue);
		__kfree_skb(skb1);
	}
	return;
}


//CG add
static void __rmt_mptcp_reinject_data(struct sk_buff *orig_skb, struct sock *meta_sk,
				  struct sock *sk, int clone_it, struct sock *dest_sk)
{
	struct sk_buff *skb, *skb1;
	const struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct mptcp_cb *mpcb = meta_tp->mpcb;
	u32 seq, end_seq;

	// printk(KERN_EMERG "rmt  BUG: failure at %s:%d/%s()! seq = %lu , end_seq = %lu, skb(ifProtectionPacket=%d,ifRMTProtected=%d)\n"
	// 	,__FILE__, __LINE__, __FUNCTION__,TCP_SKB_CB(orig_skb)->seq,TCP_SKB_CB(orig_skb)->end_seq,orig_skb->ifProtectionPacket,orig_skb->ifRMTProtected);
	if (clone_it) {
		/* pskb_copy is necessary here, because the TCP/IP-headers
		 * will be changed when it's going to be reinjected on another
		 * subflow.
		 */
		skb = pskb_copy_for_clone(orig_skb, GFP_ATOMIC);
		if(clone_it==5217)///CG add
		{
			// printk(KERN_EMERG "rmt  BUG: failure at %s:%d/%s()! \n",__FILE__, __LINE__, __FUNCTION__);
			skb->ifRMTProtected=0;
			skb->ifProtectionPacket=1;
		}
	} else {
		__skb_unlink(orig_skb, &sk->sk_write_queue);
		sock_set_flag(sk, SOCK_QUEUE_SHRUNK);
		sk->sk_wmem_queued -= orig_skb->truesize;
		sk_mem_uncharge(sk, orig_skb->truesize);
		skb = orig_skb;
	}
	if (unlikely(!skb))
		return;

	if (sk && mptcp_reconstruct_mapping(skb)) {
		__kfree_skb(skb);
		return;
	}

	skb->sk = meta_sk;

	/* If it reached already the destination, we don't have to reinject it */
	if (!after(TCP_SKB_CB(skb)->end_seq, meta_tp->snd_una)) {
		__kfree_skb(skb);
		return;
	}

	/* Only reinject segments that are fully covered by the mapping */
	if(sysctl_mptcp_coding==0)
	{
	if (skb->len + (mptcp_is_data_fin(skb) ? 1 : 0) !=
	    TCP_SKB_CB(skb)->end_seq - TCP_SKB_CB(skb)->seq) {
		u32 seq = TCP_SKB_CB(skb)->seq;
		u32 end_seq = TCP_SKB_CB(skb)->end_seq;
		// printk(KERN_EMERG "rmt  BUG: failure at %s:%d/%s()!  len = %lu,end_seq =%lu,seq =%lu\n",__FILE__, __LINE__, __FUNCTION__,skb->len,TCP_SKB_CB(skb)->end_seq,TCP_SKB_CB(skb)->seq);
		__kfree_skb(skb);

		/* Ok, now we have to look for the full mapping in the meta
		 * send-queue :S
		 */
		tcp_for_write_queue(skb, meta_sk) {
			/* Not yet at the mapping? */
			if (before(TCP_SKB_CB(skb)->seq, seq))
				continue;
			/* We have passed by the mapping */
			if (after(TCP_SKB_CB(skb)->end_seq, end_seq))
				return;
			
			__rmt_mptcp_reinject_data(skb, meta_sk, NULL, 1, dest_sk);
		}
		return;
	}
	}
	/* Segment goes back to the MPTCP-layer. So, we need to zero the
	 * path_mask/dss.
	 */
	memset(TCP_SKB_CB(skb)->dss, 0 , mptcp_dss_len);

	/* We need to find out the path-mask from the meta-write-queue
	 * to properly select a subflow.
	 */
	// printk(KERN_EMERG "rmt before BUG: failure at %s:%d/%s()! seq = %u , end_seq = %u, skb(ifProtectionPacket=%d,ifRMTProtected=%d), skb->path_mask=0x%08x\n"
	// 	,__FILE__, __LINE__, __FUNCTION__,TCP_SKB_CB(orig_skb)->seq,TCP_SKB_CB(orig_skb)->end_seq,orig_skb->ifProtectionPacket,orig_skb->ifRMTProtected,TCP_SKB_CB(skb)->path_mask);

	mptcp_find_and_set_pathmask(meta_sk, skb);///CG comments it

	TCP_SKB_CB(skb)->path_mask=0xffffffff;
	TCP_SKB_CB(skb)->path_mask=TCP_SKB_CB(skb)->path_mask&(~mptcp_pi_to_flag(tcp_sk(dest_sk)->mptcp->path_index));
	// printk(KERN_EMERG "rmt after BUG: failure at %s:%d/%s()! seq = %u , end_seq = %u, skb(ifProtectionPacket=%d,ifRMTProtected=%d), skb->path_mask=0x%08x\n"
	// ,__FILE__, __LINE__, __FUNCTION__,TCP_SKB_CB(orig_skb)->seq,TCP_SKB_CB(orig_skb)->end_seq,orig_skb->ifProtectionPacket,orig_skb->ifRMTProtected,TCP_SKB_CB(skb)->path_mask);


	/* If it's empty, just add */
	if (skb_queue_empty(&mpcb->reinject_queue)) {
		__skb_queue_head(&mpcb->reinject_queue, skb);
		return;
	}

	/* Find place to insert skb - or even we can 'drop' it, as the
	 * data is already covered by other skb's in the reinject-queue.
	 *
	 * This is inspired by code from tcp_data_queue.
	 */

	skb1 = skb_peek_tail(&mpcb->reinject_queue);
	if(sysctl_mptcp_coding==0)
	{
		seq = TCP_SKB_CB(skb)->seq;
		while (1) {
			if (!after(TCP_SKB_CB(skb1)->seq, seq))
				break;
			if (skb_queue_is_first(&mpcb->reinject_queue, skb1)) {
				skb1 = NULL;
				break;
			}
			skb1 = skb_queue_prev(&mpcb->reinject_queue, skb1);
		}

	/* Do skb overlap to previous one? */
		end_seq = TCP_SKB_CB(skb)->end_seq;
		if (skb1 && before(seq, TCP_SKB_CB(skb1)->end_seq)) {
			if (!after(end_seq, TCP_SKB_CB(skb1)->end_seq)) {
			/* All the bits are present. Don't reinject */
				__kfree_skb(skb);
				return;
			}
			if (seq == TCP_SKB_CB(skb1)->seq) {
				if (skb_queue_is_first(&mpcb->reinject_queue, skb1))
					skb1 = NULL;
				else
					skb1 = skb_queue_prev(&mpcb->reinject_queue, skb1);
			}
		}
		if (!skb1)
			__skb_queue_head(&mpcb->reinject_queue, skb);
		else
			__skb_queue_after(&mpcb->reinject_queue, skb1, skb);

	/* And clean segments covered by new one as whole. */
		while (!skb_queue_is_last(&mpcb->reinject_queue, skb)) {
			skb1 = skb_queue_next(&mpcb->reinject_queue, skb);

			if (!after(end_seq, TCP_SKB_CB(skb1)->seq))
				break;

			__skb_unlink(skb1, &mpcb->reinject_queue);
			__kfree_skb(skb1);
		}
	}
else
     __skb_queue_after(&mpcb->reinject_queue, skb1, skb);
	return;
}
//CG add
//ly add reinject coding start
static void __rmt_mptcp_reinject_coding(struct sk_buff *skb, struct sock *meta_sk, struct sock *sk, int clone_it, struct sock *dest_sk)
{
	struct sk_buff  *skb1;
	const struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct mptcp_cb *mpcb = meta_tp->mpcb;
	u32 seq, end_seq;
	if (clone_it) 
	{
		if(skb&&clone_it==5217)///CG add
		{
			//printk(KERN_EMERG "coding information succeed,skb->ifProtectionPacket=2\n");
			skb->ifRMTProtected=0;
			skb->ifProtectionPacket=2;		
		}
	} 
	else 
	{
		__skb_unlink(skb, &sk->sk_write_queue);
		sock_set_flag(sk, SOCK_QUEUE_SHRUNK);
		sk->sk_wmem_queued -= skb->truesize;
		sk_mem_uncharge(sk, skb->truesize);
	}
	if (unlikely(!skb))
	{
		//printk(KERN_EMERG "coding skb unlikely!!!\n");
		return;
	}
	//skb->sk = meta_sk;
	//ly add ,I don't kown this function.it seems we have to update our dataseq.
	//printk(KERN_EMERG "before reconstruct mapping seq is %u,end_seq is %u\n",TCP_SKB_CB(skb)->seq,TCP_SKB_CB(skb)->end_seq);
	if (sk && mptcp_reconstruct_mapping(skb)) {
		//printk(KERN_EMERG "coding  BUG: mptcp_reconstruct_mapping failure!!!!!\n");
		__kfree_skb(skb); 
		return;
	}//ly add again ,we don't use the mapping and DSS*/
	/* ly add now we don't need to use it,but we can use when we add the mptcp head start
	if (!after(TCP_SKB_CB(skb)->end_seq, meta_tp->snd_una)) {
		__kfree_skb(skb);
		return;
	}ly add end*/
	//printk(KERN_EMERG "afer reconstruct mapping seq is %u,end_seq is %u\n",TCP_SKB_CB(skb)->seq,TCP_SKB_CB(skb)->end_seq);
	skb->sk = meta_sk;
	skb->csum = skb_checksum(skb, 0, skb->len, 0);
	//if (!after(TCP_SKB_CB(skb)->end_seq, meta_tp->snd_una)) {
	//	printk(KERN_EMERG "no use end seq we have to change!!!!!\n");
		//__kfree_skb(skb);
		//return;
	//}
	// if(skb->len + (mptcp_is_data_fin(skb) ? 1 : 0) !=
	//     TCP_SKB_CB(skb)->end_seq - TCP_SKB_CB(skb)->seq)
	// {
	// 	printk(KERN_EMERG "the skb is xor right ,skb->len became large!\n");
	// }

	memset(TCP_SKB_CB(skb)->dss, 0 , mptcp_dss_len);
	mptcp_find_and_set_pathmask(meta_sk, skb);///CG comments it

	TCP_SKB_CB(skb)->path_mask=0xffffffff;
	TCP_SKB_CB(skb)->path_mask=TCP_SKB_CB(skb)->path_mask&(~mptcp_pi_to_flag(tcp_sk(dest_sk)->mptcp->path_index));
	/* If it's empty, just add */
	if (skb_queue_empty(&mpcb->reinject_queue)) {
		__skb_queue_head(&mpcb->reinject_queue, skb);
		//printk(KERN_EMERG "111we put the xor packet in the head of queue!!!\n");
		return;
	}
	// ly delete some check
	skb1 = skb_peek_tail(&mpcb->reinject_queue);
	//seq = TCP_SKB_CB(skb)->seq;
	//if (!skb1)
	//{
		//__skb_queue_head(&mpcb->reinject_queue, skb);
		//printk(KERN_EMERG "222we put the xor packet in the head of queue!!!\n");
	//}
	
	//else
	//{
		__skb_queue_after(&mpcb->reinject_queue, skb1, skb);
		//printk(KERN_EMERG "we put the xor packet in the tail of queue!!!\n");
	//}
	// ly delete,we don't need to check the specific place,just reinject in the tail

	return;

}
//ly add reinject coding end
/* Inserts data into the reinject queue */
void mptcp_reinject_data(struct sock *sk, int clone_it)
{
	struct sk_buff *skb_it, *tmp;
	struct tcp_sock *tp = tcp_sk(sk);
	struct sock *meta_sk = tp->meta_sk;

	/* It has already been closed - there is really no point in reinjecting */
	if (meta_sk->sk_state == TCP_CLOSE)
		return;

	skb_queue_walk_safe(&sk->sk_write_queue, skb_it, tmp) {
		struct tcp_skb_cb *tcb = TCP_SKB_CB(skb_it);
		/* Subflow syn's and fin's are not reinjected.
		 *
		 * As well as empty subflow-fins with a data-fin.
		 * They are reinjected below (without the subflow-fin-flag)
		 */
		if (tcb->tcp_flags & TCPHDR_SYN ||
		    (tcb->tcp_flags & TCPHDR_FIN && !mptcp_is_data_fin(skb_it)) ||
		    (tcb->tcp_flags & TCPHDR_FIN && mptcp_is_data_fin(skb_it) && !skb_it->len))
			continue;

		__mptcp_reinject_data(skb_it, meta_sk, sk, clone_it);
	}

	skb_it = tcp_write_queue_tail(meta_sk);
	/* If sk has sent the empty data-fin, we have to reinject it too. */
	if (skb_it && mptcp_is_data_fin(skb_it) && skb_it->len == 0 &&
	    TCP_SKB_CB(skb_it)->path_mask & mptcp_pi_to_flag(tp->mptcp->path_index)) {
		__mptcp_reinject_data(skb_it, meta_sk, NULL, 1);
	}
 
	tp->pf = 1;

	mptcp_push_pending_frames(meta_sk);
}
EXPORT_SYMBOL(mptcp_reinject_data);

// put the skb in reinject queue  change from mptcp_reinject_data
bool rmt_mptcp_reinject_data(struct sock *sk, int clone_it,struct sk_buff *skb_it, struct sock *dest_sk,bool ifcoding)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct sock *meta_sk = tp->meta_sk;

	/* It has already been closed - there is really no point in reinjecting */
	if (meta_sk->sk_state == TCP_CLOSE)
		return 1;

    if(ifcoding==false)
    {
    	__rmt_mptcp_reinject_data(skb_it, meta_sk, sk, clone_it, dest_sk);
    }
	else
	{
		__rmt_mptcp_reinject_coding(skb_it, meta_sk, sk, clone_it, dest_sk);
	}	
	return 0;
}

static void mptcp_combine_dfin(const struct sk_buff *skb,
			       const struct sock *meta_sk,
			       struct sock *subsk)
{
	const struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	const struct mptcp_cb *mpcb = meta_tp->mpcb;

	/* In infinite mapping we always try to combine */
	if (mpcb->infinite_mapping_snd)
		goto combine;

	/* Don't combine, if they didn't combine when closing - otherwise we end
	 * up in TIME_WAIT, even if our app is smart enough to avoid it.
	 */
	if (!mptcp_sk_can_recv(meta_sk) && !mpcb->dfin_combined)
		return;

	/* Don't combine if there is still outstanding data that remains to be
	 * DATA_ACKed, because otherwise we may never be able to deliver this.
	 */
	if (meta_tp->snd_una != TCP_SKB_CB(skb)->seq)
		return;

combine:
	if (tcp_close_state(subsk)) {
		subsk->sk_shutdown |= SEND_SHUTDOWN;
		TCP_SKB_CB(skb)->tcp_flags |= TCPHDR_FIN;
	}
}

static int mptcp_write_dss_mapping(const struct tcp_sock *tp, const struct sk_buff *skb,
				   __be32 *ptr)
{
	const struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);
	__be32 *start = ptr;
	__u16 data_len;

	*ptr++ = htonl(tcb->seq); /* data_seq */

	/* If it's a non-data DATA_FIN, we set subseq to 0 (draft v7) */
	if (mptcp_is_data_fin(skb) && skb->len == 0)
		*ptr++ = 0; /* subseq */
	else
		*ptr++ = htonl(tp->write_seq - tp->mptcp->snt_isn); /* subseq */

	if (tcb->mptcp_flags & MPTCPHDR_INF)
		data_len = 0;
	else
		data_len = tcb->end_seq - tcb->seq;

	if (tp->mpcb->dss_csum && data_len) {
		__be16 *p16 = (__be16 *)ptr;
		__be32 hdseq = mptcp_get_highorder_sndbits(skb, tp->mpcb);
		__wsum csum;

		*ptr = htonl(((data_len) << 16) |
			     (TCPOPT_EOL << 8) |
			     (TCPOPT_EOL));
		csum = csum_partial(ptr - 2, 12, skb->csum);
		p16++;
		*p16++ = csum_fold(csum_partial(&hdseq, sizeof(hdseq), csum));
	} else {
		*ptr++ = htonl(((data_len) << 16) |
			       (TCPOPT_NOP << 8) |
			       (TCPOPT_NOP));
	}

	return ptr - start;
}

static int mptcp_write_dss_data_ack(const struct tcp_sock *tp, const struct sk_buff *skb,
				    __be32 *ptr)
{
	struct mp_dss *mdss = (struct mp_dss *)ptr;
	__be32 *start = ptr;

	mdss->kind = TCPOPT_MPTCP;
	mdss->sub = MPTCP_SUB_DSS;
	mdss->rsv1 = 0;
	mdss->rsv2 = 0;
	mdss->F = mptcp_is_data_fin(skb) ? 1 : 0;
	mdss->m = 0;
	mdss->M = mptcp_is_data_seq(skb) ? 1 : 0;
	mdss->a = 0;
	mdss->A = 1;
	mdss->len = mptcp_sub_len_dss(mdss, tp->mpcb->dss_csum);
	ptr++;

	*ptr++ = htonl(mptcp_meta_tp(tp)->rcv_nxt);

	return ptr - start;
}

/* RFC6824 states that once a particular subflow mapping has been sent
 * out it must never be changed. However, packets may be split while
 * they are in the retransmission queue (due to SACK or ACKs) and that
 * arguably means that we would change the mapping (e.g. it splits it,
 * our sends out a subset of the initial mapping).
 *
 * Furthermore, the skb checksum is not always preserved across splits
 * (e.g. mptcp_fragment) which would mean that we need to recompute
 * the DSS checksum in this case.
 *
 * To avoid this we save the initial DSS mapping which allows us to
 * send the same DSS mapping even for fragmented retransmits.
 */
static void mptcp_save_dss_data_seq(const struct tcp_sock *tp, struct sk_buff *skb)
{
	struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);
	__be32 *ptr = (__be32 *)tcb->dss;

	tcb->mptcp_flags |= MPTCPHDR_SEQ;

	ptr += mptcp_write_dss_data_ack(tp, skb, ptr);
	ptr += mptcp_write_dss_mapping(tp, skb, ptr);
}

/* Write the saved DSS mapping to the header */
static int mptcp_write_dss_data_seq(const struct tcp_sock *tp, struct sk_buff *skb,
				    __be32 *ptr)
{
	__be32 *start = ptr;

	memcpy(ptr, TCP_SKB_CB(skb)->dss, mptcp_dss_len);

	/* update the data_ack */
	start[1] = htonl(mptcp_meta_tp(tp)->rcv_nxt);

	/* dss is in a union with inet_skb_parm and
	 * the IP layer expects zeroed IPCB fields.
	 */
	memset(TCP_SKB_CB(skb)->dss, 0 , mptcp_dss_len);

	return mptcp_dss_len/sizeof(*ptr);
}

/*mengyuan just note*/
static bool mptcp_skb_entail(struct sock *sk, struct sk_buff *skb, int reinject)
{
	struct tcp_sock *tp = tcp_sk(sk);
	const struct sock *meta_sk = mptcp_meta_sk(sk);
	const struct mptcp_cb *mpcb = tp->mpcb;
	struct tcp_skb_cb *tcb;
	struct sk_buff *subskb = NULL;

	if (!reinject)
		TCP_SKB_CB(skb)->mptcp_flags |= (mpcb->snd_hiseq_index ?
						  MPTCPHDR_SEQ64_INDEX : 0);

	subskb = pskb_copy_for_clone(skb, GFP_ATOMIC);

	if (!subskb)
		return false;

	//CG add
	// __u16 bestsknum = sk->sk_num;
	// u16 bestskdport = ntohs(sk->sk_dport);
	// char srcIP[16],dstIP[16];
	// memset(srcIP,'\0',16);
	// memset(dstIP,'\0',16);
	// sprintf(srcIP,"%d.%d.%d.%d",NIPQUAD(sk->sk_rcv_saddr));
	// sprintf(dstIP,"%d.%d.%d.%d",NIPQUAD(sk->sk_daddr));
	// printk(KERN_EMERG "mptcp_skb_entail (%s--->%s) data level bestsknum=%u,bestskdport=%u seq = %lu , end_seq = %lu, skb(ifProtectionPacket=%d,ifRMTProtected=%d)"
 //    ,srcIP,dstIP,bestsknum,bestskdport,TCP_SKB_CB(subskb)->seq,TCP_SKB_CB(subskb)->end_seq,subskb->ifProtectionPacket,subskb->ifRMTProtected);

	/* At the subflow-level we need to call again tcp_init_tso_segs. We
	 * force this, by setting gso_segs to 0. It has been set to 1 prior to
	 * the call to mptcp_skb_entail.
	 */
	skb_shinfo(subskb)->gso_segs = 0;

	// printk(KERN_EMERG "rmt  BUG: failure at %s:%d/%s()! \n",__FILE__, __LINE__, __FUNCTION__);
	// printk(KERN_EMERG "before set mask path_index_to_flag=%d, skb->path_mask=0x%08x \n",mptcp_pi_to_flag(tp->mptcp->path_index),TCP_SKB_CB(skb)->path_mask);
	
	TCP_SKB_CB(skb)->path_mask |= mptcp_pi_to_flag(tp->mptcp->path_index);

	// printk(KERN_EMERG "rmt  BUG: failure at %s:%d/%s()! \n",__FILE__, __LINE__, __FUNCTION__);
	// printk(KERN_EMERG "after set mask path_index_to_flag=%d, skb->path_mask=0x%08x \n",mptcp_pi_to_flag(tp->mptcp->path_index),TCP_SKB_CB(skb)->path_mask);

	if (!(sk->sk_route_caps & NETIF_F_ALL_CSUM) &&
	    skb->ip_summed == CHECKSUM_PARTIAL) {
		subskb->csum = skb->csum = skb_checksum(skb, 0, skb->len, 0);
		subskb->ip_summed = skb->ip_summed = CHECKSUM_NONE;
	}

	tcb = TCP_SKB_CB(subskb);

	if (tp->mpcb->send_infinite_mapping &&
	    !tp->mpcb->infinite_mapping_snd &&
	    !before(tcb->seq, mptcp_meta_tp(tp)->snd_nxt)) {
		tp->mptcp->fully_established = 1;
		tp->mpcb->infinite_mapping_snd = 1;
		tp->mptcp->infinite_cutoff_seq = tp->write_seq;
		tcb->mptcp_flags |= MPTCPHDR_INF;
	}

	if (mptcp_is_data_fin(subskb))
		mptcp_combine_dfin(subskb, meta_sk, sk);

	mptcp_save_dss_data_seq(tp, subskb);

	tcb->seq = tp->write_seq;
	tcb->sacked = 0; /* reset the sacked field: from the point of view
			  * of this subflow, we are sending a brand new
			  * segment
			  */
	/* Take into account seg len */
	tp->write_seq += subskb->len + ((tcb->tcp_flags & TCPHDR_FIN) ? 1 : 0);
	tcb->end_seq = tp->write_seq;
    //ly add
 //    if(subskb->ifProtectionPacket==2)
	// {
	// 	printk(KERN_EMERG "subskb->len is %d\n",subskb->len);
	// 	printk(KERN_EMERG "subskb->seq is %u,subskb->end_seq is %u\n",tcb->seq,tcb->end_seq);
	// }
	//ly add
	/* If it's a non-payload DATA_FIN (also no subflow-fin), the
	 * segment is not part of the subflow but on a meta-only-level.
	 */
	if (!mptcp_is_data_fin(subskb) || tcb->end_seq != tcb->seq) {

		//CG add
		// printk(KERN_EMERG "rmt  BUG: failure at %s:%d/%s()! \n",__FILE__, __LINE__, __FUNCTION__);
		//ly add
	    tp->count_skb=tp->count_skb+1;
	    mptcp_meta_tp(tp)->count_skb=mptcp_meta_tp(tp)->count_skb+1;
	    subskb->counted_skb=mptcp_meta_tp(tp)->count_skb;
	    skb->counted_skb=mptcp_meta_tp(tp)->count_skb;
	    //printk(KERN_EMERG "mptcp_meta_tp(tp)->count_skb=%u,tp->count_skb=%u,skb->counted_skb=%u,subskb->counted_skb=%u\n",mptcp_meta_tp(tp)->count_skb,tp->count_skb,skb->counted_skb,subskb->counted_skb);
	    // ly add
	    // if(tp->mptcp)
	    // 	printk(KERN_EMERG "entail tp->mptcp->path_index=%u\n",tp->mptcp->path_index);

		 __u16 bestsknum = sk->sk_num;
		 u16 bestskdport = ntohs(sk->sk_dport);
		// printk(KERN_EMERG "mptcp_skb_entail data level bestsknum=%u,bestskdport=%u seq = %lu , end_seq = %lu, skb(ifProtectionPacket=%d,ifRMTProtected=%d)"
	    //,bestsknum,bestskdport,TCP_SKB_CB(subskb)->seq,TCP_SKB_CB(subskb)->end_seq,subskb->ifProtectionPacket,subskb->ifRMTProtected);
	    //ly add
		
		 ////CG add for clear ifAllprotected tag
		if(subskb->ifProtectionPacket==0 && subskb->ifRMTProtected==0)
		{
			tp->ifAllProtected=0;
			//printk(KERN_EMERG "in skb mptcp,entail,we  clear,ifAllprotected tag,seq=%u.end =%u,skb->ifProtectionPacket=%d,skb->ifRMTProtected=%d"
			//,TCP_SKB_CB(subskb)->seq,TCP_SKB_CB(subskb)->end_seq,subskb->ifProtectionPacket,subskb->ifRMTProtected);
		}
		//ly add 20190428
	    if(subskb->ifProtectionPacket==0 && (skb->ifRMTProtected==0||skb->ifRMTProtected==2)) 
	    {
		    tp->ifAllFusoProtected=0;
		   // printk(KERN_EMERG "in skb mptcp entail,we  clear,ifAllprotected tag,seq=%u.end =%u,skb->ifProtectionPacket=%d,skb->ifRMTProtected=%d"
			//,TCP_SKB_CB(subskb)->seq,TCP_SKB_CB(subskb)->end_seq,subskb->ifProtectionPacket,subskb->ifRMTProtected);
	    }
	    //ly add 20190428

		tcp_add_write_queue_tail(sk, subskb);
		sk->sk_wmem_queued += subskb->truesize;
		sk_mem_charge(sk, subskb->truesize);
	} else {
		int err;

		/* Necessary to initialize for tcp_transmit_skb. mss of 1, as
		 * skb->len = 0 will force tso_segs to 1.
		 */
		tcp_init_tso_segs(sk, subskb, 1);
		/* Empty data-fins are sent immediatly on the subflow */

		err = tcp_transmit_skb(sk, subskb, 1, GFP_ATOMIC);

		/* It has not been queued, we can free it now. */
		kfree_skb(subskb);

		if (err)
			return false;
	}

	if (!tp->mptcp->fully_established) {
		tp->mptcp->second_packet = 1;
		tp->mptcp->last_end_data_seq = TCP_SKB_CB(skb)->end_seq;
	}

	return true;
}
/*mengyuan just note ends*/

/* Fragment an skb and update the mptcp meta-data. Due to reinject, we
 * might need to undo some operations done by tcp_fragment.
 */
static int mptcp_fragment(struct sock *meta_sk, struct sk_buff *skb, u32 len,
			  gfp_t gfp, int reinject)
{
	int ret, diff, old_factor;
	struct sk_buff *buff;
	u8 flags;

	if (skb_headlen(skb) < len)
		diff = skb->len - len;
	else
		diff = skb->data_len;
	old_factor = tcp_skb_pcount(skb);

	/* The mss_now in tcp_fragment is used to set the tso_segs of the skb.
	 * At the MPTCP-level we do not care about the absolute value. All we
	 * care about is that it is set to 1 for accurate packets_out
	 * accounting.
	 */
	ret = tcp_fragment(meta_sk, skb, len, UINT_MAX, gfp);
	if (ret)
		return ret;

	buff = skb->next;

	flags = TCP_SKB_CB(skb)->mptcp_flags;
	TCP_SKB_CB(skb)->mptcp_flags = flags & ~(MPTCPHDR_FIN);
	TCP_SKB_CB(buff)->mptcp_flags = flags;
	TCP_SKB_CB(buff)->path_mask = TCP_SKB_CB(skb)->path_mask;

	/* If reinject == 1, the buff will be added to the reinject
	 * queue, which is currently not part of memory accounting. So
	 * undo the changes done by tcp_fragment and update the
	 * reinject queue. Also, undo changes to the packet counters.
	 */
	if (reinject == 1) {
		int undo = buff->truesize - diff;
		meta_sk->sk_wmem_queued -= undo;
		sk_mem_uncharge(meta_sk, undo);

		tcp_sk(meta_sk)->mpcb->reinject_queue.qlen++;
		meta_sk->sk_write_queue.qlen--;

		if (!before(tcp_sk(meta_sk)->snd_nxt, TCP_SKB_CB(buff)->end_seq)) {
			undo = old_factor - tcp_skb_pcount(skb) -
				tcp_skb_pcount(buff);
			if (undo)
				tcp_adjust_pcount(meta_sk, skb, -undo);
		}
	}

	return 0;
}

/* Inspired by tcp_write_wakeup */
int mptcp_write_wakeup(struct sock *meta_sk)
{
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct sk_buff *skb;
	struct sock *sk_it;
	int ans = 0;

	if (meta_sk->sk_state == TCP_CLOSE)
		return -1;

	skb = tcp_send_head(meta_sk);
	if (skb &&
	    before(TCP_SKB_CB(skb)->seq, tcp_wnd_end(meta_tp))) {
		unsigned int mss;
		unsigned int seg_size = tcp_wnd_end(meta_tp) - TCP_SKB_CB(skb)->seq;
		struct sock *subsk = meta_tp->mpcb->sched_ops->get_subflow(meta_sk, skb, true);
		struct tcp_sock *subtp;
		if (!subsk)
			goto window_probe;
		subtp = tcp_sk(subsk);
		mss = tcp_current_mss(subsk);

		seg_size = min(tcp_wnd_end(meta_tp) - TCP_SKB_CB(skb)->seq,
			       tcp_wnd_end(subtp) - subtp->write_seq);

		if (before(meta_tp->pushed_seq, TCP_SKB_CB(skb)->end_seq))
			meta_tp->pushed_seq = TCP_SKB_CB(skb)->end_seq;

		/* We are probing the opening of a window
		 * but the window size is != 0
		 * must have been a result SWS avoidance ( sender )
		 */
		if (seg_size < TCP_SKB_CB(skb)->end_seq - TCP_SKB_CB(skb)->seq ||
		    skb->len > mss) {
			seg_size = min(seg_size, mss);
			TCP_SKB_CB(skb)->tcp_flags |= TCPHDR_PSH;
			if (mptcp_fragment(meta_sk, skb, seg_size,
					   GFP_ATOMIC, 0))
				return -1;
		} else if (!tcp_skb_pcount(skb)) {
			/* see mptcp_write_xmit on why we use UINT_MAX */
			tcp_set_skb_tso_segs(meta_sk, skb, UINT_MAX);
		}

		TCP_SKB_CB(skb)->tcp_flags |= TCPHDR_PSH;
		if (!mptcp_skb_entail(subsk, skb, 0))
			return -1;
		skb_mstamp_get(&skb->skb_mstamp);

		mptcp_check_sndseq_wrap(meta_tp, TCP_SKB_CB(skb)->end_seq -
						 TCP_SKB_CB(skb)->seq);
		tcp_event_new_data_sent(meta_sk, skb);

		__tcp_push_pending_frames(subsk, mss, TCP_NAGLE_PUSH);

		return 0;
	} else {
window_probe:
		if (between(meta_tp->snd_up, meta_tp->snd_una + 1,
			    meta_tp->snd_una + 0xFFFF)) {
			mptcp_for_each_sk(meta_tp->mpcb, sk_it) {
				if (mptcp_sk_can_send_ack(sk_it))
					tcp_xmit_probe_skb(sk_it, 1);
			}
		}

		/* At least one of the tcp_xmit_probe_skb's has to succeed */
		mptcp_for_each_sk(meta_tp->mpcb, sk_it) {
			int ret;

			if (!mptcp_sk_can_send_ack(sk_it))
				continue;

			ret = tcp_xmit_probe_skb(sk_it, 0);
			if (unlikely(ret > 0))
				ans = ret;
		}
		return ans;
	}
}

/*mengyuan from this line to mptcp_write_xmit is added by mengyuan*/
/* the functions of mptcp,just from another file*/
static DEFINE_SPINLOCK(mptcp_sched_list_lock);
static LIST_HEAD(mptcp_sched_list);

struct defsched_priv {
	u32	last_rbuf_opti;
};
static struct defsched_priv *defsched_get_priv(const struct tcp_sock *tp)
{
	printk(KERN_EMERG "rmt  BUG: failure at %s:%d/%s()! \n",__FILE__, __LINE__, __FUNCTION__);
	return (struct defsched_priv *)&tp->mptcp->mptcp_sched[0];
}

static bool mptcp_is_def_unavailable(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	/* Set of states for which we are allowed to send data */
	if (!mptcp_sk_can_send(sk))
		return true;

	/* We do not send data on this subflow unless it is
	 * fully established, i.e. the 4th ack has been received.
	 */
	if (tp->mptcp->pre_established)
		return true;

	if (tp->pf)
		return true;

	return false;
}

static bool mptcp_is_temp_unavailable(struct sock *sk,
				      const struct sk_buff *skb,
				      bool zero_wnd_test)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	unsigned int mss_now, space, in_flight;

	if (inet_csk(sk)->icsk_ca_state == TCP_CA_Loss) {
		/* If SACK is disabled, and we got a loss, TCP does not exit
		 * the loss-state until something above high_seq has been
		 * acked. (see tcp_try_undo_recovery)
		 *
		 * high_seq is the snd_nxt at the moment of the RTO. As soon
		 * as we have an RTO, we won't push data on the subflow.
		 * Thus, snd_una can never go beyond high_seq.
		 */
		if (!tcp_is_reno(tp))
			return true;
		else if (tp->snd_una != tp->high_seq)
			return true;
	}

	if (!tp->mptcp->fully_established) {
		/* Make sure that we send in-order data */
		if (skb && tp->mptcp->second_packet &&
		    tp->mptcp->last_end_data_seq != TCP_SKB_CB(skb)->seq)
			return true;
	}

	/* If TSQ is already throttling us, do not send on this subflow. When
	 * TSQ gets cleared the subflow becomes eligible again.
	 */
	if (test_bit(TSQ_THROTTLED, &tp->tsq_flags))
		return true;

	in_flight = tcp_packets_in_flight(tp);
	/* Not even a single spot in the cwnd */
	if (in_flight >= tp->snd_cwnd)
		return true;

	/* Now, check if what is queued in the subflow's send-queue
	 * already fills the cwnd.
	 */
	space = (tp->snd_cwnd - in_flight) * tp->mss_cache;

	if (tp->write_seq - tp->snd_nxt > space)
		return true;

	if (zero_wnd_test && !before(tp->write_seq, tcp_wnd_end(tp)))
		return true;

	mss_now = tcp_current_mss(sk);
	//ly add
	if(skb&&skb->ifProtectionPacket==2)
	{
		mss_now=mss_now+24;
	}
	//ly add

	/* Don't send on this subflow if we bypass the allowed send-window at
	 * the per-subflow level. Similar to tcp_snd_wnd_test, but manually
	 * calculated end_seq (because here at this point end_seq is still at
	 * the meta-level).
	 */
	if (skb && !zero_wnd_test &&
	    after(tp->write_seq + min(skb->len, mss_now), tcp_wnd_end(tp)))
		return true;

	return false;
}

/* Is the sub-socket sk available to send the skb? */
static bool mptcp_is_available(struct sock *sk, const struct sk_buff *skb,
			       bool zero_wnd_test)
{
	return !mptcp_is_def_unavailable(sk) &&
	       !mptcp_is_temp_unavailable(sk, skb, zero_wnd_test);
}


//find the skb needed to be ifProtectionPacket
static struct sk_buff *simple_retransmission_worstsubf_skb(struct sock *worst_sk, struct sock *meta_sk, bool *ifLastPacket){
    struct tcp_sock *worst_tp = tcp_sk(worst_sk);
    struct sk_buff *skb = tcp_write_queue_head(worst_sk);

	tcp_for_write_queue_from(skb, worst_sk) {     
   //  	printk(KERN_EMERG "rmt -----simple_retransmission_worstsubf_skb walk round: seq = %u ,end_seq = %u, snd_una = %u, ifProtectionPacket=%d, ifRMTProtected=%d, skb->path_mask=0x%08x\n"
			// ,TCP_SKB_CB(skb)->seq,TCP_SKB_CB(skb)->end_seq,worst_tp->snd_una,skb->ifProtectionPacket,skb->ifRMTProtected,TCP_SKB_CB(skb)->path_mask);    

		if (TCP_SKB_CB(skb)->sacked & TCPCB_SACKED_ACKED)   // /* Short-circut when first non-SACKed skb has been checked */  
		{ 
			// printk(KERN_EMERG "rmt -----simple_retransmission_sacked");
			continue;  
		}

		if (skb->ifRMTProtected || skb->ifProtectionPacket)
		{ 
			// printk(KERN_EMERG "rmt -----simple_retransmission_protected_or_rmted");
			continue;  
		}

		struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);
		/* Subflow syn's and fin's are not reinjected.
			*
			* As well as empty subflow-fins with a data-fin.
			* They are reinjected below (without the subflow-fin-flag)
			*/
		if (tcb->tcp_flags & TCPHDR_SYN ||
		(tcb->tcp_flags & TCPHDR_FIN && !mptcp_is_data_fin(skb)) ||
		(tcb->tcp_flags & TCPHDR_FIN && mptcp_is_data_fin(skb) && !skb->len))
		{
			// printk(KERN_EMERG "rmt -----simple_retransmission_syn's and fin's");
			continue;     
		}    
			       	
		if(!before(TCP_SKB_CB(skb)->end_seq, worst_tp->snd_una))   //确定要重发的包
		{
			if(skb->next == (struct sk_buff *)(&(worst_sk)->sk_write_queue))
			{
				// printk(KERN_EMERG "rmt -----simple_retransmission_ last skb get");
				*ifLastPacket=true;
			}
		  	return skb;
		}    
		// printk(KERN_EMERG "rmt -----simple_retransmission_ before una");
    } 

    worst_tp->ifAllProtected=1;
    // printk(KERN_EMERG "-----no coding simple_retransmission_worstsubf_skb ifAllProtected=%d,ifAllFusoProtected=%d\n",worst_tp->ifAllProtected,worst_tp->ifAllFusoProtected);
    // printk(KERN_EMERG "rmt  BUG: failure at %s:%d/%s()! \n",__FILE__, __LINE__, __FUNCTION__);
    // printk(KERN_EMERG "-----no simple_retransmission_worstsubf_skb  flight = %lu, ifAllProtected=%d\n",in_flight,worst_tp->ifAllProtected);
    return NULL;
}
//ly add 20190428
static struct sk_buff *no_coding_simple_retransmission_worstsubf_skb(struct sock *worst_sk, struct sock *meta_sk, bool *ifLastPacket){
    struct tcp_sock *worst_tp = tcp_sk(worst_sk);
    struct sk_buff *skb = tcp_write_queue_head(worst_sk);

	tcp_for_write_queue_from(skb, worst_sk) {     
   //  	printk(KERN_EMERG "rmt -----simple_retransmission_worstsubf_skb walk round: seq = %u ,end_seq = %u, snd_una = %u, ifProtectionPacket=%d, ifRMTProtected=%d, skb->path_mask=0x%08x\n"
			// ,TCP_SKB_CB(skb)->seq,TCP_SKB_CB(skb)->end_seq,worst_tp->snd_una,skb->ifProtectionPacket,skb->ifRMTProtected,TCP_SKB_CB(skb)->path_mask);    

		if (TCP_SKB_CB(skb)->sacked & TCPCB_SACKED_ACKED)   // /* Short-circut when first non-SACKed skb has been checked */  
		{ 
			// printk(KERN_EMERG "rmt -----simple_retransmission_sacked");
			continue;  
		}

		if (skb->ifRMTProtected==1|| skb->ifProtectionPacket)
		{ 
			// printk(KERN_EMERG "rmt -----simple_retransmission_protected_or_rmted");
			continue;  
		}

		struct tcp_skb_cb *tcb = TCP_SKB_CB(skb);
		/* Subflow syn's and fin's are not reinjected.
			*
			* As well as empty subflow-fins with a data-fin.
			* They are reinjected below (without the subflow-fin-flag)
			*/
		if (tcb->tcp_flags & TCPHDR_SYN ||
		(tcb->tcp_flags & TCPHDR_FIN && !mptcp_is_data_fin(skb)) ||
		(tcb->tcp_flags & TCPHDR_FIN && mptcp_is_data_fin(skb) && !skb->len))
		{
			// printk(KERN_EMERG "rmt -----simple_retransmission_syn's and fin's");
			continue;     
		}    
			       	
		if(!before(TCP_SKB_CB(skb)->end_seq, worst_tp->snd_una))   //确定要重发的包
		{
			if(skb->next == (struct sk_buff *)(&(worst_sk)->sk_write_queue))
			{
				// printk(KERN_EMERG "rmt -----simple_retransmission_ last skb get");
				*ifLastPacket=true;
			}
		  	return skb;
		}    
		// printk(KERN_EMERG "rmt -----simple_retransmission_ before una");
    } 

    worst_tp->ifAllFusoProtected=1;
    //printk(KERN_EMERG "rmt  BUG: failure at %s:%d/%s()! \n",__FILE__, __LINE__, __FUNCTION__);
    // printk(KERN_EMERG "-----no coding simple_retransmission_worstsubf_skb   ifAllProtected=%d,ifAllFusoProtected=%d\n",worst_tp->ifAllProtected,worst_tp->ifAllFusoProtected);
    return NULL;
}
//ly add 20190428
//ly add Xor start
static struct sk_buff *return_XOR_skb(struct sock *worst_sk, struct sock *meta_sk,struct CodeInfo codeinfo,bool *ifLastPacket)
{
    struct tcp_sock *worst_tp=tcp_sk(worst_sk);
    struct sk_buff *skb = tcp_write_queue_head(worst_sk);
    unsigned int xorlen;
    u32 dataseq;
    tcp_for_write_queue_from(skb, worst_sk) 
    {
		if(skb->next == (struct sk_buff *)(&(worst_sk)->sk_write_queue))
		{
			*ifLastPacket=true;
		}
		if(TCP_SKB_CB(skb)->seq==codeinfo.startseq)
		{
			//printk(KERN_EMERG "in return _xor_skb,coded skb1 startseq is TCP_SKB_CB(skb)->seq is%u, TCP_SKB_CB(skb)->end_seq=%u\n",TCP_SKB_CB(skb)->seq,TCP_SKB_CB(skb)->end_seq);
            break;
        }
        
    }
    struct sk_buff *skb2 = pskb_copy_for_clone(skb, GFP_ATOMIC);
    struct sk_buff *skb1=skb->next;
    struct tcp_skb_cb *tcb2 = TCP_SKB_CB(skb2);
    xorlen=skb2->len-skb2->data_len;
    struct mp_dss *mpdss = (struct mp_dss *)tcb2->dss;
	u32 *p32;
	if (mpdss->M)
	{
		p32 = (u32 *)mpdss;
		p32++;
		if (mpdss->A) 
		{
			p32++;
		    if (mpdss->a)
			    p32++;
	    }
	    dataseq = ntohl(*p32);
	}
	else
	{
		dataseq=0;
		//printk(KERN_EMERG "why here no mpdss->M?\n");
	}		
    tcp_for_write_queue_from(skb1, worst_sk)
    {	
        if(skb1->next == (struct sk_buff *)(&(worst_sk)->sk_write_queue))
		{
		    *ifLastPacket=true;
	    }
        unsigned int n=skb2->len-skb2->data_len;
        unsigned int n1=skb1->len-skb1->data_len;
        xorlen= n1 ^ xorlen;
        mpdss = (struct mp_dss *)TCP_SKB_CB(skb1)->dss;
        u32 dataseq_tmp;
        if (mpdss->M)
        {
        	p32 = (u32 *)mpdss;
        	p32++;
        	if (mpdss->A) 
        	{
        		p32++;
        		if (mpdss->a)
        			p32++;
        	}
        	dataseq_tmp = ntohl(*p32);
        }
        else
        {
        	dataseq_tmp =0;
        	//printk(KERN_EMERG "why here no mpdss->M?\n");
        }
        dataseq=dataseq ^ dataseq_tmp;
        unsigned char *ptr1=skb1->data;
        unsigned char *ptr2=skb2->data;
        // printk(KERN_EMERG "in return _xor_skb,coded skb1 startseq is TCP_SKB_CB(skb)->seq is%u, TCP_SKB_CB(skb)->end_seq=%u\n",TCP_SKB_CB(skb1)->seq,TCP_SKB_CB(skb1)->end_seq);
        // printk(KERN_EMERG "=linear data:skb2->data1=%02X\n",*(skb2->data));
        // printk(KERN_EMERG "=linear data:skb2->data2=%02X\n",*((skb2->data)+1));
        // printk(KERN_EMERG "=linear data:skb2->data3=%02X\n",*((skb2->data)+2));
        // if(skb2->data+10)
        // {
        // 	printk(KERN_EMERG "=linear data:skb2->data10=%02X\n",*((skb2->data)+10));
        // }
        // if(skb2->data+20)
        // {
        // 	printk(KERN_EMERG "=linear data:skb2->data20=%02X\n",*((skb2->data)+20));
        // }
        // if(skb2->data+30)
        // {
        // 	printk(KERN_EMERG "=linear data:skb2->data30=%02X\n",*((skb2->data)+30));
        // }
        // if(skb2->data+45)
        // {
        // 	printk(KERN_EMERG "=linear data:skb2->data45=%02X\n",*((skb2->data)+45));
        // }
        // if(skb2->data+80)
        // {
        // 	printk(KERN_EMERG "=linear data:skb2->data80=%02X\n",*((skb2->data)+80));
        // }
        // printk(KERN_EMERG "=linear data:skb1->data1=%02X\n",*(skb1->data));
        // printk(KERN_EMERG "=linear data:skb1->data2=%02X\n",*((skb1->data)+1));
        // printk(KERN_EMERG "=linear data:skb1->data3=%02X\n",*((skb1->data)+2));
        // if(skb1->data+10)
        // {
        // 	printk(KERN_EMERG "=linear data:skb1->data10=%02X\n",*((skb1->data)+10));
        // }
        // if(skb1->data+20)
        // {
        // 	printk(KERN_EMERG "=linear data:skb1->data20=%02X\n",*((skb1->data)+20));
        // }
        // if(skb1->data+30)
        // {
        // 	printk(KERN_EMERG "=linear data:skb1->data30=%02X\n",*((skb1->data)+30));
        // }
        // if(skb1->data+45)
        // {
        // 	printk(KERN_EMERG "=linear data:skb1->data45=%02X\n",*((skb1->data)+45));
        // }
        // if(skb1->data+80)
        // {
        // 	printk(KERN_EMERG "=linear data:skb1->data80=%02X\n",*((skb1->data)+80));
        // }
        // printk(KERN_EMERG "we can get linear data skb1:%d\n",n1);
        // printk(KERN_EMERG "we can get linear data skb2:%d\n",n);
        if(n>=n1)
        {
        	n=n1;
            while(n--)
            {
                *ptr2 = *ptr1 ^ *ptr2 ;
        	    ptr1 = ptr1 + 1;
                ptr2 = ptr2 + 1;
            }
        }
        else
        {
        	while(n--)
            {
                *ptr2 = *ptr1 ^ *ptr2 ;
        	    ptr1 = ptr1 + 1;
                ptr2 = ptr2 + 1;
                n1--;
            }
            if(skb_tailroom(skb2) <n1)
        	{
    	        struct sk_buff *nskb;
    	        nskb = skb_copy_expand(skb2, skb_headroom(skb2), skb_tailroom(skb2)+n1,GFP_ATOMIC);
    	        if(!nskb)
    	        {
    	        	printk(KERN_EMERG "low memory....\n");        
    	        	dev_kfree_skb(skb2);
    	        	return NULL;
    	        }    
    	        else
    	        {
                    kfree_skb(skb2); 
                    skb2 = nskb;
                }
            }
            while(n1--)
            {
            	memcpy(skb_put(skb2,1),ptr1,1);
            	ptr1 = ptr1 + 1;
            }
        }
        //printk(KERN_EMERG "we xor the data successfully!!\n");
        
        if(TCP_SKB_CB(skb1)->end_seq==codeinfo.stopseq)
        {
        	break;
        }
    }
    //printk(KERN_EMERG "=final linear data:skb2->data1=%02X\n",*(skb2->data));
   // printk(KERN_EMERG "=final linear data:skb2->data2=%02X\n",*((skb2->data)+1));
    //ly noted 20190430
   // struct sk_buff *skb3= pskb_copy_for_clone(skb1, GFP_ATOMIC);
    //ly noted 20190430
    //struct tcphdr *th = tcp_hdr(skb2);
    //unsigned char *ptr_head=( unsigned char *)(th + 1);
    //struct mptcp_option *mp_opt = (struct mptcp_option *)ptr_head;
    //printk(KERN_EMERG "now we get the sizeoftcp option!!!\n"); 
    //u8 n=sizeof(*mp_opt);
	//u8 n1=sizeof(*ptr_head);
	//printk(KERN_EMERG "we can get the finally sizeofmp_opt:%u\n",n);
	//printk(KERN_EMERG "we can get the finally sizeofptr_head2:%u\n",n1);
	//if(n==0)
	   // n=n1;
    struct datacodeinfo info;
    info.subflowid=tcp_sk(worst_sk)->mptcp->path_index;
    info.startseq=codeinfo.startseq;
	info.stopseq=codeinfo.stopseq;
	info.pindex=0;
	info.datak=codeinfo.DataK;
	info.codem=codeinfo.CodeM;
	info.xorlen=xorlen;
	info.dataseq=dataseq;
	//printk(KERN_EMERG "we can get the data add info:subflowid=%u,startseq=%u,stopseq=%u,headlen=%u,datak=%u,codem=%u,pindex=%u,dataseq=%u\n",info.subflowid,info.startseq,info.stopseq,info.headlen,info.datak,info.codem,info.pindex,info.dataseq);
	u8 m=sizeof(info);
	//printk(KERN_EMERG "we can get the finally add info size:%u\n",m);
	//ly noted 20190430
	// if(skb3->len-skb3->data_len<skb2->len-skb2->data_len)
	// {
	// 	if(skb_tailroom(skb3) <(skb2->len-skb2->data_len)-(skb3->len-skb3->data_len))
 //        {
 //    	    struct sk_buff *nskb;
 //    	    nskb = skb_copy_expand(skb3, skb_headroom(skb3), skb_tailroom(skb3)+((skb2->len-skb2->data_len)-(skb3->len-skb3->data_len)),GFP_ATOMIC);
 //    	    if(!nskb)
 //    	    {
 //    	        printk(KERN_EMERG "low memory....\n");        
 //    	        dev_kfree_skb(skb3);
 //    	        return NULL;
 //    	    }    
 //    	    else
 //    	    {
 //                kfree_skb(skb3); 
 //                skb3 = nskb;
 //            }
 //        }
 //        unsigned char *a=skb_put(skb3,(skb2->len-skb2->data_len)-(skb3->len-skb3->data_len));
	// }
	//memcpy(skb3->data,skb2->data,skb2->len-skb2->data_len);
	//ly noted 20190430
	//printk(KERN_EMERG "=final linear data:skb2->data1=%02X\n",*(skb2->data));
   // printk(KERN_EMERG "=final linear data:skb2->data2=%02X\n",*((skb2->data)+1));
	//printk(KERN_EMERG "now skb length is=%d\n",skb2->len-skb2->data_len);
	if(skb_tailroom(skb2) < m)
    {
    	//printk(KERN_EMERG "enter the skb_copy_expand \n");
    	struct sk_buff *nskb;
        nskb = skb_copy_expand(skb2, skb_headroom(skb2), skb_tailroom(skb2)+m,GFP_ATOMIC);
        if(!nskb)
        {
            printk(KERN_EMERG "low memory....\n");        
            dev_kfree_skb(skb2);
            return NULL;
        }    
        else
        {
             kfree_skb(skb2); 
             skb2 = nskb;
        }
    }
    // printk(KERN_EMERG "xorlen=%d",xorlen);
    // printk(KERN_EMERG "info.xorlen=%d",info.xorlen);
    // printk(KERN_EMERG "dataseq=%u",dataseq);
    // printk(KERN_EMERG "info.dataseq=%u",info.dataseq);
    // printk(KERN_EMERG "now skb length3 is=%d\n",skb2->len-skb2->data_len);
    memcpy(skb_put(skb2,m),&info,m);
    //printk(KERN_EMERG "now skb length4 is=%d\n",skb2->len-skb2->data_len);
    if(skb1==(struct sk_buff *)(&(worst_sk)->sk_write_queue))
	{
		worst_tp->ifAllProtected=1;
		//printk(KERN_EMERG "in xor ,ifAllProtected=%d,ifAllFusoProtected=%d\n",worst_tp->ifAllProtected,worst_tp->ifAllFusoProtected);
	}
	//unsigned char *tmp=skb_tail_pointer(skb2);
	//tmp=tmp-m;
	//struct datacodeinfo *infoptr=(struct datacodeinfo *)tmp;
	// if(!infoptr)
	// {
	// 	printk(KERN_EMERG "infoptr is NULL!\n");
	// }
	// printk(KERN_EMERG "infoptr->subflowid=%u,infoptr->startseq=%u,infoptr->stopseq=%u\n",infoptr->subflowid,infoptr->startseq,infoptr->stopseq);
	// printk(KERN_EMERG "infoptr->headlen=%u,infoptr->datak=%u,infoptr->codem=%u,infoptr->pindex=%u\n",infoptr->headlen,infoptr->datak,infoptr->codem,infoptr->pindex);
	// printk(KERN_EMERG "infoptr->xorlen=%d\n",infoptr->xorlen);
	// printk(KERN_EMERG "infoptr->dataseq=%u\n",infoptr->dataseq);
	//ly noted 20190430
	//__kfree_skb(skb2); 
	//ly noted 20190430
	return skb2;
}

//ly add xor end
//ly add flag start
static void set_flag(struct sock *worst_sk,struct CodeInfo codeinfo)
{
	struct tcp_sock *worst_tp=tcp_sk(worst_sk);
    struct sk_buff *skb = tcp_write_queue_head(worst_sk);
    //int temp=0;
    tcp_for_write_queue_from(skb, worst_sk) 
    {
		if(TCP_SKB_CB(skb)->seq==codeinfo.startseq)
		{
			//temp++;
			// if(sysctl_mptcp_testmss)
			// {
			// 	skb->ifRMTProtected = 2;
			// }
			// else
			// {
				skb->ifRMTProtected = 2;
			//}
          //  skb->counted_skb=0;
            //printk(KERN_EMERG "set_flag1....\n");  
            break;
		}
    }
    if(skb_queue_is_last(&(worst_sk)->sk_write_queue, skb))
	{
		//printk(KERN_EMERG " no set_flag....\n");
		return;
	}
	else
	{
		skb=skb_queue_next(&(worst_sk)->sk_write_queue, skb);
	} 
    tcp_for_write_queue_from(skb, worst_sk) 
    {
        //temp++; 
    	skb->ifRMTProtected = 2;
       // skb->counted_skb=0;
    	if(TCP_SKB_CB(skb)->end_seq==codeinfo.stopseq)
        {
        	//skb->ifRMTProtected = 1;
        	//printk(KERN_EMERG " set_flaged....\n");
        	break;
        }
    	
    }
}
//ly add flag end
//ly add test for fixed code number
static struct CodeInfo  get_DataM_CodeK_for_test(struct sock *worst_sk)
{
	struct tcp_sock *worst_tp = tcp_sk(worst_sk);
    struct sk_buff *skb = tcp_write_queue_head(worst_sk);
    struct CodeInfo codeinfo;
    codeinfo.CodeM=0;
    codeinfo.DataK=0;
    codeinfo.MaxParityNum=0;
    codeinfo.startseq=0;
    codeinfo.stopseq=0;
    bool flag=false;
    int count=0;
    u32 count_skb=worst_sk->sk_write_queue.qlen;
    if(count_skb<10)
    {
    	//printk(KERN_EMERG "the worst_sk's count_skb<3\n");
    	codeinfo.CodeM=0;
    	codeinfo.DataK=0;
    	codeinfo.MaxParityNum=0;
    	codeinfo.startseq=0;
    	codeinfo.stopseq=0;
    	return codeinfo;
    }
    //printk(KERN_EMERG "worst_sk->sk_write_queue.qlen=%d\n",worst_sk->sk_write_queue.qlen);
    tcp_for_write_queue_from(skb, worst_sk) 
    {
    	//printk(KERN_EMERG "in find skb->counted_skb,skb->len=%d\n",skb->len);
    	//printk(KERN_EMERG "the worst_sk's count_skb====%u,worst_tp->count_skb===%u\n",skb->counted_skb,count_skb);
    	if(skb->counted_skb==262)
    	{
    		flag=true;
    		int i=10;
    		codeinfo.startseq=TCP_SKB_CB(skb)->seq;    		
   			while(i--)
    		{
    			codeinfo.stopseq=TCP_SKB_CB(skb)->end_seq;
    			codeinfo.DataK++;
    			codeinfo.CodeM=1;
    			codeinfo.MaxParityNum++;
    			if(skb->ifRMTProtected||skb->ifProtectionPacket)
    			{
    				count++;
    			}
    			//printk(KERN_EMERG "in find datak,codem,test,,countedskb=%u,seq=%u,end_seq=%u,skb->ifRMTProtected=%d,skb->ifProtectionPacket=%d\n"
    			//	,skb->counted_skb,TCP_SKB_CB(skb)->seq,TCP_SKB_CB(skb)->end_seq,skb->ifRMTProtected,skb->ifProtectionPacket);
    			if(skb_queue_is_last(&(worst_sk)->sk_write_queue, skb))
    			{
    			//	printk(KERN_EMERG "why skb_queue_is_last,i=%d",i);
    				break;
    			}
    			skb=skb_queue_next(&(worst_sk)->sk_write_queue, skb);
    		}
    		break;
    	}
	}

	
	
	if(flag&&count==0)
	{
		return codeinfo;
	}
	else
	{
		//printk(KERN_EMERG "we didn't find skb->counted_skb==30\n");
		//printk(KERN_EMERG  "skb->ifRMTProtected||skb->ifProtectionPacket count=%d\n",count);
    	codeinfo.CodeM=0;
    	codeinfo.DataK=0;
    	codeinfo.MaxParityNum=0;
    	codeinfo.startseq=0;
    	codeinfo.stopseq=0;
    	return codeinfo;
	}

}
//ly add test for fixed code number
//ly add codeinfo start
static struct CodeInfo  get_DataM_CodeK(struct sock *worst_sk)
{
	struct tcp_sock *worst_tp = tcp_sk(worst_sk);
    struct sk_buff *skb = tcp_write_queue_head(worst_sk);
    struct sk_buff *skb2;
    struct CodeInfo codeinfo;
    int datak;
    int temp1;
    int sacked;
    struct tcp_skb_cb *tcb;
    u32 total_retrans,total_trans,packet_sent_from_last_retran;
    int datak1;
refind:    
	codeinfo.CodeM=0;
	codeinfo.DataK=0;
	codeinfo.MaxParityNum=0;
	codeinfo.startseq=0;
	codeinfo.stopseq=0;
	datak=0;//unacked packets
	temp1=0;//temp count for codeinfo.MaxParityNum
	sacked=0;//count for sacked;
    tcp_for_write_queue_from(skb, worst_sk) 
    {
		if (skb->ifRMTProtected || skb->ifProtectionPacket)
		{ 
			continue;  
		} 
		tcb = TCP_SKB_CB(skb);
		if (tcb->tcp_flags & TCPHDR_SYN ||
		(tcb->tcp_flags & TCPHDR_FIN && !mptcp_is_data_fin(skb)) ||
		(tcb->tcp_flags & TCPHDR_FIN && mptcp_is_data_fin(skb) && !skb->len)||!mptcp_is_data_seq(skb))
		{
			continue;     
		}
		if(!before(TCP_SKB_CB(skb)->end_seq, worst_tp->snd_una))  
		{
			if (TCP_SKB_CB(skb)->sacked & TCPCB_SACKED_ACKED)  
			{
				sacked++;
			}
		  	datak++;
		  	temp1++;
			codeinfo.MaxParityNum++;
		  	codeinfo.startseq=TCP_SKB_CB(skb)->seq;
		  	codeinfo.stopseq=TCP_SKB_CB(skb)->end_seq;
		  	break;
		}
	}
	//ly add 20190225
	if(!skb_queue_is_last(&(worst_sk)->sk_write_queue, skb))
	{
		skb2=skb_queue_next(&(worst_sk)->sk_write_queue, skb);
		tcp_for_write_queue_from(skb2, worst_sk)
		{
			if (skb2->ifRMTProtected || skb2->ifProtectionPacket)
			{ 
				break;  
			} 
			tcb = TCP_SKB_CB(skb2);
		    if (tcb->tcp_flags & TCPHDR_SYN ||
		    (tcb->tcp_flags & TCPHDR_FIN && !mptcp_is_data_fin(skb2)) ||
		    (tcb->tcp_flags & TCPHDR_FIN && mptcp_is_data_fin(skb2) && !skb2->len))
		    {
			    break;     
			}
			if(before(TCP_SKB_CB(skb2)->end_seq, worst_tp->snd_una))
			{
				break;
			}
			datak++;
		}
	}
	//printk(KERN_EMERG "unacked data number is %d\n",datak);
	if(datak==0)
	{
		return codeinfo;
	}
	total_retrans = worst_tp->total_retrans;
	total_trans = worst_tp->total_trans;
	packet_sent_from_last_retran  = worst_tp->packet_sent_from_last_retran;
	if(total_retrans==0)
	{
		datak1=(int)packet_sent_from_last_retran;
	}
	else
	{
		datak1=(int)total_trans/(int)total_retrans;
	}
	if(datak1>=datak)
	{
		codeinfo.DataK=datak;
		codeinfo.CodeM=1;
	}
	else
	{
		codeinfo.DataK=datak1;
		codeinfo.CodeM=1;
	}
	if(codeinfo.DataK==0)
	{
		codeinfo.CodeM=0;
	}
	//ly add max_code_block
	if(codeinfo.DataK>MAX_CODE_BLOCK)
	{
		codeinfo.DataK=MAX_CODE_BLOCK;
	}
	//ly add max_code_block
	if(skb_queue_is_last(&(worst_sk)->sk_write_queue, skb))
	{
		return codeinfo;
	}
	else
	{
		skb=skb_queue_next(&(worst_sk)->sk_write_queue, skb);
	}   
	tcp_for_write_queue_from(skb, worst_sk)
	{
		temp1++;
		codeinfo.stopseq=TCP_SKB_CB(skb)->end_seq;
		if(temp1==codeinfo.DataK)
		{
			if (TCP_SKB_CB(skb)->sacked & TCPCB_SACKED_ACKED)  // /* Short-circut when first non-SACKed skb has been checked */  
			{
				sacked++;
			}
		    else
			{
				codeinfo.MaxParityNum++; 
			}
			break;
		}
		if (TCP_SKB_CB(skb)->sacked & TCPCB_SACKED_ACKED)   // /* Short-circut when first non-SACKed skb has been checked */  
		{
			sacked++;
			continue;  
		}
		codeinfo.MaxParityNum++;		
	}
	if(sacked==codeinfo.DataK)
	{
		codeinfo.DataK=0;
		codeinfo.CodeM=0;
		if(!skb_queue_is_last(&(worst_sk)->sk_write_queue, skb))
		{
		    skb=skb_queue_next(&(worst_sk)->sk_write_queue, skb);
		    goto refind;
		}
	}
	return codeinfo;
}
//ly add codeinfo end
// find worst subflow
static struct sock *find_worst_subflow(struct sock *meta_sk)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct sock *sk, *worstsk;
	worstsk = NULL;
	u32 in_flight,cwnd,rwnd,minwnd,rtt;
	unsigned int mss_now;
	u64 worstcost;
	worstcost = 0;
	u64 cost;

	mptcp_for_each_sk(mpcb,sk){
		struct tcp_sock *tp = tcp_sk(sk);
		in_flight = tcp_packets_in_flight(tp);
		cwnd = tp->snd_cwnd;
		rwnd = (u32) tp->rcv_wnd/mss_now;
		rtt  = tp->srtt_us;
		mss_now = tcp_current_mss(sk);

		// char srcIP[16],dstIP[16];
		// memset(srcIP,'\0',16);
		// memset(dstIP,'\0',16);
		// sprintf(srcIP,"%d.%d.%d.%d",NIPQUAD(sk->sk_rcv_saddr));
		// sprintf(dstIP,"%d.%d.%d.%d",NIPQUAD(sk->sk_daddr));
		// printk(KERN_EMERG "-----find_worst_subflow(%s--->%s) flight = %u cwnd = %u sport = %u,dport = %u\n"
		// 	,srcIP,dstIP,in_flight,cwnd,sk->sk_num,ntohs(sk->sk_dport));

		////FIX me
		//Missing two conditions: 1) only protection packets in this sub-flow; 2) this sub-flow has all been ifProtectionPacket
		
		if(tp->ifAllProtected==1)
		//ly add 20190428
		//if(tp->ifAllProtected==1&&tp->ifAllFusoProtected==1)
		//ly add 20190428
		{
			// printk(KERN_EMERG "find_worst_subflow: All ifProtectionPacket flight == 0, sport = %u, dport = %u\n",
			// 	sk->sk_num,ntohs(sk->sk_dport));
			continue;
		}

		if(in_flight == 0)
			continue;

		minwnd=cwnd;

		cost = in_flight * rtt * DEVISION_SCALER/ minwnd;
		
		if(cost>=worstcost){
			worstcost = cost;
			worstsk = sk;
		}
		
		// printk(KERN_EMERG "find_worst_subflow  flight = %u rtt = %u cost =%llu worstcost=%llu cwnd = %u sport = %u, dport = %u\n"
		// 	,in_flight,rtt,cost,worstcost,cwnd,sk->sk_num,ntohs(sk->sk_dport));

	}
	
	if(worstsk==NULL){
		// printk(KERN_EMERG "no worst subflow!\n");
		return worstsk;
	}
	else{
		// char srcIP[16],dstIP[16];
		// memset(srcIP,'\0',16);
		// memset(dstIP,'\0',16);
		// sprintf(srcIP,"%d.%d.%d.%d",NIPQUAD(worstsk->sk_rcv_saddr));
		// sprintf(dstIP,"%d.%d.%d.%d",NIPQUAD(worstsk->sk_daddr));
		// printk(KERN_EMERG "return worst subflow(%s--->%s) sport = %u,dport = %u\n"
		// 	,srcIP,dstIP,worstsk->sk_num,ntohs(worstsk->sk_dport));
   		
   		return worstsk;
	}
}
//ly add
static struct sock *find_worst_subflow_coding(struct sock *meta_sk,bool *allfusoed_flag)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct sock *sk, *worstsk;
	worstsk = NULL;
	u32 in_flight,cwnd,rwnd,minwnd,rtt;
	unsigned int mss_now;
	u64 worstcost;
	worstcost = 0;
	u64 cost;

	mptcp_for_each_sk(mpcb,sk){
		struct tcp_sock *tp = tcp_sk(sk);
		in_flight = tcp_packets_in_flight(tp);
		cwnd = tp->snd_cwnd;
		rwnd = (u32) tp->rcv_wnd/mss_now;
		rtt  = tp->srtt_us;
		mss_now = tcp_current_mss(sk);
		if(sysctl_mptcp_testmss==1)
		{
			if(tp->ifAllProtected==1&&tp->ifAllFusoProtected==1)
		    {
			    continue;
		    }
		}
		else
		{
		    if(tp->ifAllProtected==1)
		    {
			    continue;
		    }
		}
		
		if(in_flight == 0)
			continue;

		minwnd=cwnd;

		cost = in_flight * rtt * DEVISION_SCALER/ minwnd;
		
		if(cost>=worstcost){
			worstcost = cost;
			worstsk = sk;
		}
	}
	
	if(worstsk==NULL)
	{
		if(sysctl_mptcp_testmss==0)
		{
			worstcost = 0;
			mptcp_for_each_sk(mpcb,sk)
			{
				struct tcp_sock *tp = tcp_sk(sk);
				in_flight = tcp_packets_in_flight(tp);
				cwnd = tp->snd_cwnd;
				rwnd = (u32) tp->rcv_wnd/mss_now;
				rtt  = tp->srtt_us;
				mss_now = tcp_current_mss(sk);
				if(tp->ifAllProtected==1&&tp->ifAllFusoProtected==1)
				{
					continue;
				}

				if(in_flight == 0)
					continue;
				minwnd=cwnd;
				cost = in_flight * rtt * DEVISION_SCALER/ minwnd;
				if(cost>=worstcost)
				{
					worstcost = cost;
					worstsk = sk;
				}
			}
			if(worstsk!=NULL&&tcp_sk(worstsk)->ifAllProtected==1)
			{
				*allfusoed_flag=true;
			}
		}
		return worstsk;
	}
	else
	{
		if(sysctl_mptcp_testmss==1)
		{
		    if(tcp_sk(worstsk)->ifAllProtected==1)
	        {
	 	        *allfusoed_flag=true;
	        }
	    }
   		return worstsk;
	}
}
//ly add
static struct sock *find_worst_subflow_lossrate(struct sock *meta_sk)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct sock *sk, *worstsk;
	worstsk = NULL;
	u32 in_flight,total_retrans,total_trans,packet_sent_from_last_retran;
	long long int worstcost;
	worstcost = 0;
	long long int cost;

	mptcp_for_each_sk(mpcb,sk){
		struct tcp_sock *tp = tcp_sk(sk);
		in_flight = tcp_packets_in_flight(tp);
		total_retrans = tp->total_retrans;
		total_trans = tp->total_trans;
		packet_sent_from_last_retran  = tp->packet_sent_from_last_retran;

		// char srcIP[16],dstIP[16];
		// memset(srcIP,'\0',16);
		// memset(dstIP,'\0',16);
		// sprintf(srcIP,"%d.%d.%d.%d",NIPQUAD(sk->sk_rcv_saddr));
		// sprintf(dstIP,"%d.%d.%d.%d",NIPQUAD(sk->sk_daddr));
		// printk(KERN_EMERG "-----find_worst_subflow_lossrate(%s:%u--->%s:%u) flight = %u cwnd = %u total_retrans = %u total_trans = %u packet_sent_from_last_retran = %u rtt =%u\n"
		// 	,srcIP,sk->sk_num,dstIP,ntohs(sk->sk_dport),in_flight,tp->snd_cwnd,total_retrans,total_trans,packet_sent_from_last_retran,tp->srtt_us);

		////FIX me
		//Missing two conditions: 1) only protection packets in this sub-flow; 2) this sub-flow has all been ifProtectionPacket
		if(tp->ifAllProtected==1)
		//ly add 20190428
		//if(tp->ifAllProtected==1&&tp->ifAllFusoProtected==1)
		//ly add 20190428
		{
			// printk(KERN_EMERG "find_worst_subflow_lossrate: All ifProtectionPacket sport = %u, dport = %u\n",
			// 	sk->sk_num,ntohs(sk->sk_dport));
			continue;
		}

		if(in_flight == 0)
			continue;
		if(packet_sent_from_last_retran==0 || total_trans==0)
		{
			// printk(KERN_EMERG "rmt error: packet_sent_from_last_retran==0 || total_trans==0 !\n");
			break;
		}

		if(total_retrans==0)
			cost=0-DEVISION_SCALER/((long long int)tp->srtt_us);
		else
			cost = ((long long int)total_retrans*DEVISION_SCALER/(long long int)total_trans)+(1*DEVISION_SCALER/(long long int)packet_sent_from_last_retran);
		
		if(worstcost ==0){
        	worstcost = cost;
        	worstsk = sk;
        }
		if(cost>worstcost){
			worstcost = cost;
			worstsk = sk;
		}
		
		// printk(KERN_EMERG "find_worst_subflow_lossrate cost =%lli worstcost=%lli sport = %u, dport = %u\n"
		// 	,cost,worstcost,sk->sk_num,ntohs(sk->sk_dport));

	}
	
	if(worstsk==NULL){
		// printk(KERN_EMERG "no worst subflow!\n");
		return worstsk;
	}
	else{
		// char srcIP[16],dstIP[16];
		// memset(srcIP,'\0',16);
		// memset(dstIP,'\0',16);
		// sprintf(srcIP,"%d.%d.%d.%d",NIPQUAD(worstsk->sk_rcv_saddr));
		// sprintf(dstIP,"%d.%d.%d.%d",NIPQUAD(worstsk->sk_daddr));
		// printk(KERN_EMERG "return worst subflow(%s--->%s) sport = %u,dport = %u\n"
		// 	,srcIP,dstIP,worstsk->sk_num,ntohs(worstsk->sk_dport));
   		
   		return worstsk;
	}
}
//ly add
static struct sock *find_worst_subflow_lossrate_coding(struct sock *meta_sk,bool *allfusoed_flag)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct sock *sk, *worstsk;
	worstsk = NULL;
	u32 in_flight,total_retrans,total_trans,packet_sent_from_last_retran;
	long long int worstcost;
	worstcost = 0;
	long long int cost;

	mptcp_for_each_sk(mpcb,sk)
	{
		struct tcp_sock *tp = tcp_sk(sk);
		in_flight = tcp_packets_in_flight(tp);
		total_retrans = tp->total_retrans;
		total_trans = tp->total_trans;
		packet_sent_from_last_retran  = tp->packet_sent_from_last_retran;
		if(sysctl_mptcp_testmss==1)
		{
			if(tp->ifAllProtected==1&&tp->ifAllFusoProtected==1)
		    {
			    continue;
		    }
		}
		else
		{
		    if(tp->ifAllProtected==1)
		    {
			    continue;
		    }
		}

		if(in_flight == 0)
			continue;
		if(packet_sent_from_last_retran==0 || total_trans==0)
		{
			break;
		}

		if(total_retrans==0)
			cost=0-DEVISION_SCALER/((long long int)tp->srtt_us);
		else
			cost = ((long long int)total_retrans*DEVISION_SCALER/(long long int)total_trans)+(1*DEVISION_SCALER/(long long int)packet_sent_from_last_retran);
		
		if(worstcost ==0){
        	worstcost = cost;
        	worstsk = sk;
        }
		if(cost>worstcost){
			worstcost = cost;
			worstsk = sk;
		}

	}
	
	if(worstsk==NULL)
	{
		if(sysctl_mptcp_testmss==0)
		{
			worstcost = 0;
			mptcp_for_each_sk(mpcb,sk){
				struct tcp_sock *tp = tcp_sk(sk);
				in_flight = tcp_packets_in_flight(tp);
				total_retrans = tp->total_retrans;
				total_trans = tp->total_trans;
				packet_sent_from_last_retran  = tp->packet_sent_from_last_retran;
				if(tp->ifAllProtected==1&&tp->ifAllFusoProtected==1)
				{
					continue;
				}
				if(in_flight == 0)
					continue;
				if(packet_sent_from_last_retran==0 || total_trans==0)
				{
					break;
				}
				if(total_retrans==0)
					cost=0-DEVISION_SCALER/((long long int)tp->srtt_us);
				else
					cost = ((long long int)total_retrans*DEVISION_SCALER/(long long int)total_trans)+(1*DEVISION_SCALER/(long long int)packet_sent_from_last_retran);
				if(worstcost ==0){
					worstcost = cost;
					worstsk = sk;
				}
				if(cost>worstcost){
					worstcost = cost;
					worstsk = sk;
				}
			}
			if(worstsk!=NULL&&tcp_sk(worstsk)->ifAllProtected==1)
			{
				*allfusoed_flag=true;
			}
		}
		return worstsk;
	}
	else
	{
		if(sysctl_mptcp_testmss==1)
		{
			if(tcp_sk(worstsk)->ifAllProtected==1)
			{
	 	        *allfusoed_flag=true;
	        }
	    }
		return worstsk;
	}
}
//ly add
//find best subflow
static struct sock *find_best_subflow(struct sock *meta_sk,struct sk_buff *skb, u8 worstPathIndex)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct sock *sk, *bestsk;
	bestsk = NULL;
	u32 in_flight,cwnd,rtt;
	u64 bestcost;
	bestcost = 0;
	u64 cost;
	unsigned int mss_now;
	mptcp_for_each_sk(mpcb,sk){

		struct tcp_sock *tp = tcp_sk(sk);
		in_flight = tcp_packets_in_flight(tp);
		cwnd = tp->snd_cwnd;
		rtt  = tp->srtt_us;
		mss_now = tcp_current_mss(sk);

		// char srcIP[16],dstIP[16];
		// memset(srcIP,'\0',16);
		// memset(dstIP,'\0',16);
		// sprintf(srcIP,"%d.%d.%d.%d",NIPQUAD(sk->sk_rcv_saddr));
		// sprintf(dstIP,"%d.%d.%d.%d",NIPQUAD(sk->sk_daddr));
		// printk(KERN_EMERG "-----find_best_subflow(%s--->%s) flight = %u cwnd = %u sport = %u,dport = %u\n"
		// 	,srcIP,dstIP,in_flight,cwnd,sk->sk_num,ntohs(sk->sk_dport));

		if (!mptcp_is_available(sk, skb, true))
			continue;

		if(tp->mptcp->path_index==worstPathIndex)
			continue;
		
		cost = (in_flight+1) * rtt * DEVISION_SCALER /cwnd;
		// printk(KERN_EMERG "find_best_subflow  flight = %u rtt = %u cost =%llu bestcost=%llu cwnd = %u sport = %u, dport = %u\n"
		// 	,in_flight,rtt,cost,bestcost,cwnd,sk->sk_num,ntohs(sk->sk_dport));
        if(bestcost ==0){
        	bestcost = cost;
        	bestsk = sk;
        }
		if(cost<bestcost){
			bestcost = cost;
			bestsk = sk;
		}

	}
	
	if(bestsk == NULL){
		// printk(KERN_EMERG "no best subflow!\n");
		return bestsk;
	}
	else{
		// char srcIP[16],dstIP[16];
		// memset(srcIP,'\0',16);
		// memset(dstIP,'\0',16);
		// sprintf(srcIP,"%d.%d.%d.%d",NIPQUAD(bestsk->sk_rcv_saddr));
		// sprintf(dstIP,"%d.%d.%d.%d",NIPQUAD(bestsk->sk_daddr));
		// printk(KERN_EMERG "return best subflow(%s--->%s) sport = %u,dport = %u\n"
		// 	,srcIP,dstIP,bestsk->sk_num,ntohs(bestsk->sk_dport));
   		
   		return bestsk;
	}
}
static struct sock *find_best_subflow_lossrate(struct sock *meta_sk,struct sk_buff *skb, u8 worstPathIndex)
{
	struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
	struct sock *sk, *bestsk;
	bestsk = NULL;
	u32 in_flight,total_retrans,total_trans,packet_sent_from_last_retran;
	long long int bestcost;
	bestcost = 0;
	long long int cost;
	mptcp_for_each_sk(mpcb,sk){
		struct tcp_sock *tp = tcp_sk(sk);
		in_flight = tcp_packets_in_flight(tp);
		total_retrans = tp->total_retrans;
		total_trans = tp->total_trans;
		packet_sent_from_last_retran  = tp->packet_sent_from_last_retran;

		// char srcIP[16],dstIP[16];
		// memset(srcIP,'\0',16);
		// memset(dstIP,'\0',16);
		// sprintf(srcIP,"%d.%d.%d.%d",NIPQUAD(sk->sk_rcv_saddr));
		// sprintf(dstIP,"%d.%d.%d.%d",NIPQUAD(sk->sk_daddr));
		// printk(KERN_EMERG "-----find_best_subflow_lossrate(%s:%u--->%s:%u) flight = %u cwnd = %u total_retrans = %u total_trans = %u packet_sent_from_last_retran = %u rtt =%u\n"
		// 	,srcIP,sk->sk_num,dstIP,ntohs(sk->sk_dport),in_flight,tp->snd_cwnd,total_retrans,total_trans,packet_sent_from_last_retran,tp->srtt_us);

		if (!mptcp_is_available(sk, skb, true))
			continue;

		// if(tp->mptcp->path_index==worstPathIndex)
		// 	continue;
		
		if(packet_sent_from_last_retran==0 || total_trans==0)
			cost=DEVISION_SCALER;
		else if(total_retrans==0)
			cost=0-DEVISION_SCALER/((long long int)tp->srtt_us);
		else
			cost = ((long long int)total_retrans*DEVISION_SCALER/(long long int)total_trans)+(1*DEVISION_SCALER/(long long int)packet_sent_from_last_retran);
		
        if(bestcost ==0){
        	bestcost = cost;
        	bestsk = sk;
        }
		if(cost<bestcost){
			bestcost = cost;
			bestsk = sk;
		}
		// printk(KERN_EMERG "find_best_subflow_lossrate cost =%lli bestcost=%lli sport = %u, dport = %u\n"
		// 	,cost,bestcost,sk->sk_num,ntohs(sk->sk_dport));
	}
	
	if(bestsk == NULL){
		// printk(KERN_EMERG "no best subflow!\n");
		return bestsk;
	}
	else{
		// char srcIP[16],dstIP[16];
		// memset(srcIP,'\0',16);
		// memset(dstIP,'\0',16);
		// sprintf(srcIP,"%d.%d.%d.%d",NIPQUAD(bestsk->sk_rcv_saddr));
		// sprintf(dstIP,"%d.%d.%d.%d",NIPQUAD(bestsk->sk_daddr));
		// printk(KERN_EMERG "return best subflow(%s--->%s) sport = %u,dport = %u\n"
		// 	,srcIP,dstIP,bestsk->sk_num,ntohs(bestsk->sk_dport));
   		
   		return bestsk;
	}
}

/* find the ifProtectionPacket packet in reinject queue and call the write_xmit in the subflow to send it
	return 1 means not successfully sent a retransmission packet*/
bool simple_retransmission(struct sock *worst_sk,struct sock *meta_sk,struct sock *best_sk,struct sk_buff *skb,bool ifcoding){

    struct tcp_sock *meta_tp = tcp_sk(meta_sk);
    struct mptcp_cb *mpcb = meta_tp->mpcb;
 
	skb->ifRMTProtected = 1;

 	/*Copy skb to reinject queue  
 	In rmt_mptcp_reinject_data skb->ifRMTProtected==1 will be clear and set ifProtectionPacket=1;
 	if ifProtectionPacket==1, this packet will immediately sent by following code.
 	And successive retransmission will from reinject queue.*/	
 	int clone_it=5217; 
 	/////5217 means that this is to reinject a RMTProtected packet. The new copy in th reinject queue will have ifProtectionPacket=1, and ifRMTProtected=0;
    if(rmt_mptcp_reinject_data(worst_sk,clone_it,skb,best_sk,ifcoding))
    	return 1;

    mptcp_push_pending_frames(meta_sk);
    return 0;
}

bool try_send_recoveries(struct sock *meta_sk,int *countfuso)///CG add
{
	///// return value: 1 means not send; 0 means something sent
	int count=0;
	while(1)
	{
		struct sock *worst_sk, *best_sk;
		bool ifLastPacket=false;
		if(sysctl_mptcp_testtcp)
		{
			bool iffindpath=false;
			struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
			struct sock *sk=NULL;
			mptcp_for_each_sk(mpcb,sk)
			{
				if(tcp_sk(sk)->mptcp->path_index==2)
				{
					iffindpath=true;
				    struct tcp_sock *tp = tcp_sk(sk);
		            u32 in_flight = tcp_packets_in_flight(tp);
		            u32 total_trans = tp->total_trans;
		            u32 packet_sent_from_last_retran  = tp->packet_sent_from_last_retran;
		            if(tp->ifAllProtected==1)
		            {
		            	//printk(KERN_EMERG "tp->ifAllProtected==1,so 2!=worst_sk\n");
		            	return 1;
		            }
		            if(in_flight == 0)
		            {
		            	//printk(KERN_EMERG "in_flight == 0,so 2!=worst_sk\n");
			            return 1;
		            }
		            if(packet_sent_from_last_retran==0 || total_trans==0)
		            {
		            	//printk(KERN_EMERG "packet_sent_from_last_retran==0 || total_trans==0,so 2!=worst_sk\n");
		            	return 1;
		            }
		            if(!skb_queue_empty(&(sk)->sk_write_queue))
		            {
		            	//printk(KERN_EMERG "we find the worst_sk=2\n");
		            	worst_sk=sk;
		            	break;
		            }
		            else
		            {
		            	//printk(KERN_EMERG "we can't find the worst_sk=2\n");
		            	return 1;
		            }
			    }
			}
			if(!iffindpath)
			{
				//printk(KERN_EMERG "in mptcp ,we can't find the 2ndpath\n");
				return 1;
			}
			if(sysctl_mptcp_testtcp==1)
			{
				struct CodeInfo codeinfo;
				codeinfo=get_DataM_CodeK_for_test(worst_sk);
				if(codeinfo.DataK==0)
                {
            	    return 1;
                }
                struct sk_buff *skb=return_XOR_skb(worst_sk,meta_sk,codeinfo,&ifLastPacket);
                if (!skb)
                {
                	return 1;
                }
                if(sysctl_mptcp_rmt_path_selection==2)
                	best_sk = find_best_subflow_lossrate(meta_sk,skb,tcp_sk(worst_sk)->mptcp->path_index);
                else
                	best_sk = find_best_subflow(meta_sk,skb,tcp_sk(worst_sk)->mptcp->path_index);
                if(!best_sk)
                {
                	return 1;
                }
                if(ifLastPacket == true)
                {
                	tcp_sk(worst_sk)->ifAllProtected=1;
                }
                set_flag(worst_sk,codeinfo);
                if(simple_retransmission(worst_sk,meta_sk,best_sk,skb,true))
                	return 1;
			}
			else
			{
				struct sk_buff *skb = simple_retransmission_worstsubf_skb(worst_sk,meta_sk,&ifLastPacket);
				if (!skb)
				{
					return 1; 
				}
				if(sysctl_mptcp_rmt==1)
				{
					if(skb->counted_skb<262||skb->counted_skb>271)
					{
						return 1;
					}
				}
				if(sysctl_mptcp_rmt==2)
				{
					if(skb->counted_skb<253||skb->counted_skb>262)
					{
						return 1;
					}
				}
				if(sysctl_mptcp_rmt_path_selection==2)
					best_sk = find_best_subflow_lossrate(meta_sk,skb,tcp_sk(worst_sk)->mptcp->path_index);
				else
					best_sk = find_best_subflow(meta_sk,skb,tcp_sk(worst_sk)->mptcp->path_index);
				if(!best_sk)
				{
					return 1;
				}
				if(ifLastPacket == true)
				{
					tcp_sk(worst_sk)->ifAllProtected=1;
				}

				if(simple_retransmission(worst_sk,meta_sk,best_sk,skb,false))
					return 1;
			}
		}
		else
		{
			if(sysctl_mptcp_coding==0)
			{
				if(sysctl_mptcp_rmt)
				{
					if(sysctl_mptcp_rmt_path_selection==2)
						worst_sk = find_worst_subflow_lossrate(meta_sk);
					else
						worst_sk = find_worst_subflow(meta_sk);	
					if(worst_sk == NULL)
					{
						if(count==0)
						{
							*countfuso=-1;
						}
						else
							*countfuso=6112;
						return 1;
					}
					struct sk_buff *skb = simple_retransmission_worstsubf_skb(worst_sk,meta_sk,&ifLastPacket);
					if (!skb)
					{
						if(count==0)
						{
							*countfuso=-1;
						}
						else
							*countfuso=6112;
						return 1;
					}
					if(sysctl_mptcp_rmt_path_selection==2)
						best_sk = find_best_subflow_lossrate(meta_sk,skb,tcp_sk(worst_sk)->mptcp->path_index);
					else
						best_sk = find_best_subflow(meta_sk,skb,tcp_sk(worst_sk)->mptcp->path_index);
					if(!best_sk)
					{
						if(count!=0)
							*countfuso=count;
						else
							*countfuso=-1;
						return 1;
					}
					if(ifLastPacket == true)
					{
						tcp_sk(worst_sk)->ifAllProtected=1;
					}

					if(simple_retransmission(worst_sk,meta_sk,best_sk,skb,false))
					{
						if(count==0)
						{
							*countfuso=-1;
						}
						else
							*countfuso=6112;
						return 1;
					}
					count++;
				}
			}
			else
			{
				bool allfusoed_flag=false;
				if(sysctl_mptcp_rmt_path_selection==2)
					worst_sk = find_worst_subflow_lossrate_coding(meta_sk,&allfusoed_flag);
				else
					worst_sk = find_worst_subflow_coding(meta_sk,&allfusoed_flag);
				if(worst_sk == NULL)
				{
					return 1;
				}
				if(allfusoed_flag==false)
				{
					struct CodeInfo codeinfo;
					codeinfo=get_DataM_CodeK(worst_sk);
					if(codeinfo.DataK<=codeinfo.CodeM)
					{
						struct sk_buff *skb = simple_retransmission_worstsubf_skb(worst_sk,meta_sk,&ifLastPacket);
						if (!skb)
						{
							return 1;
						}
						if(sysctl_mptcp_rmt_path_selection==2)
							best_sk = find_best_subflow_lossrate(meta_sk,skb,tcp_sk(worst_sk)->mptcp->path_index);
						else
							best_sk = find_best_subflow(meta_sk,skb,tcp_sk(worst_sk)->mptcp->path_index);
						if(!best_sk)
						{
							return 1;
						}
						if(ifLastPacket == true)
						{
							tcp_sk(worst_sk)->ifAllProtected=1;
						}
						if(simple_retransmission(worst_sk,meta_sk,best_sk,skb,false))
							return 1;
					}
					else
					{
		    		//printk(KERN_EMERG "we enter the coding process!\n");
						struct sk_buff *skb=return_XOR_skb(worst_sk,meta_sk,codeinfo,&ifLastPacket);
						if (!skb)
						{
							return 1;
						}
						if(sysctl_mptcp_rmt_path_selection==2)
							best_sk = find_best_subflow_lossrate(meta_sk,skb,tcp_sk(worst_sk)->mptcp->path_index);
						else
							best_sk = find_best_subflow(meta_sk,skb,tcp_sk(worst_sk)->mptcp->path_index);
						if(!best_sk)
						{
							return 1;
						}
						if(ifLastPacket == true)
						{
							tcp_sk(worst_sk)->ifAllProtected=1;
						}
						set_flag(worst_sk,codeinfo);
						if(simple_retransmission(worst_sk,meta_sk,best_sk,skb,true))
							return 1;
					}
				}
				else
				{
		    	//printk(KERN_EMERG "close the coding process,we enter the retrans process!\n");
					struct sk_buff *skb = no_coding_simple_retransmission_worstsubf_skb(worst_sk,meta_sk,&ifLastPacket);
					if (!skb)
					{
						return 1;
					}
					if(sysctl_mptcp_rmt_path_selection==2)
						best_sk = find_best_subflow_lossrate(meta_sk,skb,tcp_sk(worst_sk)->mptcp->path_index);
					else
						best_sk = find_best_subflow(meta_sk,skb,tcp_sk(worst_sk)->mptcp->path_index);
					if(!best_sk)
					{
						return 1;
					}
					if(ifLastPacket == true)
					{
						tcp_sk(worst_sk)->ifAllFusoProtected=1;
					}
					if(simple_retransmission(worst_sk,meta_sk,best_sk,skb,false))
						return 1;
				}
			}
		}
	}
}
bool mptcp_write_xmit(struct sock *meta_sk, unsigned int mss_now, int nonagle,
		     int push_one, gfp_t gfp)
{

	// printk(KERN_EMERG "rmt  BUG: failure at %s:%d/%s()! \n",__FILE__, __LINE__, __FUNCTION__);
	struct tcp_sock *meta_tp = tcp_sk(meta_sk), *subtp;
	struct sock *subsk = NULL;
	struct mptcp_cb *mpcb = meta_tp->mpcb;
	struct sk_buff *skb;
	int reinject = 0;
	unsigned int sublimit;
	__u32 path_mask = 0;

	while ((skb = mpcb->sched_ops->next_segment(meta_sk, &reinject, &subsk,
						    &sublimit))) {
		unsigned int limit;

		subtp = tcp_sk(subsk);
		mss_now = tcp_current_mss(subsk);
		//ly add
		if(skb&&skb->ifProtectionPacket==2)
		{
			mss_now=mss_now+24;
		}
     //ly add

		if (reinject == 1) {
			if (!after(TCP_SKB_CB(skb)->end_seq, meta_tp->snd_una)) {
				/* Segment already reached the peer, take the next one */
				//ly add noted
				if(skb&&skb->ifProtectionPacket==2)
				{
					//printk(KERN_EMERG "your seq is received !!!so is unuse\n");

				}
				else
				{
				    __skb_unlink(skb, &mpcb->reinject_queue);
				    __kfree_skb(skb);
				    continue;
			   }
			}
		}

		// __u16 sknum = subsk->sk_num;
  //    	 u16 skdport = ntohs(subsk->sk_dport);
  //    	 printk(KERN_EMERG "rmt  BUG: failure at %s:%d/%s()! \n",__FILE__, __LINE__, __FUNCTION__);
		// printk(KERN_EMERG "rmt mptcp_write_xmit_push  data level seq = %lu, end_seq =%lu, sport = %u, dport = %u\n",
		// 	TCP_SKB_CB(skb)->seq,TCP_SKB_CB(skb)->end_seq,sknum,skdport);
		// printk(KERN_EMERG "before skb_unclone path_index_to_flag=%d, skb->path_mask=0x%08x \n",mptcp_pi_to_flag(subtp->mptcp->path_index),TCP_SKB_CB(skb)->path_mask);
		

		/* If the segment was cloned (e.g. a meta retransmission),
		 * the header must be expanded/copied so that there is no
		 * corruption of TSO information.
		 */
		if (skb_unclone(skb, GFP_ATOMIC))
			break;

		if (unlikely(!tcp_snd_wnd_test(meta_tp, skb, mss_now)))
			break;

		/* Force tso_segs to 1 by using UINT_MAX.
		 * We actually don't care about the exact number of segments
		 * emitted on the subflow. We need just to set tso_segs, because
		 * we still need an accurate packets_out count in
		 * tcp_event_new_data_sent.
		 */
		tcp_set_skb_tso_segs(meta_sk, skb, UINT_MAX);

		/* Check for nagle, irregardless of tso_segs. If the segment is
		 * actually larger than mss_now (TSO segment), then
		 * tcp_nagle_check will have partial == false and always trigger
		 * the transmission.
		 * tcp_write_xmit has a TSO-level nagle check which is not
		 * subject to the MPTCP-level. It is based on the properties of
		 * the subflow, not the MPTCP-level.
		 */
		if (unlikely(!tcp_nagle_test(meta_tp, skb, mss_now,
					     (tcp_skb_is_last(meta_sk, skb) ?
					      nonagle : TCP_NAGLE_PUSH))))
			break;

		limit = mss_now;
		/* skb->len > mss_now is the equivalent of tso_segs > 1 in
		 * tcp_write_xmit. Otherwise split-point would return 0.
		 */
		if (skb->len > mss_now && !tcp_urg_mode(meta_tp))
			/* We limit the size of the skb so that it fits into the
			 * window. Call tcp_mss_split_point to avoid duplicating
			 * code.
			 * We really only care about fitting the skb into the
			 * window. That's why we use UINT_MAX. If the skb does
			 * not fit into the cwnd_quota or the NIC's max-segs
			 * limitation, it will be split by the subflow's
			 * tcp_write_xmit which does the appropriate call to
			 * tcp_mss_split_point.
			 */
			limit = tcp_mss_split_point(meta_sk, skb, mss_now,
						    UINT_MAX / mss_now,
						    nonagle);

		if (sublimit)
			limit = min(limit, sublimit);

		if (skb->len > limit &&
		    unlikely(mptcp_fragment(meta_sk, skb, limit, gfp, reinject)))
			break;
        //ly add
		// if(skb&&skb->ifProtectionPacket==2)
		// {
		// 	printk(KERN_EMERG " our coding packet in xmit skb->seq = %u,skb->end_seq = %u,skb->len =%d\n",TCP_SKB_CB(skb)->seq,TCP_SKB_CB(skb)->end_seq,skb->len);
		// }
		// if(skb&&skb->ifProtectionPacket==0)
		// {
		// 	printk(KERN_EMERG " our data packet in xmit skb->seq = %u,skb->end_seq = %u,skb->len =%d\n",TCP_SKB_CB(skb)->seq,TCP_SKB_CB(skb)->end_seq,skb->len);
		// }
		// if(skb&&skb->ifProtectionPacket==1)
		// {
		// 	printk(KERN_EMERG " our retrans packet in xmit skb->seq = %u,skb->end_seq = %u,skb->len =%d\n",TCP_SKB_CB(skb)->seq,TCP_SKB_CB(skb)->end_seq,skb->len);
		// }
		//ly add
		//CG add
		// sknum = subsk->sk_num;
  //    	skdport = ntohs(subsk->sk_dport);
  //    	printk(KERN_EMERG "rmt  BUG: failure at %s:%d/%s()! \n",__FILE__, __LINE__, __FUNCTION__);
		// printk(KERN_EMERG "rmt mptcp_write_xmit_push  data level seq = %lu, end_seq =%lu, sport = %u, dport = %u\n",
		// 	TCP_SKB_CB(skb)->seq,TCP_SKB_CB(skb)->end_seq,sknum,skdport);
		// printk(KERN_EMERG "before  mptcp_skb_entail path_index_to_flag=%d, skb->path_mask=0x%08x \n",mptcp_pi_to_flag(subtp->mptcp->path_index),TCP_SKB_CB(skb)->path_mask);
		

		if (!mptcp_skb_entail(subsk, skb, reinject))
			break;
		/* Nagle is handled at the MPTCP-layer, so
		 * always push on the subflow
		 */
		//ly add
		if(skb&&skb->ifProtectionPacket==2)
		{
			__tcp_push_pending_frames(subsk, mss_now-24, TCP_NAGLE_PUSH);
		}
		else
			__tcp_push_pending_frames(subsk, mss_now, TCP_NAGLE_PUSH);
		
		//ly add

		//mengyuan//
		// sknum = subsk->sk_num;
  //    	skdport = ntohs(subsk->sk_dport);
  //    	 printk(KERN_EMERG "rmt  BUG: failure at %s:%d/%s()! \n",__FILE__, __LINE__, __FUNCTION__);
		// printk(KERN_EMERG "rmt mptcp_write_xmit_push  data level seq = %lu, end_seq =%lu, sport = %u, dport = %u\n",
		// 	TCP_SKB_CB(skb)->seq,TCP_SKB_CB(skb)->end_seq,sknum,skdport);
		// printk(KERN_EMERG "after __tcp_push_pending_frames path_index_to_flag=%d, skb->path_mask=0x%08x \n",mptcp_pi_to_flag(subtp->mptcp->path_index),TCP_SKB_CB(skb)->path_mask);
		//mengyuan//

		path_mask |= mptcp_pi_to_flag(subtp->mptcp->path_index);
		skb_mstamp_get(&skb->skb_mstamp);

		if (!reinject) {
			mptcp_check_sndseq_wrap(meta_tp,
						TCP_SKB_CB(skb)->end_seq -
						TCP_SKB_CB(skb)->seq);
			tcp_event_new_data_sent(meta_sk, skb);
		}

		tcp_minshall_update(meta_tp, mss_now, skb);

		if (reinject > 0) {
			__skb_unlink(skb, &mpcb->reinject_queue);
			kfree_skb(skb);
		}

		if (push_one)
			break;
	}

	mptcp_for_each_sk(mpcb, subsk) {
		subtp = tcp_sk(subsk);

		if (!(path_mask & mptcp_pi_to_flag(subtp->mptcp->path_index)))
			continue;

		/* We have pushed data on this subflow. We ignore the call to
		 * cwnd_validate in tcp_write_xmit as is_cwnd_limited will never
		 * be true (we never push more than what the cwnd can accept).
		 * We need to ensure that we call tcp_cwnd_validate with
		 * is_cwnd_limited set to true if we have filled the cwnd.
		 */
		tcp_cwnd_validate(subsk, tcp_packets_in_flight(subtp) >=
				  subtp->snd_cwnd);
	}

	return !meta_tp->packets_out && tcp_send_head(meta_sk);
}
/*mengyuan  end*/

void mptcp_write_space(struct sock *sk)
{
	mptcp_push_pending_frames(mptcp_meta_sk(sk));
}

u32 __mptcp_select_window(struct sock *sk)
{
	struct inet_connection_sock *icsk = inet_csk(sk);
	struct tcp_sock *tp = tcp_sk(sk), *meta_tp = mptcp_meta_tp(tp);
	struct sock *meta_sk = mptcp_meta_sk(sk);
	int mss, free_space, full_space, window;

	/* MSS for the peer's data.  Previous versions used mss_clamp
	 * here.  I don't know if the value based on our guesses
	 * of peer's MSS is better for the performance.  It's more correct
	 * but may be worse for the performance because of rcv_mss
	 * fluctuations.  --SAW  1998/11/1
	 */
	mss = icsk->icsk_ack.rcv_mss;
	free_space = tcp_space(meta_sk);
	full_space = min_t(int, meta_tp->window_clamp,
			tcp_full_space(meta_sk));

	if (mss > full_space)
		mss = full_space;

	if (free_space < (full_space >> 1)) {
		icsk->icsk_ack.quick = 0;

		if (tcp_memory_pressure)
			/* TODO this has to be adapted when we support different
			 * MSS's among the subflows.
			 */
			meta_tp->rcv_ssthresh = min(meta_tp->rcv_ssthresh,
						    4U * meta_tp->advmss);

		if (free_space < mss)
			return 0;
	}

	if (free_space > meta_tp->rcv_ssthresh)
		free_space = meta_tp->rcv_ssthresh;

	/* Don't do rounding if we are using window scaling, since the
	 * scaled window will not line up with the MSS boundary anyway.
	 */
	window = meta_tp->rcv_wnd;
	if (tp->rx_opt.rcv_wscale) {
		window = free_space;

		/* Advertise enough space so that it won't get scaled away.
		 * Import case: prevent zero window announcement if
		 * 1<<rcv_wscale > mss.
		 */
		if (((window >> tp->rx_opt.rcv_wscale) << tp->
		     rx_opt.rcv_wscale) != window)
			window = (((window >> tp->rx_opt.rcv_wscale) + 1)
				  << tp->rx_opt.rcv_wscale);
	} else {
		/* Get the largest window that is a nice multiple of mss.
		 * Window clamp already applied above.
		 * If our current window offering is within 1 mss of the
		 * free space we just keep it. This prevents the divide
		 * and multiply from happening most of the time.
		 * We also don't do any window rounding when the free space
		 * is too small.
		 */
		if (window <= free_space - mss || window > free_space)
			window = (free_space / mss) * mss;
		else if (mss == full_space &&
			 free_space > window + (full_space >> 1))
			window = free_space;
	}

	return window;
}

void mptcp_syn_options(const struct sock *sk, struct tcp_out_options *opts,
		       unsigned *remaining)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	opts->options |= OPTION_MPTCP;
	if (is_master_tp(tp)) {
		opts->mptcp_options |= OPTION_MP_CAPABLE | OPTION_TYPE_SYN;
		*remaining -= MPTCP_SUB_LEN_CAPABLE_SYN_ALIGN;
		opts->mp_capable.sender_key = tp->mptcp_loc_key;
		opts->dss_csum = !!sysctl_mptcp_checksum;
	} else {
		const struct mptcp_cb *mpcb = tp->mpcb;

		opts->mptcp_options |= OPTION_MP_JOIN | OPTION_TYPE_SYN;
		*remaining -= MPTCP_SUB_LEN_JOIN_SYN_ALIGN;
		opts->mp_join_syns.token = mpcb->mptcp_rem_token;
		opts->mp_join_syns.low_prio  = tp->mptcp->low_prio;
		opts->addr_id = tp->mptcp->loc_id;
		opts->mp_join_syns.sender_nonce = tp->mptcp->mptcp_loc_nonce;
	}
}

void mptcp_synack_options(struct request_sock *req,
			  struct tcp_out_options *opts, unsigned *remaining)
{
	struct mptcp_request_sock *mtreq;
	mtreq = mptcp_rsk(req);

	opts->options |= OPTION_MPTCP;
	/* MPCB not yet set - thus it's a new MPTCP-session */
	if (!mtreq->is_sub) {
		opts->mptcp_options |= OPTION_MP_CAPABLE | OPTION_TYPE_SYNACK;
		opts->mp_capable.sender_key = mtreq->mptcp_loc_key;
		opts->dss_csum = !!sysctl_mptcp_checksum || mtreq->dss_csum;
		*remaining -= MPTCP_SUB_LEN_CAPABLE_SYN_ALIGN;
	} else {
		opts->mptcp_options |= OPTION_MP_JOIN | OPTION_TYPE_SYNACK;
		opts->mp_join_syns.sender_truncated_mac =
				mtreq->mptcp_hash_tmac;
		opts->mp_join_syns.sender_nonce = mtreq->mptcp_loc_nonce;
		opts->mp_join_syns.low_prio = mtreq->low_prio;
		opts->addr_id = mtreq->loc_id;
		*remaining -= MPTCP_SUB_LEN_JOIN_SYNACK_ALIGN;
	}
}

void mptcp_established_options(struct sock *sk, struct sk_buff *skb,
			       struct tcp_out_options *opts, unsigned *size)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct mptcp_cb *mpcb = tp->mpcb;
	const struct tcp_skb_cb *tcb = skb ? TCP_SKB_CB(skb) : NULL;

	/* We are coming from tcp_current_mss with the meta_sk as an argument.
	 * It does not make sense to check for the options, because when the
	 * segment gets sent, another subflow will be chosen.
	 */
	if (!skb && is_meta_sk(sk))
		return;

	/* In fallback mp_fail-mode, we have to repeat it until the fallback
	 * has been done by the sender
	 */
	if (unlikely(tp->mptcp->send_mp_fail)) {
		opts->options |= OPTION_MPTCP;
		opts->mptcp_options |= OPTION_MP_FAIL;
		*size += MPTCP_SUB_LEN_FAIL;
		return;
	}

	if (unlikely(tp->send_mp_fclose)) {
		opts->options |= OPTION_MPTCP;
		opts->mptcp_options |= OPTION_MP_FCLOSE;
		opts->mp_capable.receiver_key = mpcb->mptcp_rem_key;
		*size += MPTCP_SUB_LEN_FCLOSE_ALIGN;
		return;
	}

	/* 1. If we are the sender of the infinite-mapping, we need the
	 *    MPTCPHDR_INF-flag, because a retransmission of the
	 *    infinite-announcment still needs the mptcp-option.
	 *
	 *    We need infinite_cutoff_seq, because retransmissions from before
	 *    the infinite-cutoff-moment still need the MPTCP-signalling to stay
	 *    consistent.
	 *
	 * 2. If we are the receiver of the infinite-mapping, we always skip
	 *    mptcp-options, because acknowledgments from before the
	 *    infinite-mapping point have already been sent out.
	 *
	 * I know, the whole infinite-mapping stuff is ugly...
	 *
	 * TODO: Handle wrapped data-sequence numbers
	 *       (even if it's very unlikely)
	 */
	if (unlikely(mpcb->infinite_mapping_snd) &&
	    ((mpcb->send_infinite_mapping && tcb &&
	      mptcp_is_data_seq(skb) &&
	      !(tcb->mptcp_flags & MPTCPHDR_INF) &&
	      !before(tcb->seq, tp->mptcp->infinite_cutoff_seq)) ||
	     !mpcb->send_infinite_mapping))
		return;

	if (unlikely(tp->mptcp->include_mpc)) {
		opts->options |= OPTION_MPTCP;
		opts->mptcp_options |= OPTION_MP_CAPABLE |
				       OPTION_TYPE_ACK;
		*size += MPTCP_SUB_LEN_CAPABLE_ACK_ALIGN;
		opts->mp_capable.sender_key = mpcb->mptcp_loc_key;
		opts->mp_capable.receiver_key = mpcb->mptcp_rem_key;
		opts->dss_csum = mpcb->dss_csum;

		if (skb)
			tp->mptcp->include_mpc = 0;
	}
	if (unlikely(tp->mptcp->pre_established)) {
		opts->options |= OPTION_MPTCP;
		opts->mptcp_options |= OPTION_MP_JOIN | OPTION_TYPE_ACK;
		*size += MPTCP_SUB_LEN_JOIN_ACK_ALIGN;
	}

	if (!tp->mptcp->include_mpc && !tp->mptcp->pre_established) {
		opts->options |= OPTION_MPTCP;
		opts->mptcp_options |= OPTION_DATA_ACK;
		/* If !skb, we come from tcp_current_mss and thus we always
		 * assume that the DSS-option will be set for the data-packet.
		 */
		if (skb && !mptcp_is_data_seq(skb)) {
			*size += MPTCP_SUB_LEN_ACK_ALIGN;
		} else {
			/* Doesn't matter, if csum included or not. It will be
			 * either 10 or 12, and thus aligned = 12
			 */
			*size += MPTCP_SUB_LEN_ACK_ALIGN +
				 MPTCP_SUB_LEN_SEQ_ALIGN;
		}

		*size += MPTCP_SUB_LEN_DSS_ALIGN;
	}

	if (unlikely(mpcb->addr_signal) && mpcb->pm_ops->addr_signal)
		mpcb->pm_ops->addr_signal(sk, size, opts, skb);

	if (unlikely(tp->mptcp->send_mp_prio) &&
	    MAX_TCP_OPTION_SPACE - *size >= MPTCP_SUB_LEN_PRIO_ALIGN) {
		opts->options |= OPTION_MPTCP;
		opts->mptcp_options |= OPTION_MP_PRIO;
		if (skb)
			tp->mptcp->send_mp_prio = 0;
		*size += MPTCP_SUB_LEN_PRIO_ALIGN;
	}

	return;
}

u16 mptcp_select_window(struct sock *sk)
{
	u16 new_win		= tcp_select_window(sk);
	struct tcp_sock *tp	= tcp_sk(sk);
	struct tcp_sock *meta_tp = mptcp_meta_tp(tp);

	meta_tp->rcv_wnd	= tp->rcv_wnd;
	meta_tp->rcv_wup	= meta_tp->rcv_nxt;

	return new_win;
}

void mptcp_options_write(__be32 *ptr, struct tcp_sock *tp,
			 const struct tcp_out_options *opts,
			 struct sk_buff *skb)
{
	if (unlikely(OPTION_MP_CAPABLE & opts->mptcp_options)) {
		struct mp_capable *mpc = (struct mp_capable *)ptr;

		mpc->kind = TCPOPT_MPTCP;

		if ((OPTION_TYPE_SYN & opts->mptcp_options) ||
		    (OPTION_TYPE_SYNACK & opts->mptcp_options)) {
			mpc->sender_key = opts->mp_capable.sender_key;
			mpc->len = MPTCP_SUB_LEN_CAPABLE_SYN;
			ptr += MPTCP_SUB_LEN_CAPABLE_SYN_ALIGN >> 2;
		} else if (OPTION_TYPE_ACK & opts->mptcp_options) {
			mpc->sender_key = opts->mp_capable.sender_key;
			mpc->receiver_key = opts->mp_capable.receiver_key;
			mpc->len = MPTCP_SUB_LEN_CAPABLE_ACK;
			ptr += MPTCP_SUB_LEN_CAPABLE_ACK_ALIGN >> 2;
		}

		mpc->sub = MPTCP_SUB_CAPABLE;
		mpc->ver = 0;
		mpc->a = opts->dss_csum;
		mpc->b = 0;
		mpc->rsv = 0;
		mpc->h = 1;
	}

	if (unlikely(OPTION_MP_JOIN & opts->mptcp_options)) {
		struct mp_join *mpj = (struct mp_join *)ptr;

		mpj->kind = TCPOPT_MPTCP;
		mpj->sub = MPTCP_SUB_JOIN;
		mpj->rsv = 0;

		if (OPTION_TYPE_SYN & opts->mptcp_options) {
			mpj->len = MPTCP_SUB_LEN_JOIN_SYN;
			mpj->u.syn.token = opts->mp_join_syns.token;
			mpj->u.syn.nonce = opts->mp_join_syns.sender_nonce;
			mpj->b = opts->mp_join_syns.low_prio;
			mpj->addr_id = opts->addr_id;
			ptr += MPTCP_SUB_LEN_JOIN_SYN_ALIGN >> 2;
		} else if (OPTION_TYPE_SYNACK & opts->mptcp_options) {
			mpj->len = MPTCP_SUB_LEN_JOIN_SYNACK;
			mpj->u.synack.mac =
				opts->mp_join_syns.sender_truncated_mac;
			mpj->u.synack.nonce = opts->mp_join_syns.sender_nonce;
			mpj->b = opts->mp_join_syns.low_prio;
			mpj->addr_id = opts->addr_id;
			ptr += MPTCP_SUB_LEN_JOIN_SYNACK_ALIGN >> 2;
		} else if (OPTION_TYPE_ACK & opts->mptcp_options) {
			mpj->len = MPTCP_SUB_LEN_JOIN_ACK;
			mpj->addr_id = 0; /* addr_id is rsv (RFC 6824, p. 21) */
			memcpy(mpj->u.ack.mac, &tp->mptcp->sender_mac[0], 20);
			ptr += MPTCP_SUB_LEN_JOIN_ACK_ALIGN >> 2;
		}
	}
	if (unlikely(OPTION_ADD_ADDR & opts->mptcp_options)) {
		struct mp_add_addr *mpadd = (struct mp_add_addr *)ptr;

		mpadd->kind = TCPOPT_MPTCP;
		if (opts->add_addr_v4) {
			mpadd->len = MPTCP_SUB_LEN_ADD_ADDR4;
			mpadd->sub = MPTCP_SUB_ADD_ADDR;
			mpadd->ipver = 4;
			mpadd->addr_id = opts->add_addr4.addr_id;
			mpadd->u.v4.addr = opts->add_addr4.addr;
			ptr += MPTCP_SUB_LEN_ADD_ADDR4_ALIGN >> 2;
		} else if (opts->add_addr_v6) {
			mpadd->len = MPTCP_SUB_LEN_ADD_ADDR6;
			mpadd->sub = MPTCP_SUB_ADD_ADDR;
			mpadd->ipver = 6;
			mpadd->addr_id = opts->add_addr6.addr_id;
			memcpy(&mpadd->u.v6.addr, &opts->add_addr6.addr,
			       sizeof(mpadd->u.v6.addr));
			ptr += MPTCP_SUB_LEN_ADD_ADDR6_ALIGN >> 2;
		}

		MPTCP_INC_STATS(sock_net((struct sock *)tp), MPTCP_MIB_ADDADDRTX);
	}
	if (unlikely(OPTION_REMOVE_ADDR & opts->mptcp_options)) {
		struct mp_remove_addr *mprem = (struct mp_remove_addr *)ptr;
		u8 *addrs_id;
		int id, len, len_align;

		len = mptcp_sub_len_remove_addr(opts->remove_addrs);
		len_align = mptcp_sub_len_remove_addr_align(opts->remove_addrs);

		mprem->kind = TCPOPT_MPTCP;
		mprem->len = len;
		mprem->sub = MPTCP_SUB_REMOVE_ADDR;
		mprem->rsv = 0;
		addrs_id = &mprem->addrs_id;

		mptcp_for_each_bit_set(opts->remove_addrs, id)
			*(addrs_id++) = id;

		/* Fill the rest with NOP's */
		if (len_align > len) {
			int i;
			for (i = 0; i < len_align - len; i++)
				*(addrs_id++) = TCPOPT_NOP;
		}

		ptr += len_align >> 2;

		MPTCP_INC_STATS(sock_net((struct sock *)tp), MPTCP_MIB_REMADDRTX);
	}
	if (unlikely(OPTION_MP_FAIL & opts->mptcp_options)) {
		struct mp_fail *mpfail = (struct mp_fail *)ptr;

		mpfail->kind = TCPOPT_MPTCP;
		mpfail->len = MPTCP_SUB_LEN_FAIL;
		mpfail->sub = MPTCP_SUB_FAIL;
		mpfail->rsv1 = 0;
		mpfail->rsv2 = 0;
		mpfail->data_seq = htonll(tp->mpcb->csum_cutoff_seq);

		ptr += MPTCP_SUB_LEN_FAIL_ALIGN >> 2;
	}
	if (unlikely(OPTION_MP_FCLOSE & opts->mptcp_options)) {
		struct mp_fclose *mpfclose = (struct mp_fclose *)ptr;

		mpfclose->kind = TCPOPT_MPTCP;
		mpfclose->len = MPTCP_SUB_LEN_FCLOSE;
		mpfclose->sub = MPTCP_SUB_FCLOSE;
		mpfclose->rsv1 = 0;
		mpfclose->rsv2 = 0;
		mpfclose->key = opts->mp_capable.receiver_key;

		ptr += MPTCP_SUB_LEN_FCLOSE_ALIGN >> 2;
	}

	if (OPTION_DATA_ACK & opts->mptcp_options) {
		if (!mptcp_is_data_seq(skb))
			ptr += mptcp_write_dss_data_ack(tp, skb, ptr);
		else
			ptr += mptcp_write_dss_data_seq(tp, skb, ptr);
	}
	if (unlikely(OPTION_MP_PRIO & opts->mptcp_options)) {
		struct mp_prio *mpprio = (struct mp_prio *)ptr;

		mpprio->kind = TCPOPT_MPTCP;
		mpprio->len = MPTCP_SUB_LEN_PRIO;
		mpprio->sub = MPTCP_SUB_PRIO;
		mpprio->rsv = 0;
		mpprio->b = tp->mptcp->low_prio;
		mpprio->addr_id = TCPOPT_NOP;

		ptr += MPTCP_SUB_LEN_PRIO_ALIGN >> 2;
	}
}

/* Sends the datafin */
void mptcp_send_fin(struct sock *meta_sk)
{
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct sk_buff *skb = tcp_write_queue_tail(meta_sk);
	int mss_now;

	if ((1 << meta_sk->sk_state) & (TCPF_CLOSE_WAIT | TCPF_LAST_ACK))
		meta_tp->mpcb->passive_close = 1;

	/* Optimization, tack on the FIN if we have a queue of
	 * unsent frames.  But be careful about outgoing SACKS
	 * and IP options.
	 */
	mss_now = mptcp_current_mss(meta_sk);

	if (tcp_send_head(meta_sk) != NULL) {
		TCP_SKB_CB(skb)->mptcp_flags |= MPTCPHDR_FIN;
		TCP_SKB_CB(skb)->end_seq++;
		meta_tp->write_seq++;
	} else {
		/* Socket is locked, keep trying until memory is available. */
		for (;;) {
			skb = alloc_skb_fclone(MAX_TCP_HEADER,
					       meta_sk->sk_allocation);
			if (skb)
				break;
			yield();
		}
		/* Reserve space for headers and prepare control bits. */
		skb_reserve(skb, MAX_TCP_HEADER);

		tcp_init_nondata_skb(skb, meta_tp->write_seq, TCPHDR_ACK);
		TCP_SKB_CB(skb)->end_seq++;
		TCP_SKB_CB(skb)->mptcp_flags |= MPTCPHDR_FIN;
		tcp_queue_skb(meta_sk, skb);
	}
	__tcp_push_pending_frames(meta_sk, mss_now, TCP_NAGLE_OFF);
}

void mptcp_send_active_reset(struct sock *meta_sk, gfp_t priority)
{
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct mptcp_cb *mpcb = meta_tp->mpcb;
	struct sock *sk = NULL, *sk_it = NULL, *tmpsk;

	if (!mpcb->cnt_subflows)
		return;

	WARN_ON(meta_tp->send_mp_fclose);

	/* First - select a socket */
	sk = mptcp_select_ack_sock(meta_sk);

	/* May happen if no subflow is in an appropriate state */
	if (!sk)
		return;

	/* We are in infinite mode - just send a reset */
	if (mpcb->infinite_mapping_snd || mpcb->infinite_mapping_rcv) {
		sk->sk_err = ECONNRESET;
		if (tcp_need_reset(sk->sk_state))
			tcp_send_active_reset(sk, priority);
		mptcp_sub_force_close(sk);
		return;
	}


	tcp_sk(sk)->send_mp_fclose = 1;
	/** Reset all other subflows */

	/* tcp_done must be handled with bh disabled */
	if (!in_serving_softirq())
		local_bh_disable();

	mptcp_for_each_sk_safe(mpcb, sk_it, tmpsk) {
		if (tcp_sk(sk_it)->send_mp_fclose)
			continue;

		sk_it->sk_err = ECONNRESET;
		if (tcp_need_reset(sk_it->sk_state))
			tcp_send_active_reset(sk_it, GFP_ATOMIC);
		mptcp_sub_force_close(sk_it);
	}

	if (!in_serving_softirq())
		local_bh_enable();

	tcp_send_ack(sk);
	inet_csk_reset_keepalive_timer(sk, inet_csk(sk)->icsk_rto);

	meta_tp->send_mp_fclose = 1;

	MPTCP_INC_STATS(sock_net(meta_sk), MPTCP_MIB_FASTCLOSETX);
}

static void mptcp_ack_retransmit_timer(struct sock *sk)
{
	struct sk_buff *skb;
	struct tcp_sock *tp = tcp_sk(sk);
	struct inet_connection_sock *icsk = inet_csk(sk);

	if (inet_csk(sk)->icsk_af_ops->rebuild_header(sk))
		goto out; /* Routing failure or similar */

	if (!tp->retrans_stamp)
		tp->retrans_stamp = tcp_time_stamp ? : 1;

	if (tcp_write_timeout(sk)) {
		MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_JOINACKRTO);
		tp->mptcp->pre_established = 0;
		sk_stop_timer(sk, &tp->mptcp->mptcp_ack_timer);
		tp->ops->send_active_reset(sk, GFP_ATOMIC);
		goto out;
	}

	skb = alloc_skb(MAX_TCP_HEADER, GFP_ATOMIC);
	if (skb == NULL) {
		sk_reset_timer(sk, &tp->mptcp->mptcp_ack_timer,
			       jiffies + icsk->icsk_rto);
		return;
	}

	/* Reserve space for headers and prepare control bits */
	skb_reserve(skb, MAX_TCP_HEADER);
	tcp_init_nondata_skb(skb, tp->snd_una, TCPHDR_ACK);

	MPTCP_INC_STATS(sock_net(sk), MPTCP_MIB_JOINACKRXMIT);

	if (tcp_transmit_skb(sk, skb, 0, GFP_ATOMIC) > 0) {
		/* Retransmission failed because of local congestion,
		 * do not backoff.
		 */
		if (!icsk->icsk_retransmits)
			icsk->icsk_retransmits = 1;
		sk_reset_timer(sk, &tp->mptcp->mptcp_ack_timer,
			       jiffies + icsk->icsk_rto);
		return;
	}


	icsk->icsk_retransmits++;
	icsk->icsk_rto = min(icsk->icsk_rto << 1, TCP_RTO_MAX);
	sk_reset_timer(sk, &tp->mptcp->mptcp_ack_timer,
		       jiffies + icsk->icsk_rto);
	if (retransmits_timed_out(sk, sysctl_tcp_retries1 + 1, 0, 0))
		__sk_dst_reset(sk);

out:;
}

void mptcp_ack_handler(unsigned long data)
{
	struct sock *sk = (struct sock *)data;
	struct sock *meta_sk = mptcp_meta_sk(sk);

	bh_lock_sock(meta_sk);
	if (sock_owned_by_user(meta_sk)) {
		/* Try again later */
		sk_reset_timer(sk, &tcp_sk(sk)->mptcp->mptcp_ack_timer,
			       jiffies + (HZ / 20));
		goto out_unlock;
	}

	if (sk->sk_state == TCP_CLOSE)
		goto out_unlock;
	if (!tcp_sk(sk)->mptcp->pre_established)
		goto out_unlock;

	mptcp_ack_retransmit_timer(sk);

	sk_mem_reclaim(sk);

out_unlock:
	bh_unlock_sock(meta_sk);
	sock_put(sk);
}

/* Similar to tcp_retransmit_skb
 *
 * The diff is that we handle the retransmission-stats (retrans_stamp) at the
 * meta-level.
 */
int mptcp_retransmit_skb(struct sock *meta_sk, struct sk_buff *skb)
{
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct sock *subsk;
	unsigned int limit, mss_now;
	int err = -1;

	/* Do not sent more than we queued. 1/4 is reserved for possible
	 * copying overhead: fragmentation, tunneling, mangling etc.
	 *
	 * This is a meta-retransmission thus we check on the meta-socket.
	 */
	if (atomic_read(&meta_sk->sk_wmem_alloc) >
	    min(meta_sk->sk_wmem_queued + (meta_sk->sk_wmem_queued >> 2), meta_sk->sk_sndbuf)) {
		return -EAGAIN;
	}

	/* We need to make sure that the retransmitted segment can be sent on a
	 * subflow right now. If it is too big, it needs to be fragmented.
	 */
	subsk = meta_tp->mpcb->sched_ops->get_subflow(meta_sk, skb, false);
	if (!subsk) {
		/* We want to increase icsk_retransmits, thus return 0, so that
		 * mptcp_retransmit_timer enters the desired branch.
		 */
		err = 0;
		goto failed;
	}
	mss_now = tcp_current_mss(subsk);

	/* If the segment was cloned (e.g. a meta retransmission), the header
	 * must be expanded/copied so that there is no corruption of TSO
	 * information.
	 */
	if (skb_unclone(skb, GFP_ATOMIC)) {
		err = -ENOMEM;
		goto failed;
	}

	/* Must have been set by mptcp_write_xmit before */
	BUG_ON(!tcp_skb_pcount(skb));

	limit = mss_now;
	/* skb->len > mss_now is the equivalent of tso_segs > 1 in
	 * tcp_write_xmit. Otherwise split-point would return 0.
	 */
	if (skb->len > mss_now && !tcp_urg_mode(meta_tp))
		limit = tcp_mss_split_point(meta_sk, skb, mss_now,
					    UINT_MAX / mss_now,
					    TCP_NAGLE_OFF);

	if (skb->len > limit &&
	    unlikely(mptcp_fragment(meta_sk, skb, limit,
				    GFP_ATOMIC, 0)))
		goto failed;

	if (!mptcp_skb_entail(subsk, skb, -1))
		goto failed;
	skb_mstamp_get(&skb->skb_mstamp);

	/* Update global TCP statistics. */
	MPTCP_INC_STATS(sock_net(meta_sk), MPTCP_MIB_RETRANSSEGS);

	/* Diff to tcp_retransmit_skb */

	/* Save stamp of the first retransmit. */
	if (!meta_tp->retrans_stamp)
		meta_tp->retrans_stamp = tcp_skb_timestamp(skb);

	__tcp_push_pending_frames(subsk, mss_now, TCP_NAGLE_PUSH);

	return 0;

failed:
	NET_INC_STATS_BH(sock_net(meta_sk), LINUX_MIB_TCPRETRANSFAIL);
	return err;
}

/* Similar to tcp_retransmit_timer
 *
 * The diff is that we have to handle retransmissions of the FAST_CLOSE-message
 * and that we don't have an srtt estimation at the meta-level.
 */
void mptcp_retransmit_timer(struct sock *meta_sk)
{
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct mptcp_cb *mpcb = meta_tp->mpcb;
	struct inet_connection_sock *meta_icsk = inet_csk(meta_sk);
	int err;

	/* In fallback, retransmission is handled at the subflow-level */
	if (!meta_tp->packets_out || mpcb->infinite_mapping_snd ||
	    mpcb->send_infinite_mapping)
		return;

	WARN_ON(tcp_write_queue_empty(meta_sk));

	if (!meta_tp->snd_wnd && !sock_flag(meta_sk, SOCK_DEAD) &&
	    !((1 << meta_sk->sk_state) & (TCPF_SYN_SENT | TCPF_SYN_RECV))) {
		/* Receiver dastardly shrinks window. Our retransmits
		 * become zero probes, but we should not timeout this
		 * connection. If the socket is an orphan, time it out,
		 * we cannot allow such beasts to hang infinitely.
		 */
		struct inet_sock *meta_inet = inet_sk(meta_sk);
		if (meta_sk->sk_family == AF_INET) {
			LIMIT_NETDEBUG(KERN_DEBUG "MPTCP: Peer %pI4:%u/%u unexpectedly shrunk window %u:%u (repaired)\n",
				       &meta_inet->inet_daddr,
				       ntohs(meta_inet->inet_dport),
				       meta_inet->inet_num, meta_tp->snd_una,
				       meta_tp->snd_nxt);
		}
#if IS_ENABLED(CONFIG_IPV6)
		else if (meta_sk->sk_family == AF_INET6) {
			LIMIT_NETDEBUG(KERN_DEBUG "MPTCP: Peer %pI6:%u/%u unexpectedly shrunk window %u:%u (repaired)\n",
				       &meta_sk->sk_v6_daddr,
				       ntohs(meta_inet->inet_dport),
				       meta_inet->inet_num, meta_tp->snd_una,
				       meta_tp->snd_nxt);
		}
#endif
		if (tcp_time_stamp - meta_tp->rcv_tstamp > TCP_RTO_MAX) {
			tcp_write_err(meta_sk);
			return;
		}

		mptcp_retransmit_skb(meta_sk, tcp_write_queue_head(meta_sk));
		goto out_reset_timer;
	}

	if (tcp_write_timeout(meta_sk))
		return;

	if (meta_icsk->icsk_retransmits == 0)
		NET_INC_STATS_BH(sock_net(meta_sk), LINUX_MIB_TCPTIMEOUTS);

	meta_icsk->icsk_ca_state = TCP_CA_Loss;

	err = mptcp_retransmit_skb(meta_sk, tcp_write_queue_head(meta_sk));
	if (err > 0) {
		/* Retransmission failed because of local congestion,
		 * do not backoff.
		 */
		if (!meta_icsk->icsk_retransmits)
			meta_icsk->icsk_retransmits = 1;
		inet_csk_reset_xmit_timer(meta_sk, ICSK_TIME_RETRANS,
					  min(meta_icsk->icsk_rto, TCP_RESOURCE_PROBE_INTERVAL),
					  TCP_RTO_MAX);
		return;
	}

	/* Increase the timeout each time we retransmit.  Note that
	 * we do not increase the rtt estimate.  rto is initialized
	 * from rtt, but increases here.  Jacobson (SIGCOMM 88) suggests
	 * that doubling rto each time is the least we can get away with.
	 * In KA9Q, Karn uses this for the first few times, and then
	 * goes to quadratic.  netBSD doubles, but only goes up to *64,
	 * and clamps at 1 to 64 sec afterwards.  Note that 120 sec is
	 * defined in the protocol as the maximum possible RTT.  I guess
	 * we'll have to use something other than TCP to talk to the
	 * University of Mars.
	 *
	 * PAWS allows us longer timeouts and large windows, so once
	 * implemented ftp to mars will work nicely. We will have to fix
	 * the 120 second clamps though!
	 */
	meta_icsk->icsk_backoff++;
	meta_icsk->icsk_retransmits++;

out_reset_timer:
	/* If stream is thin, use linear timeouts. Since 'icsk_backoff' is
	 * used to reset timer, set to 0. Recalculate 'icsk_rto' as this
	 * might be increased if the stream oscillates between thin and thick,
	 * thus the old value might already be too high compared to the value
	 * set by 'tcp_set_rto' in tcp_input.c which resets the rto without
	 * backoff. Limit to TCP_THIN_LINEAR_RETRIES before initiating
	 * exponential backoff behaviour to avoid continue hammering
	 * linear-timeout retransmissions into a black hole
	 */
	if (meta_sk->sk_state == TCP_ESTABLISHED &&
	    (meta_tp->thin_lto || sysctl_tcp_thin_linear_timeouts) &&
	    tcp_stream_is_thin(meta_tp) &&
	    meta_icsk->icsk_retransmits <= TCP_THIN_LINEAR_RETRIES) {
		meta_icsk->icsk_backoff = 0;
		/* We cannot do the same as in tcp_write_timer because the
		 * srtt is not set here.
		 */
		mptcp_set_rto(meta_sk);
	} else {
		/* Use normal (exponential) backoff */
		meta_icsk->icsk_rto = min(meta_icsk->icsk_rto << 1, TCP_RTO_MAX);
	}
	inet_csk_reset_xmit_timer(meta_sk, ICSK_TIME_RETRANS, meta_icsk->icsk_rto, TCP_RTO_MAX);

	return;
}

/* Modify values to an mptcp-level for the initial window of new subflows */
void mptcp_select_initial_window(int __space, __u32 mss, __u32 *rcv_wnd,
				__u32 *window_clamp, int wscale_ok,
				__u8 *rcv_wscale, __u32 init_rcv_wnd,
				 const struct sock *sk)
{
	struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;

	*window_clamp = mpcb->orig_window_clamp;
	__space = tcp_win_from_space(mpcb->orig_sk_rcvbuf);

	tcp_select_initial_window(__space, mss, rcv_wnd, window_clamp,
				  wscale_ok, rcv_wscale, init_rcv_wnd, sk);
}

static inline u64 mptcp_calc_rate(const struct sock *meta_sk, unsigned int mss,
				  unsigned int (*mss_cb)(struct sock *sk))
{
	struct sock *sk;
	u64 rate = 0;

	mptcp_for_each_sk(tcp_sk(meta_sk)->mpcb, sk) {
		struct tcp_sock *tp = tcp_sk(sk);
		int this_mss;
		u64 this_rate;

		if (!mptcp_sk_can_send(sk))
			continue;

		/* Do not consider subflows without a RTT estimation yet
		 * otherwise this_rate >>> rate.
		 */
		if (unlikely(!tp->srtt_us))
			continue;

		this_mss = mss_cb(sk);

		/* If this_mss is smaller than mss, it means that a segment will
		 * be splitted in two (or more) when pushed on this subflow. If
		 * you consider that mss = 1428 and this_mss = 1420 then two
		 * segments will be generated: a 1420-byte and 8-byte segment.
		 * The latter will introduce a large overhead as for a single
		 * data segment 2 slots will be used in the congestion window.
		 * Therefore reducing by ~2 the potential throughput of this
		 * subflow. Indeed, 1428 will be send while 2840 could have been
		 * sent if mss == 1420 reducing the throughput by 2840 / 1428.
		 *
		 * The following algorithm take into account this overhead
		 * when computing the potential throughput that MPTCP can
		 * achieve when generating mss-byte segments.
		 *
		 * The formulae is the following:
		 *  \sum_{\forall sub} ratio * \frac{mss * cwnd_sub}{rtt_sub}
		 * Where ratio is computed as follows:
		 *  \frac{mss}{\ceil{mss / mss_sub} * mss_sub}
		 *
		 * ratio gives the reduction factor of the theoretical
		 * throughput a subflow can achieve if MPTCP uses a specific
		 * MSS value.
		 */
		this_rate = div64_u64((u64)mss * mss * (USEC_PER_SEC << 3) *
				      max(tp->snd_cwnd, tp->packets_out),
				      (u64)tp->srtt_us *
				      DIV_ROUND_UP(mss, this_mss) * this_mss);
		rate += this_rate;
	}

	return rate;
}

static unsigned int __mptcp_current_mss(const struct sock *meta_sk,
					unsigned int (*mss_cb)(struct sock *sk))
{
	unsigned int mss = 0;
	u64 rate = 0;
	struct sock *sk;

	mptcp_for_each_sk(tcp_sk(meta_sk)->mpcb, sk) {
		int this_mss;
		u64 this_rate;

		if (!mptcp_sk_can_send(sk))
			continue;

		this_mss = mss_cb(sk);

		/* Same mss values will produce the same throughput. */
		if (this_mss == mss)
			continue;

		/* See whether using this mss value can theoretically improve
		 * the performances.
		 */
		this_rate = mptcp_calc_rate(meta_sk, this_mss, mss_cb);
		if (this_rate >= rate) {
			mss = this_mss;
			rate = this_rate;
		}
	}

	return mss;
}

unsigned int mptcp_current_mss(struct sock *meta_sk)
{
	unsigned int mss = __mptcp_current_mss(meta_sk, tcp_current_mss);

	/* If no subflow is available, we take a default-mss from the
	 * meta-socket.
	 */
	return !mss ? tcp_current_mss(meta_sk) : mss;
}

static unsigned int mptcp_select_size_mss(struct sock *sk)
{
	return tcp_sk(sk)->mss_cache;
}

int mptcp_select_size(const struct sock *meta_sk, bool sg)
{
	unsigned int mss = __mptcp_current_mss(meta_sk, mptcp_select_size_mss);

	if (sg) {
		if (mptcp_sk_can_gso(meta_sk)) {
			mss = SKB_WITH_OVERHEAD(2048 - MAX_TCP_HEADER);
		} else {
			int pgbreak = SKB_MAX_HEAD(MAX_TCP_HEADER);

			if (mss >= pgbreak &&
			    mss <= pgbreak + (MAX_SKB_FRAGS - 1) * PAGE_SIZE)
				mss = pgbreak;
		}
	}

	return !mss ? tcp_sk(meta_sk)->mss_cache : mss;
}

int mptcp_check_snd_buf(const struct tcp_sock *tp)
{
	const struct sock *sk;
	u32 rtt_max = tp->srtt_us;
	u64 bw_est;

	if (!tp->srtt_us)
		return tp->reordering + 1;

	mptcp_for_each_sk(tp->mpcb, sk) {
		if (!mptcp_sk_can_send(sk))
			continue;

		if (rtt_max < tcp_sk(sk)->srtt_us)
			rtt_max = tcp_sk(sk)->srtt_us;
	}

	bw_est = div64_u64(((u64)tp->snd_cwnd * rtt_max) << 16,
				(u64)tp->srtt_us);

	return max_t(unsigned int, (u32)(bw_est >> 16),
			tp->reordering + 1);
}

unsigned int mptcp_xmit_size_goal(const struct sock *meta_sk, u32 mss_now,
				  int large_allowed)
{
	struct sock *sk;
	u32 xmit_size_goal = 0;

	if (large_allowed && mptcp_sk_can_gso(meta_sk)) {
		mptcp_for_each_sk(tcp_sk(meta_sk)->mpcb, sk) {
			int this_size_goal;

			if (!mptcp_sk_can_send(sk))
				continue;

			this_size_goal = tcp_xmit_size_goal(sk, mss_now, 1);
			if (this_size_goal > xmit_size_goal)
				xmit_size_goal = this_size_goal;
		}
	}

	return max(xmit_size_goal, mss_now);
}

/* Similar to tcp_trim_head - but we correctly copy the DSS-option */
int mptcp_trim_head(struct sock *sk, struct sk_buff *skb, u32 len)
{
	if (skb_cloned(skb)) {
		if (pskb_expand_head(skb, 0, 0, GFP_ATOMIC))
			return -ENOMEM;
	}

	__pskb_trim_head(skb, len);

	TCP_SKB_CB(skb)->seq += len;
	skb->ip_summed = CHECKSUM_PARTIAL;

	skb->truesize	     -= len;
	sk->sk_wmem_queued   -= len;
	sk_mem_uncharge(sk, len);
	sock_set_flag(sk, SOCK_QUEUE_SHRUNK);

	/* Any change of skb->len requires recalculation of tso factor. */
	if (tcp_skb_pcount(skb) > 1)
		tcp_set_skb_tso_segs(sk, skb, tcp_skb_mss(skb));

	return 0;
}
