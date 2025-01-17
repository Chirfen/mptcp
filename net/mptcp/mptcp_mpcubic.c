/*
 * TCP CUBIC: Binary Increase Congestion control for TCP v2.3
 * Home page:
 *      http://netsrv.csc.ncsu.edu/twiki/bin/view/Main/BIC
 * This is from the implementation of CUBIC TCP in
 * Sangtae Ha, Injong Rhee and Lisong Xu,
 *  "CUBIC: A New TCP-Friendly High-Speed TCP Variant"
 *  in ACM SIGOPS Operating System Review, July 2008.
 * Available from:
 *  http://netsrv.csc.ncsu.edu/export/cubic_a_new_tcp_2008.pdf
 *
 * CUBIC integrates a new slow start algorithm, called HyStart.
 * The details of HyStart are presented in
 *  Sangtae Ha and Injong Rhee,
 *  "Taming the Elephants: New TCP Slow Start", NCSU TechReport 2008.
 * Available from:
 *  http://netsrv.csc.ncsu.edu/export/hystart_techreport_2008.pdf
 *
 * All testing results are available from:
 * http://netsrv.csc.ncsu.edu/wiki/index.php/TCP_Testing
 *
 * Unless CUBIC is enabled and congestion window is large
 * this behaves the same as the original Reno.
 */

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/math64.h>
#include <net/tcp.h>

#include <net/mptcp.h>

#define BICTCP_BETA_SCALE    1024	/* Scale factor beta calculation
					 * max_cwnd = snd_cwnd * beta
					 */
#define	BICTCP_HZ		10	/* BIC HZ 2^10 = 1024 */

/* Two methods of hybrid slow start */
#define HYSTART_ACK_TRAIN	0x1
#define HYSTART_DELAY		0x2

/* Number of delay samples for detecting the increase of delay */
#define HYSTART_MIN_SAMPLES	8
#define HYSTART_DELAY_MIN	(4U<<3)
#define HYSTART_DELAY_MAX	(16U<<3)
#define HYSTART_DELAY_THRESH(x)	clamp(x, HYSTART_DELAY_MIN, HYSTART_DELAY_MAX)

static int fast_convergence __read_mostly = 1;
static int beta __read_mostly = 717;	/* = 717/1024 (BICTCP_BETA_SCALE) */
static int initial_ssthresh __read_mostly;
static int bic_scale __read_mostly = 41;
static int tcp_friendliness __read_mostly = 1;

static int hystart __read_mostly = 0;
static int hystart_detect __read_mostly = HYSTART_ACK_TRAIN | HYSTART_DELAY;
static int hystart_low_window __read_mostly = 16;
static int hystart_ack_delta __read_mostly = 2;

static u32 cube_rtt_scale __read_mostly;
static u32 beta_scale __read_mostly;
static u64 cube_factor __read_mostly;

/* Note parameters that are used for precomputing scale factors are read-only */
module_param(fast_convergence, int, 0644);
MODULE_PARM_DESC(fast_convergence, "turn on/off fast convergence");
module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "beta for multiplicative increase");
module_param(initial_ssthresh, int, 0644);
MODULE_PARM_DESC(initial_ssthresh, "initial value of slow start threshold");
module_param(bic_scale, int, 0444);
MODULE_PARM_DESC(bic_scale, "scale (scaled by 1024) value for bic function (bic_scale/1024)");
module_param(tcp_friendliness, int, 0644);
MODULE_PARM_DESC(tcp_friendliness, "turn on/off tcp friendliness");
module_param(hystart, int, 0644);
MODULE_PARM_DESC(hystart, "turn on/off hybrid slow start algorithm");
module_param(hystart_detect, int, 0644);
MODULE_PARM_DESC(hystart_detect, "hyrbrid slow start detection mechanisms"
		 " 1: packet-train 2: delay 3: both packet-train and delay");
module_param(hystart_low_window, int, 0644);
MODULE_PARM_DESC(hystart_low_window, "lower bound cwnd for hybrid slow start");
module_param(hystart_ack_delta, int, 0644);
MODULE_PARM_DESC(hystart_ack_delta, "spacing between ack's indicating train (msecs)");

/* BIC TCP Parameters */
struct bictcp {
	u32	cnt;		/* increase cwnd by 1 after ACKs */
	u32	last_max_cwnd;	/* last maximum snd_cwnd */
	u32	loss_cwnd;	/* congestion window at last loss */
	u32	last_cwnd;	/* the last snd_cwnd */
	u32	last_time;	/* time when updated last_cwnd */
	u32	bic_origin_point;/* origin point of bic function */
	u32	bic_K;		/* time to origin point
				   from the beginning of the current epoch */
	u32	delay_min;	/* min delay (msec << 3) */
	u32	epoch_start;	/* beginning of an epoch */
	u32	ack_cnt;	/* number of acks */
	u32	tcp_cwnd;	/* estimated tcp cwnd */
#define ACK_RATIO_SHIFT	4
#define ACK_RATIO_LIMIT (32u << ACK_RATIO_SHIFT)
	u16	delayed_ack;	/* estimate the ratio of Packets/ACKs << 4 */
	u8	sample_cnt;	/* number of samples to decide curr_rtt */
	u8	found;		/* the exit point is found? */
	u32	round_start;	/* beginning of each round */
	u32	end_seq;	/* end_seq of the round */
	u32	last_ack;	/* last time when the ACK spacing is close */
	u32	curr_rtt;	/* the minimum rtt of current round */
};

static inline void bictcp_reset(struct bictcp *ca)
{
	ca->cnt = 0;
	ca->last_max_cwnd = 0;
	ca->last_cwnd = 0;
	ca->last_time = 0;
	ca->bic_origin_point = 0;
	ca->bic_K = 0;
	ca->delay_min = 0;
	ca->epoch_start = 0;
	ca->delayed_ack = 2 << ACK_RATIO_SHIFT;
	ca->ack_cnt = 0;
	ca->tcp_cwnd = 0;
	ca->found = 0;
}


//added by zhuo
/* Scaling is done in the numerator with alpha_scale_num and in the denominator
 * with alpha_scale_den.
 *
 * To downscale, we just need to use alpha_scale.
 *
 * We have: alpha_scale = alpha_scale_num / (alpha_scale_den ^ 2)
 */
static int alpha_scale_den = 10;
static int alpha_scale_num = 32;
static int alpha_scale = 12;

struct mptcp_ccc {
	u64	alpha;
	u64 alpha_friendly;
	//added by zhuo
	u32 mpcubic_start_cwnd;
	bool	forced_update;
};

static inline int mptcp_ccc_sk_can_send(const struct sock *sk)
{
	return mptcp_sk_can_send(sk) && tcp_sk(sk)->srtt_us;
}

static inline u64 mptcp_get_alpha(const struct sock *meta_sk)
{
	return ((struct mptcp_ccc *)inet_csk_ca(meta_sk))->alpha;
}

static inline void mptcp_set_alpha(const struct sock *meta_sk, u64 alpha)
{
	((struct mptcp_ccc *)inet_csk_ca(meta_sk))->alpha = alpha;
}

static inline u64 mptcp_get_alpha_friendly(const struct sock *meta_sk)
{
	return ((struct mptcp_ccc *)inet_csk_ca(meta_sk))->alpha_friendly;
}

static inline void mptcp_set_alpha_friendly(const struct sock *meta_sk, u64 alpha_friendly)
{
	((struct mptcp_ccc *)inet_csk_ca(meta_sk))->alpha_friendly = alpha_friendly;
}

static inline u32 mptcp_get_mpcubic_start_cwnd(const struct sock *meta_sk)
{
	return ((struct mptcp_ccc *)inet_csk_ca(meta_sk))->mpcubic_start_cwnd;
}

static inline void mptcp_set_mpcubic_start_cwnd(const struct sock *meta_sk, u32 mpcubic_start_cwnd)
{
	((struct mptcp_ccc *)inet_csk_ca(meta_sk))->mpcubic_start_cwnd = mpcubic_start_cwnd;
}

static inline u64 mptcp_ccc_scale(u32 val, int scale)
{
	return (u64) val << scale;
}

static inline bool mptcp_get_forced(const struct sock *meta_sk)
{
	return ((struct mptcp_ccc *)inet_csk_ca(meta_sk))->forced_update;
}

static inline void mptcp_set_forced(const struct sock *meta_sk, bool force)
{
	((struct mptcp_ccc *)inet_csk_ca(meta_sk))->forced_update = force;
}

static void mptcp_ccc_recalc_alpha(const struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	const struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;
	const struct sock *sub_sk;
	int best_cwnd = 0, best_rtt = 0, can_send = 0;
	u64 max_numerator = 0, sum_denominator = 0, alpha = 1,alpha_temp1,alpha_temp2;

	if (!mpcb)
		return;

	/* Only one subflow left - fall back to normal reno-behavior
	 * (set alpha to 1)
	 */
	if (mpcb->cnt_established <= 1)
		goto exit;

	/* Do regular alpha-calculation for multiple subflows */

	/* Find the max numerator of the alpha-calculation */
	mptcp_for_each_sk(mpcb, sub_sk) {
		struct tcp_sock *sub_tp = tcp_sk(sub_sk);
		u64 tmp;

		if (!mptcp_ccc_sk_can_send(sub_sk))
			continue;

		can_send++;

		/* We need to look for the path, that provides the max-value.
		 * Integer-overflow is not possible here, because
		 * tmp will be in u64.
		 */
//		tmp = div64_u64(mptcp_ccc_scale(sub_tp->snd_cwnd,
//				alpha_scale_num), (u64)sub_tp->srtt_us * sub_tp->srtt_us);
		tmp = div64_u64(mptcp_ccc_scale(sub_tp->snd_cwnd*sub_tp->snd_cwnd,
				alpha_scale_num), (u64)(sub_tp->srtt_us>>10) * (sub_tp->srtt_us>>10) * (sub_tp->srtt_us>>10) * (sub_tp->srtt_us>>10) );
		//
		//printk(KERN_INFO "10.0.0.2: snd_cwnd:%d,alph_scale_num:%d,tmp:%lld,srtt_us:%d\n",sub_tp->snd_cwnd,alpha_scale_num,tmp,sub_tp->srtt_us);
		if (tmp >= max_numerator) {
			max_numerator = tmp;
			best_cwnd = sub_tp->snd_cwnd;
			best_rtt = sub_tp->srtt_us;
		}
	}

	/* No subflow is able to send - we don't care anymore */
	if (unlikely(!can_send))
		goto exit;

	/* Calculate the denominator */
	mptcp_for_each_sk(mpcb, sub_sk) {
		struct tcp_sock *sub_tp = tcp_sk(sub_sk);

		if (!mptcp_ccc_sk_can_send(sub_sk))
			continue;

		sum_denominator += div_u64(
				mptcp_ccc_scale(sub_tp->snd_cwnd,
						10) * best_rtt,
						sub_tp->srtt_us);
		//printk(KERN_INFO "10.0.0.2: deno:%lld\n",div_u64(mptcp_ccc_scale(sub_tp->snd_cwnd,10) * best_rtt,	sub_tp->srtt_us));
	}
	sum_denominator *= sum_denominator;
	//printk(KERN_INFO "10.0.0.2: sum_denominator:%lld\n",sum_denominator);

	if (unlikely(!sum_denominator)) {
		pr_err("%s: sum_denominator == 0, cnt_established:%d\n",
		       __func__, mpcb->cnt_established);
		mptcp_for_each_sk(mpcb, sub_sk) {
			struct tcp_sock *sub_tp = tcp_sk(sub_sk);
			pr_err("%s: pi:%d, state:%d\n, rtt:%u, cwnd: %u",
			       __func__, sub_tp->mptcp->path_index,
			       sub_sk->sk_state, sub_tp->srtt_us,
			       sub_tp->snd_cwnd);
		}
	}
	
	alpha_temp1=div64_u64(mptcp_ccc_scale(best_cwnd*best_cwnd,36), sum_denominator);
	alpha_temp2=div64_u64(mptcp_ccc_scale(tp->snd_cwnd*tp->snd_cwnd,36), sum_denominator);

	//if(alpha_temp1> (u64)mptcp_ccc_scale(1,63) || alpha_temp2> (u64)mptcp_ccc_scale(1,63))
	  //printk(KERN_INFO "10.0.0.2: alpha_temp1:%lld alpha_temp2:%lld\n quite large\n",alpha_temp1,alpha_temp2);

	alpha= alpha_temp1*alpha_temp2>>16;
	
	//printk(KERN_INFO "10.0.0.2: calculated alpha%lld,alpha_temp1%lld,alpha_temp2:%lld,best%d,current:%d\n",alpha,alpha_temp1,alpha_temp2,best_cwnd,tp->snd_cwnd);
	if (unlikely(!alpha))
		alpha = 1;

exit:
	mptcp_set_alpha(mptcp_meta_sk(sk), alpha);
}

static void mptcp_ccc_recalc_alpha_friendly(const struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	const struct mptcp_cb *mpcb = tcp_sk(sk)->mpcb;
	const struct sock *sub_sk;
	int best_cwnd = 0, best_rtt = 0, can_send = 0;
	u64 max_numerator = 0, sum_denominator = 0, alpha_friendly = 1;

	if (!mpcb)
		return;

	/* Only one subflow left - fall back to normal reno-behavior
	 * (set alpha to 1)
	 */
	if (mpcb->cnt_established <= 1)
		goto exit;

	/* Do regular alpha-calculation for multiple subflows */

	/* Find the max numerator of the alpha-calculation */
	mptcp_for_each_sk(mpcb, sub_sk) {
		struct tcp_sock *sub_tp = tcp_sk(sub_sk);
		u64 tmp;

		if (!mptcp_ccc_sk_can_send(sub_sk))
			continue;

		can_send++;

		/* We need to look for the path, that provides the max-value.
		 * Integer-overflow is not possible here, because
		 * tmp will be in u64.
		 */
//		tmp = div64_u64(mptcp_ccc_scale(sub_tp->snd_cwnd,
//				alpha_scale_num), (u64)sub_tp->srtt_us * sub_tp->srtt_us);
		tmp = div64_u64(mptcp_ccc_scale(sub_tp->snd_cwnd,
				alpha_scale_num), (u64)(sub_tp->srtt_us>>10) * (sub_tp->srtt_us>>10)  );
		//printk(KERN_INFO "10.0.0.2: alpha friendly snd_cwnd:%d,alph_scale_num:%d,tmp:%lld,srtt_us:%d\n",sub_tp->snd_cwnd,alpha_scale_num,tmp,sub_tp->srtt_us);
		if (tmp >= max_numerator) {
			max_numerator = tmp;
			best_cwnd = sub_tp->snd_cwnd;
			best_rtt = sub_tp->srtt_us;
		}
	}

	/* No subflow is able to send - we don't care anymore */
	if (unlikely(!can_send))
		goto exit;

	/* Calculate the denominator */
	mptcp_for_each_sk(mpcb, sub_sk) {
		struct tcp_sock *sub_tp = tcp_sk(sub_sk);

		if (!mptcp_ccc_sk_can_send(sub_sk))
			continue;

		sum_denominator += div_u64(
				mptcp_ccc_scale(sub_tp->snd_cwnd,
						10) * best_rtt,
						sub_tp->srtt_us);
		//printk(KERN_INFO "10.0.0.2: friendly deno:%lld\n",div_u64(mptcp_ccc_scale(sub_tp->snd_cwnd,10) * best_rtt,			sub_tp->srtt_us));
	}
	sum_denominator *= sum_denominator;
	//printk(KERN_INFO "10.0.0.2: friendly sum_denominator:%lld\n",sum_denominator);

	if (unlikely(!sum_denominator)) {
		pr_err("%s: sum_denominator == 0, cnt_established:%d\n",
		       __func__, mpcb->cnt_established);
		mptcp_for_each_sk(mpcb, sub_sk) {
			struct tcp_sock *sub_tp = tcp_sk(sub_sk);
			pr_err("%s: pi:%d, state:%d\n, rtt:%u, cwnd: %u",
			       __func__, sub_tp->mptcp->path_index,
			       sub_sk->sk_state, sub_tp->srtt_us,
			       sub_tp->snd_cwnd);
		}
	}

	alpha_friendly = div64_u64(mptcp_ccc_scale(best_cwnd*tp->snd_cwnd,36), sum_denominator);
	//printk(KERN_INFO "10.0.0.2: calculated alpha friendly%lld,best%d,current:%d\n",alpha_friendly,best_cwnd,tp->snd_cwnd);

	if (unlikely(!alpha_friendly))
		alpha_friendly = 1;

exit:
	mptcp_set_alpha_friendly(mptcp_meta_sk(sk), alpha_friendly);
}

static void mptcp_ccc_init(struct sock *sk)
{
	if (mptcp(tcp_sk(sk))) {
		mptcp_set_forced(mptcp_meta_sk(sk), 0);
		mptcp_set_alpha(mptcp_meta_sk(sk), 1);
	}
	/* If we do not mptcp, behave like reno: return */
}

static void mptcp_ccc_cwnd_event(struct sock *sk, enum tcp_ca_event event)
{
	if (event == CA_EVENT_LOSS)
		mptcp_ccc_recalc_alpha(sk);
}

static void mptcp_ccc_set_state(struct sock *sk, u8 ca_state)
{
	if (!mptcp(tcp_sk(sk)))
		return;

	mptcp_set_forced(mptcp_meta_sk(sk), 1);
}

static void mptcp_ccc_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	const struct mptcp_cb *mpcb = tp->mpcb;
	int snd_cwnd;

	if (!mptcp(tp)) {
		tcp_reno_cong_avoid(sk, ack, acked);
		return;
	}

	if (!tcp_is_cwnd_limited(sk))
		return;

	if (tp->snd_cwnd <= tp->snd_ssthresh) {
		/* In "safe" area, increase. */
		tcp_slow_start(tp, acked);
		mptcp_ccc_recalc_alpha(sk);
		return;
	}

	if (mptcp_get_forced(mptcp_meta_sk(sk))) {
		mptcp_ccc_recalc_alpha(sk);
		mptcp_set_forced(mptcp_meta_sk(sk), 0);
	}

	if (mpcb->cnt_established > 1) {
		u64 alpha = mptcp_get_alpha(mptcp_meta_sk(sk));

		/* This may happen, if at the initialization, the mpcb
		 * was not yet attached to the sock, and thus
		 * initializing alpha failed.
		 */
		if (unlikely(!alpha))
			alpha = 1;

		snd_cwnd = (int) div_u64 ((u64) mptcp_ccc_scale(1, alpha_scale),
						alpha);

		/* snd_cwnd_cnt >= max (scale * tot_cwnd / alpha, cwnd)
		 * Thus, we select here the max value.
		 */
		if (snd_cwnd < tp->snd_cwnd)
			snd_cwnd = tp->snd_cwnd;
	} else {
		snd_cwnd = tp->snd_cwnd;
	}

	if (tp->snd_cwnd_cnt >= snd_cwnd) {
		if (tp->snd_cwnd < tp->snd_cwnd_clamp) {
			tp->snd_cwnd++;
			mptcp_ccc_recalc_alpha(sk);
		}

		tp->snd_cwnd_cnt = 0;
	} else {
		tp->snd_cwnd_cnt++;
	}
}


static inline u32 bictcp_clock(void)
{
#if HZ < 1000
	return ktime_to_ms(ktime_get_real());
#else
	return jiffies_to_msecs(jiffies);
#endif
}

static inline void bictcp_hystart_reset(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	ca->round_start = ca->last_ack = bictcp_clock();
	ca->end_seq = tp->snd_nxt;
	ca->curr_rtt = 0;
	ca->sample_cnt = 0;
}

static void bictcp_init(struct sock *sk)
{
	struct bictcp *ca = inet_csk_ca(sk);

	bictcp_reset(ca);
	ca->loss_cwnd = 0;

	if (hystart)
		bictcp_hystart_reset(sk);

	if (!hystart && initial_ssthresh)
		tcp_sk(sk)->snd_ssthresh = initial_ssthresh;

//added by zhuo
	if (mptcp(tcp_sk(sk))) {
		mptcp_set_forced(mptcp_meta_sk(sk), 0);
		mptcp_set_alpha(mptcp_meta_sk(sk), 1);
	}
	/* If we do not mptcp, behave like reno: return */

}

/* calculate the cubic root of x using a table lookup followed by one
 * Newton-Raphson iteration.
 * Avg err ~= 0.195%
 */
static u32 cubic_root(u64 a)
{
	u32 x, b, shift;
	/*
	 * cbrt(x) MSB values for x MSB values in [0..63].
	 * Precomputed then refined by hand - Willy Tarreau
	 *
	 * For x in [0..63],
	 *   v = cbrt(x << 18) - 1
	 *   cbrt(x) = (v[x] + 10) >> 6
	 */
	static const u8 v[] = {
		/* 0x00 */    0,   54,   54,   54,  118,  118,  118,  118,
		/* 0x08 */  123,  129,  134,  138,  143,  147,  151,  156,
		/* 0x10 */  157,  161,  164,  168,  170,  173,  176,  179,
		/* 0x18 */  181,  185,  187,  190,  192,  194,  197,  199,
		/* 0x20 */  200,  202,  204,  206,  209,  211,  213,  215,
		/* 0x28 */  217,  219,  221,  222,  224,  225,  227,  229,
		/* 0x30 */  231,  232,  234,  236,  237,  239,  240,  242,
		/* 0x38 */  244,  245,  246,  248,  250,  251,  252,  254,
	};

	b = fls64(a);
	if (b < 7) {
		/* a in [0..63] */
		return ((u32)v[(u32)a] + 35) >> 6;
	}

	b = ((b * 84) >> 8) - 1;
	shift = (a >> (b * 3));

	x = ((u32)(((u32)v[shift] + 10) << b)) >> 6;

	/*
	 * Newton-Raphson iteration
	 *                         2
	 * x    = ( 2 * x  +  a / x  ) / 3
	 *  k+1          k         k
	 */
	x = (2 * x + (u32)div64_u64(a, (u64)x * (u64)(x - 1)));
	x = ((x * 341) >> 10);
	return x;
}

/*
 * Compute congestion window to use.
 */
//static inline void bictcp_update(struct bictcp *ca, u32 cwnd, u32 acked)

static inline void bictcp_update(struct bictcp *ca, u32 cwnd, u32 acked,u64 alpha,u64 alpha_friendly,struct sock *sk)
{
//	u32 delta, bic_target, max_cnt;
	u32 bic_target, max_cnt;
	u64 delta;
	u64 offs, t;
    u32 in_tcp_friendly=0;
	ca->ack_cnt += acked;	/* count the number of ACKed packets */

	//test case 2
	//alpha=1<<12;
	//alpha_friendly=1<<14;

	//test case 1
	//alpha=1;
	//alpha_friendly=1;

	if (ca->last_cwnd == cwnd &&
	    (s32)(tcp_time_stamp - ca->last_time) <= HZ / 32)
		return;

	/* The CUBIC function can update ca->cnt at most once per jiffy.
	 * On all cwnd reduction events, ca->epoch_start is set to 0,
	 * which will force a recalculation of ca->cnt.
	 */
	if (ca->epoch_start && tcp_time_stamp == ca->last_time)
		goto tcp_friendliness;

	ca->last_cwnd = cwnd;
	ca->last_time = tcp_time_stamp;

	if (ca->epoch_start == 0) {
		ca->epoch_start = tcp_time_stamp;	/* record beginning */
		ca->ack_cnt = acked;			/* start counting */
		ca->tcp_cwnd = cwnd;			/* syn with cubic */

		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->bic_origin_point = cwnd;
			//added by zhuo
			mptcp_set_mpcubic_start_cwnd(mptcp_meta_sk(sk),cwnd);
			printk(KERN_INFO "10.0.0.2: first bic_K %u\n",ca->bic_K);
		} else {
			/* Compute new K based on
			 * (wmax-cwnd) * (srtt>>3 / HZ) / c * 2^(3*bictcp_HZ)
			 */
			ca->bic_K = cubic_root(cube_factor
					       * (ca->last_max_cwnd - cwnd));
			ca->bic_origin_point = ca->last_max_cwnd;
			//added by zhuo
			mptcp_set_mpcubic_start_cwnd(mptcp_meta_sk(sk),cwnd);
			printk(KERN_INFO "10.0.0.2: first bic_K %u,cwnd:%u,last_max_cwnd:%u,cube_factor:%u\n",ca->bic_K,cwnd,ca->last_max_cwnd,cube_factor);
		}
	}

	/* cubic function - calc*/
	/* calculate c * time^3 / rtt,
	 *  while considering overflow in calculation of time^3
	 * (so time^3 is done by using 64 bit)
	 * and without the support of division of 64bit numbers
	 * (so all divisions are done by using 32 bit)
	 *  also NOTE the unit of those veriables
	 *	  time  = (t - K) / 2^bictcp_HZ
	 *	  c = bic_scale >> 10
	 * rtt  = (srtt >> 3) / HZ
	 * !!! The following code does not have overflow problems,
	 * if the cwnd < 1 million packets !!!
	 */

	t = (s32)(tcp_time_stamp - ca->epoch_start);
	t += msecs_to_jiffies(ca->delay_min >> 3);
	/* change the unit from HZ to bictcp_HZ */
	t <<= BICTCP_HZ;
	do_div(t, HZ);

	if (t < ca->bic_K)		/* t - K */
		offs = ca->bic_K - t;
	else
		offs = t - ca->bic_K;

	/* c/rtt * (t-K)^3 */
	delta = (cube_rtt_scale * offs * offs * offs) >> (10+3*BICTCP_HZ);
/*
	if(alpha!=1&&alpha<(1<<16))
	{
		delta=alpha*delta>>16;
		printk(KERN_INFO "10.0.0.2: alpha:%lld\n",alpha);
	}
*/

	if (t < ca->bic_K)                            /* below origin*/
		bic_target = ca->bic_origin_point - delta;
	else                                          /* above origin*/
		bic_target = ca->bic_origin_point + delta;

	/* cubic function - calc bictcp_cnt*/
	if (bic_target > cwnd) {
		ca->cnt = cwnd / (bic_target - cwnd);
	} else {
		ca->cnt = 100 * cwnd;              /* very small increment*/
	}

	/*
	 * The initial growth of cubic function may be too conservative
	 * when the available bandwidth is still unknown.
	 */
	if (ca->last_max_cwnd == 0 && ca->cnt > 20)
		ca->cnt = 20;	/* increase cwnd 5% per RTT */

//tcp_friendliness:
	/* TCP Friendly */
	if (tcp_friendliness) {
		u32 scale = beta_scale;

		u32 temp_cwnd=ca->tcp_cwnd;
		u32 temp_cnt=ca->ack_cnt;
		printk(KERN_INFO "10.0.0.2: original tcp_cwnd:%u,ack_cnt:%u\n",temp_cwnd,temp_cnt);

		delta = (cwnd * scale) >> 3;
		while (ca->ack_cnt > delta) {		/* update tcp cwnd */
			ca->ack_cnt -= delta;
			ca->tcp_cwnd++;
		}

		if (ca->tcp_cwnd > cwnd) {	/* if bic is slower than tcp */
			delta = ca->tcp_cwnd - cwnd;
			max_cnt = cwnd / delta;
			if (ca->cnt > max_cnt)
			{
				ca->cnt = max_cnt;
				in_tcp_friendly=1;
				//printk(KERN_INFO "10.0.0.2: in tcp friendly regin;\n");
			}
		}

		ca->tcp_cwnd=temp_cwnd;
		ca->ack_cnt=temp_cnt;
	}

	if(in_tcp_friendly==0)
	{
	/* c/rtt * (t-K)^3 */
		printk(KERN_INFO "10.0.0.2: not in friendly region;bic_target:%d,delta:%lld,cwnd:%d,ca bic_K:%u\n",bic_target,delta,cwnd,ca->bic_K);
		u32 mpcubic_start_cwnd=mptcp_get_mpcubic_start_cwnd(mptcp_meta_sk(sk));
		u32 mpcubic_bic_K;
		if (ca->last_max_cwnd <= mpcubic_start_cwnd) {
			mpcubic_bic_K = 0;
			printk(KERN_INFO "10.0.0.2: second bic_K 0\n");
		} else {
			/* Compute new K based on
			 * (wmax-cwnd) * (srtt>>3 / HZ) / c * 2^(3*bictcp_HZ)
			 */
			if(alpha!=1&&alpha<(1<<16))
			{
			    mpcubic_bic_K = cubic_root( (cube_factor * (ca->bic_origin_point - mpcubic_start_cwnd)<<16) /alpha);
			    printk(KERN_INFO "10.0.0.2: alpha:%lld,ca bic_k:%u,cube_factor:%u,orgigin_point:%u,start_cwnd:%u,t:%d,cube_rtt_scale:%u\n",alpha,mpcubic_bic_K,cube_factor,ca->bic_origin_point,mpcubic_start_cwnd,t,cube_rtt_scale);
			}
			else
			{
				mpcubic_bic_K = cubic_root(cube_factor * (ca->bic_origin_point - mpcubic_start_cwnd));
				printk(KERN_INFO "10.0.0.2: second bic_k: %u mpcubic_start_cwnd:%u,origin_point%u\n",mpcubic_bic_K,mpcubic_start_cwnd,ca->bic_origin_point);
			}
		}

		if (t < mpcubic_bic_K)		/* t - K */
			offs = mpcubic_bic_K - t;
		else
			offs = t - mpcubic_bic_K;

		/* c/rtt * (t-K)^3 */
		delta = (cube_rtt_scale * offs * offs * offs) >> (10+3*BICTCP_HZ);

		if(alpha!=1&&alpha<(1<<16))
		{
			delta=(alpha*delta)>>16;
			//printk(KERN_INFO "10.0.0.2: alpha:%lld\n",alpha);
		}

		if (t < mpcubic_bic_K)                            /* below origin*/
			bic_target = ca->bic_origin_point - delta;
		else                                          /* above origin*/
			bic_target = ca->bic_origin_point + delta;

		printk(KERN_INFO "10.0.0.2: not in friendly region;bic_target:%d,delta:%lld,cwnd:%d,alpha:%lld,cube_rtt_scale:%u\n",bic_target,delta,cwnd,alpha,cube_rtt_scale);

		/* cubic function - calc bictcp_cnt*/
		if (bic_target > cwnd) {
			ca->cnt = cwnd / (bic_target - cwnd);
		} else {
			ca->cnt = 100 * cwnd;              /* very small increment*/
		}
	
		/*
		 * The initial growth of cubic function may be too conservative
		 * when the available bandwidth is still unknown.
		 */
		if (ca->last_max_cwnd == 0 && ca->cnt > 20)
			ca->cnt = 20;	/* increase cwnd 5% per RTT */
	
	}else
	{
tcp_friendliness:
	/* TCP Friendly */
	if (tcp_friendliness) {
		u32 scale = beta_scale;

		delta = (cwnd * scale) >> 3;

		//printk(KERN_INFO "10.0.0.2: in friendly region;delta:%lld,cwnd:%d\n",delta,cwnd);

		if(alpha_friendly!=1&&alpha_friendly<(1<<16))
		{
			delta=((delta<<32)/alpha_friendly)>>16;
			//printk(KERN_INFO "10.0.0.2: alpha friendly:%lld\n",alpha_friendly);
		}

		//printk(KERN_INFO "10.0.0.2: in friendly region;delta:%lld,cwnd:%d,alpha_friendly:%lld\n",delta,cwnd,alpha_friendly);
		//printk(KERN_INFO "10.0.0.2: increase test  tcp_cwnd:%u,ack_cnt:%u\n",ca->tcp_cwnd,ca->ack_cnt);

		while (ca->ack_cnt > delta) {		/* update tcp cwnd */
			ca->ack_cnt -= delta;
			ca->tcp_cwnd++;
		}

		printk(KERN_INFO "10.0.0.2: target tcp_cwnd:%u,ack_cnt\n",ca->tcp_cwnd,ca->ack_cnt);

		if (ca->tcp_cwnd > cwnd) {	/* if bic is slower than tcp */
			delta = ca->tcp_cwnd - cwnd;
			max_cnt = cwnd / delta;
			if (ca->cnt > max_cnt)
			{
				ca->cnt = max_cnt;
				//in_tcp_friendly=1;
			}
		}

		}

	}

	/* The maximum rate of cwnd increase CUBIC allows is 1 packet per
	 * 2 packets ACKed, meaning cwnd grows at 1.5x per RTT.
	 */
	ca->cnt = max(ca->cnt, 2U);
}

//masked by zhuo
/*static void bictcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);


	if (!tcp_is_cwnd_limited(sk))
		return;

	if (tp->snd_cwnd <= tp->snd_ssthresh) {
		if (hystart && after(ack, ca->end_seq))
			bictcp_hystart_reset(sk);
		tcp_slow_start(tp, acked);
	} else {
		bictcp_update(ca, tp->snd_cwnd, acked);
		tcp_cong_avoid_ai_new(tp, ca->cnt, acked);
	}
}
*/

static void bictcp_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	const struct mptcp_cb *mpcb = tp->mpcb;
	int snd_cwnd;

	if (!mptcp(tp)) {
		tcp_reno_cong_avoid(sk, ack, acked);
		return;
	}

	if (!tcp_is_cwnd_limited(sk))
		return;

	if (tp->snd_cwnd <= tp->snd_ssthresh) {
		/* In "safe" area, increase. */
		if (hystart && after(ack, ca->end_seq))
			bictcp_hystart_reset(sk);
		tcp_slow_start(tp, acked);
		mptcp_ccc_recalc_alpha(sk);
		mptcp_ccc_recalc_alpha_friendly(sk);
		return;
	}

	if (mptcp_get_forced(mptcp_meta_sk(sk))) {
		mptcp_ccc_recalc_alpha(sk);
		mptcp_set_forced(mptcp_meta_sk(sk), 0);
	}

	u64 alpha,alpha_friendly;
	if (mpcb->cnt_established > 1) {
		alpha = mptcp_get_alpha(mptcp_meta_sk(sk));
		alpha_friendly=mptcp_get_alpha_friendly(mptcp_meta_sk(sk));
		/* This may happen, if at the initialization, the mpcb
		 * was not yet attached to the sock, and thus
		 * initializing alpha failed.
		 */
		if (unlikely(!alpha))
		{
			alpha = 1;
			alpha_friendly = 1;
		}

	} else {
		alpha = 1;
		alpha_friendly =1;
	}
	
	bictcp_update(ca, tp->snd_cwnd, acked,alpha,alpha_friendly,sk);
	tcp_cong_avoid_ai_new(tp, ca->cnt, acked);
	mptcp_ccc_recalc_alpha(sk);
	mptcp_ccc_recalc_alpha_friendly(sk);
}


static u32 bictcp_recalc_ssthresh(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	ca->epoch_start = 0;	/* end of epoch */

	/* Wmax and fast convergence */
	if (tp->snd_cwnd < ca->last_max_cwnd && fast_convergence)
		ca->last_max_cwnd = (tp->snd_cwnd * (BICTCP_BETA_SCALE + beta))
			/ (2 * BICTCP_BETA_SCALE);
	else
		ca->last_max_cwnd = tp->snd_cwnd;

	ca->loss_cwnd = tp->snd_cwnd;

	return max((tp->snd_cwnd * beta) / BICTCP_BETA_SCALE, 2U);
}

static u32 bictcp_undo_cwnd(struct sock *sk)
{
	struct bictcp *ca = inet_csk_ca(sk);

	return max(tcp_sk(sk)->snd_cwnd, ca->loss_cwnd);
}

static void bictcp_state(struct sock *sk, u8 new_state)
{
	if (new_state == TCP_CA_Loss) {
		bictcp_reset(inet_csk_ca(sk));
		bictcp_hystart_reset(sk);
	}

//added by zhuo
	if (!mptcp(tcp_sk(sk)))
		return;

	mptcp_set_forced(mptcp_meta_sk(sk), 1);
}

static void hystart_update(struct sock *sk, u32 delay)
{
	struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);

	if (!(ca->found & hystart_detect)) {
		u32 now = bictcp_clock();

		/* first detection parameter - ack-train detection */
		if ((s32)(now - ca->last_ack) <= hystart_ack_delta) {
			ca->last_ack = now;
			if ((s32)(now - ca->round_start) > ca->delay_min >> 4)
				ca->found |= HYSTART_ACK_TRAIN;
		}

		/* obtain the minimum delay of more than sampling packets */
		if (ca->sample_cnt < HYSTART_MIN_SAMPLES) {
			if (ca->curr_rtt == 0 || ca->curr_rtt > delay)
				ca->curr_rtt = delay;

			ca->sample_cnt++;
		} else {
			if (ca->curr_rtt > ca->delay_min +
			    HYSTART_DELAY_THRESH(ca->delay_min>>4))
				ca->found |= HYSTART_DELAY;
		}
		/*
		 * Either one of two conditions are met,
		 * we exit from slow start immediately.
		 */
		if (ca->found & hystart_detect)
			tp->snd_ssthresh = tp->snd_cwnd;
	}
}

/* Track delayed acknowledgment ratio using sliding window
 * ratio = (15*ratio + sample) / 16
 */
static void bictcp_acked(struct sock *sk, u32 cnt, s32 rtt_us)
{
	const struct inet_connection_sock *icsk = inet_csk(sk);
	const struct tcp_sock *tp = tcp_sk(sk);
	struct bictcp *ca = inet_csk_ca(sk);
	u32 delay;

	if (icsk->icsk_ca_state == TCP_CA_Open) {
		u32 ratio = ca->delayed_ack;

		ratio -= ca->delayed_ack >> ACK_RATIO_SHIFT;
		ratio += cnt;

		ca->delayed_ack = clamp(ratio, 1U, ACK_RATIO_LIMIT);
	}

	/* Some calls are for duplicates without timetamps */
	if (rtt_us < 0)
		return;

	/* Discard delay samples right after fast recovery */
	if (ca->epoch_start && (s32)(tcp_time_stamp - ca->epoch_start) < HZ)
		return;

	delay = (rtt_us << 3) / USEC_PER_MSEC;
	if (delay == 0)
		delay = 1;

	/* first time call or link delay decreases */
	if (ca->delay_min == 0 || ca->delay_min > delay)
		ca->delay_min = delay;

	/* hystart triggers when cwnd is larger than some threshold */
	if (hystart && tp->snd_cwnd <= tp->snd_ssthresh &&
	    tp->snd_cwnd >= hystart_low_window)
		hystart_update(sk, delay);
}

static struct tcp_congestion_ops cubictcp __read_mostly = {
	.init		= bictcp_init,
	.ssthresh	= bictcp_recalc_ssthresh,
	.cong_avoid	= bictcp_cong_avoid,
	.set_state	= bictcp_state,
//added cwnd event by zhuo
	.cwnd_event	= mptcp_ccc_cwnd_event,
	.undo_cwnd	= bictcp_undo_cwnd,
	.pkts_acked     = bictcp_acked,
	.owner		= THIS_MODULE,
	.name		= "mpcubic",
	
};

static int __init cubictcp_register(void)
{
	BUILD_BUG_ON(sizeof(struct bictcp) > ICSK_CA_PRIV_SIZE);
	BUILD_BUG_ON(sizeof(struct mptcp_ccc) > ICSK_CA_PRIV_SIZE);

	/* Precompute a bunch of the scaling factors that are used per-packet
	 * based on SRTT of 100ms
	 */

	beta_scale = 8*(BICTCP_BETA_SCALE+beta) / 3
		/ (BICTCP_BETA_SCALE - beta);

	cube_rtt_scale = (bic_scale * 10);	/* 1024*c/rtt */

	/* calculate the "K" for (wmax-cwnd) = c/rtt * K^3
	 *  so K = cubic_root( (wmax-cwnd)*rtt/c )
	 * the unit of K is bictcp_HZ=2^10, not HZ
	 *
	 *  c = bic_scale >> 10
	 *  rtt = 100ms
	 *
	 * the following code has been designed and tested for
	 * cwnd < 1 million packets
	 * RTT < 100 seconds
	 * HZ < 1,000,00  (corresponding to 10 nano-second)
	 */

	/* 1/c * 2^2*bictcp_HZ * srtt */
	cube_factor = 1ull << (10+3*BICTCP_HZ); /* 2^40 */

	/* divide by bic_scale and by constant Srtt (100ms) */
	do_div(cube_factor, bic_scale * 10);

	return tcp_register_congestion_control(&cubictcp);
}

static void __exit cubictcp_unregister(void)
{
	tcp_unregister_congestion_control(&cubictcp);
}

module_init(cubictcp_register);
module_exit(cubictcp_unregister);

MODULE_AUTHOR("Sangtae Ha, Stephen Hemminger");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("CUBIC TCP");
MODULE_VERSION("2.3");

