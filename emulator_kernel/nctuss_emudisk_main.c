#include <linux/module.h>
#include <linux/io.h>
#include <linux/genhd.h>
#include <linux/hdreg.h>
#include <linux/moduleparam.h>
#include <linux/ide.h>
#include <linux/stat.h>
#include <linux/kfifo.h>
#include <linux/semaphore.h>
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/sched/rt.h>
#include <linux/ktime.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/time.h>
#include <linux/kernel.h>
#include <linux/ktime.h>
#include <linux/spinlock_types.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>

#include <linux/if_ether.h>
#include <linux/skbuff.h>
#include <linux/inet.h>
#include <linux/types.h>
#include <linux/udp.h>
#include <linux/ip.h>
#include <linux/netdevice.h>

#include <asm/delay.h>
#include <asm/fiq.h>
#include <asm/unaligned.h>

//#pragma GCC push_options
//#pragma GCC optimize ("-Os") // Not very usefull. The kernel module size goes from 21472 to 21456...


#if 1 // disable printkk
#define printkk(fmt, ...) do{ } while(0)
#else
#define printkk(fmt, ...) \
	do { printk(fmt, ##__VA_ARGS__); } while(0)
#endif

//#define printk(fmt, ...) do{ } while(0)


//#define TRACE_STAT

/*
Measure the time the emulator kernel spends between OS pause and OS resume for each I/O request.

*/
//#define MEASURE_EMULATOR_KERNEL_OVERHEAD

#define PAUSE_OS_CALLER_STRAGETY_TRANSFER_DATA 1
#define PAUSE_OS_CALLER_STRAGETY_GET_NEXT_RESPONSE 2
#define PAUSE_OS_CALLER_PROCESSING_TASK_GET_NEXT_RESPONSE 3


//#define USE_FIQ
//#define DEBUG_FIQ
#define COMBINE_REQ_DATA
#define DIFFERENT_SUBMIT_TIME

static void __iomem *my_ipi_base;


const unsigned long memory_storage_start = 0x07000000; // The start address of the system memory that can be used as backing store.
const unsigned long memory_storage_size = 0x19000000; // The size of the backing store. (512-112)=400MB


#define NIPQUAD(addr) \
    ((unsigned char *)&addr)[0], \
    ((unsigned char *)&addr)[1], \
    ((unsigned char *)&addr)[2], \
    ((unsigned char *)&addr)[3]


struct nctuss_netpoll {
	struct net_device *dev;
	__be32 local_ip, remote_ip;
	u16 local_port, remote_port;
	unsigned char local_mac[6], remote_mac[6];
};

extern void nctuss_takeover_emacps(struct net_device *ndev, void *func, char *memory_buffer_virt, char *memory_buffer_phy);
extern void nctuss_restore_emacps(struct net_device *ndev);
extern void nctuss_poll_emacps(struct net_device *ndev);
//extern void nctuss_send_udp_emacps(struct nctuss_netpoll *nn, const char *msg, int len);
extern void nctuss_xemacps_send_skb(struct sk_buff *skb, struct net_device *ndev);
extern void nctuss_xemacps_return_skb(struct sk_buff *skb);

////extern void nctuss_emacps_reset(struct net_device *ndev);

extern struct net_device *dev_get_by_name(struct net *net, const char *name);
extern struct net init_net;

extern void nctuss_gt_stop(void);
extern void nctuss_gt_resume(void);
extern void *nctuss_gt_get_gt_counter_base(void);
extern u32 nctuss_gt_get_counter_value(void);
extern spinlock_t nctuss_gt_spinlock;

extern void * nctuss_get_my_ipi_base(void);

extern void nctuss_ttc_stop(void);
extern void nctuss_ttc_resume(void);

extern void nctuss_smp_invoke_function(void *func); // defined in arch/arm/kernel/smp.c

extern void wakeup_softirqd(void); // In softirq.c. EXPORT_SYMBOL added by us!

extern void nctuss_l2x0_lock(void); // In cache-l2x0.c
extern void nctuss_l2x0_unlock(void);



// The number of parallel requests that the emulated disk can handle. 
#define MAX_DISK_REQUESTS_IN_PARALLEL 8

#define SECTOR_SIZE 512
#define MAX_REQUEST_SECTOR_NUM 128
#define MAX_SKB_DATA_SIZE 2048
#define MAX_EMULATOR_CHANNELS 32

static long speed_factor = 1000;
module_param(speed_factor, long, S_IRUGO);
static long disk_channels = 1;
module_param(disk_channels, long, S_IRUGO);
static long server_port_offset = 0;
module_param(server_port_offset, long, S_IRUGO);


typedef struct
{
	long speed_factor;
	
	long size;
	unsigned char *data;
	
	spinlock_t lock;
	struct request_queue *queue;
	struct gendisk *gd;
	int major;

	long request_inflight;


	struct task_struct *processing_task;

	bool stopping; // For letting the processing thread to know that we are unloading.

	ktime_t next_response_to_reply_time_kt;
	u32 next_response_to_reply_num;
	u32 next_response_to_reply_ids[MAX_DISK_REQUESTS_IN_PARALLEL];
	u64 latest_response_time; // The latest response that was submitted to the kernel. We check with the emulator starting from this time for the next completed requests.

	struct nctuss_netpoll nn;
	struct net_device *ndev;

	u32 udp_send_reply_id;
	bool skb_replied;
	struct sk_buff *skb; // the replied skb
	u64 current_waiting_reply_id;

} nctuss_emudisk_t;

static nctuss_emudisk_t  Emudisk;

#if defined(TRACE_STAT) || defined(MEASURE_EMULATOR_KERNEL_OVERHEAD)
typedef struct
{
	u32 from_who; // 1 = stragety, 2 = processing, 3 = softirq complete
	u32 request_id;
	u32 queue_state;
	u32 data1;
	u32 pending_request_ids[MAX_DISK_REQUESTS_IN_PARALLEL];
	
} nctuss_emudisk_stats_t;

#define nctuss_emudisk_stats_max_num (50*50*10)
static nctuss_emudisk_stats_t nctuss_emudisk_stats[nctuss_emudisk_stats_max_num];
static long nctuss_emudisk_stats_num = 0;
#endif



struct request_id_to_req
{
	u32 request_id;
	struct request *req;
	int req_sectors;
	struct sk_buff *skbs_for_read[MAX_REQUEST_SECTOR_NUM];
	struct sk_buff *skbs_for_write[MAX_REQUEST_SECTOR_NUM];
	char *data;
};

struct request_id_to_req request_id_to_req_mapping[MAX_DISK_REQUESTS_IN_PARALLEL];

static int nctuss_put_request_id_to_req_mapping(u32 request_id, struct request *req, int req_sectors, struct sk_buff ***skbs_for_read, struct sk_buff ***skbs_for_write, char **data)
{
	int i;
#ifdef TRACE_STAT
	for(i=0; i<disk_channels; i++) {
		nctuss_emudisk_stats[nctuss_emudisk_stats_num].pending_request_ids[i] = request_id_to_req_mapping[i].request_id;
	}
	nctuss_emudisk_stats[nctuss_emudisk_stats_num].from_who = 4;
	nctuss_emudisk_stats_num++;
#endif

	for(i=0; i<disk_channels; i++) {
		if(request_id_to_req_mapping[i].request_id == 0) {
			request_id_to_req_mapping[i].request_id = request_id;
			request_id_to_req_mapping[i].req = req;
			*skbs_for_read = request_id_to_req_mapping[i].skbs_for_read;
			*skbs_for_write = request_id_to_req_mapping[i].skbs_for_write;
			*data = request_id_to_req_mapping[i].data;
			request_id_to_req_mapping[i].req_sectors = req_sectors;
			
			return 0;
		}
	}
	printk(KERN_ALERT "nctuss_emudisk: nctuss_put_request_id_to_req_mapping error!\n");
	while(1);
	
	return -1;
}

static int nctuss_get_request_id_to_req_mapping(u32 request_id, struct request **req, int *req_sectors, struct sk_buff ***skbs_for_read, struct sk_buff ***skbs_for_write, char **data)
{
	int i;

#ifdef TRACE_STAT
	for(i=0; i<disk_channels; i++) {
		nctuss_emudisk_stats[nctuss_emudisk_stats_num].pending_request_ids[i] = request_id_to_req_mapping[i].request_id;
	}
	nctuss_emudisk_stats[nctuss_emudisk_stats_num].from_who = 5;
	nctuss_emudisk_stats_num++;
#endif

	for(i=0; i<disk_channels; i++) {
		if(request_id_to_req_mapping[i].request_id == request_id) {
			*req = request_id_to_req_mapping[i].req;
			request_id_to_req_mapping[i].request_id = 0;
			*skbs_for_read = request_id_to_req_mapping[i].skbs_for_read;
			*skbs_for_write = request_id_to_req_mapping[i].skbs_for_write;
			*data = request_id_to_req_mapping[i].data;
			*req_sectors = request_id_to_req_mapping[i].req_sectors;
			return 0;
		}
	}
	printk(KERN_ALERT "nctuss_emudisk: nctuss_get_request_id_to_req_mapping error!\n");
	while(1);
	
	return -1;	
}


enum NctussEmuRequestType {
	RequestType_reset = 1,
	RequestType_submitReqeust,
	RequestType_getNextResponses,
	RequestType_readWriteData // data1 = r/w (read=0, write=1), data2 = sector. actual data (512bytes) comes after struct nctuss_emu_request.
};

enum NctussEmuReplyType {
	ReplyType_ok = 1,
	ReplyType_error
};


struct nctuss_emu_request {
	u32 id;
	u32 type;
	u32 data1;
	u32 data2;
	u32 data3;
	u32 data4;
	u32 data5;
	u32 data6;
	u32 id_check;
};

struct nctuss_emu_reply {
	u32 id;
	u32 type;
	u32 data1;
	u32 data2;
	u32 data3;
	u32 data4;
	//Used for replying "RequestType_getNextResponses". The ids of the requets that will complete next.
	u32 io_request_ids[MAX_EMULATOR_CHANNELS];
	u32 id_check;	
};


static void emulator_return_reply_skb(struct sk_buff *skb)
{
	nctuss_xemacps_return_skb(skb);
}

static void nctuss_receive_skb(struct sk_buff *skb)
{
	int ip_len, udp_len, eth_len;
	struct udphdr *udph;
	struct iphdr *iph;
	struct ethhdr *mh;
	struct nctuss_emu_reply *reply;
	u8 *data;
	u32 data_len;

	udp_len = sizeof(*udph);
	ip_len = udp_len + sizeof(*iph);
	eth_len = ip_len + ETH_HLEN + NET_IP_ALIGN;

	mh = eth_hdr(skb);
	
	if(skb->protocol == cpu_to_be16(ETH_P_IP) ) {
		iph = (struct iphdr*)skb->data;
		printkk(KERN_ALERT "IP -: saddr=%u.%u.%u.%u, daddr=%u.%u.%u.%u\n", NIPQUAD(iph->saddr), NIPQUAD(iph->daddr));

		udph = (struct udphdr*)(skb->data + (iph->ihl*4));
		printkk(KERN_ALERT "UDP -: source=%d, dest=%d\n", ntohs(udph->source), ntohs(udph->dest));

		if(Emudisk.nn.local_ip == iph->daddr && Emudisk.nn.remote_ip == iph->saddr && Emudisk.nn.local_port == ntohs(udph->dest) && Emudisk.nn.remote_port == ntohs(udph->source)) {
			printkk(KERN_ALERT "IP: saddr=%u.%u.%u.%u, daddr=%u.%u.%u.%u\n", NIPQUAD(iph->saddr), NIPQUAD(iph->daddr));
			printkk(KERN_ALERT "UDP: source=%d, dest=%d\n", ntohs(udph->source), ntohs(udph->dest));

			
			data = skb->data + (iph->ihl*4) + sizeof(struct udphdr);
			reply = (struct nctuss_emu_reply *)data;
			data_len = skb->len - sizeof(struct udphdr) - (iph->ihl*4);

			if(reply->id == Emudisk.current_waiting_reply_id) {

				if(unlikely(data_len > MAX_SKB_DATA_SIZE)) {
					printk(KERN_ALERT "nctuss_emudisk: nctuss_receive_skb: data_len(%d) > %d\n", data_len, MAX_SKB_DATA_SIZE);
					while(1);
				}
				
				skb->data = skb->data + ((iph->ihl*4) + sizeof(struct udphdr));
				Emudisk.skb = skb;
				Emudisk.skb_replied = true;
			}
			else {
				printk(KERN_ALERT "nctuss_receive_skb: reply->id=%u, Emudisk.current_waiting_reply_id=%llu\n", reply->id, Emudisk.current_waiting_reply_id);
			}
		}
		else {
			emulator_return_reply_skb(skb);
		}
		
	}else {
		emulator_return_reply_skb(skb);
	}
	
}

static int open_ethernet_channel(char *memory_buffer_virt, char *memory_buffer_phy)
{
	nctuss_takeover_emacps(Emudisk.ndev, nctuss_receive_skb, memory_buffer_virt, memory_buffer_phy);
	return 0;
}

static int close_ethernet_channel(void)
{
	nctuss_restore_emacps(Emudisk.ndev); 
	return 0;
}

static void skb_udp_finish(struct sk_buff *skb)
{
	
	int eth_len, ip_len, udp_len;
	struct udphdr *udph;
	struct iphdr *iph;
	struct ethhdr *eth;
	struct nctuss_netpoll *nn;

	nn = &Emudisk.nn;

	udp_len = skb->len + sizeof(*udph);
	ip_len = eth_len = udp_len + sizeof(*iph);


	skb_push(skb, sizeof(*udph));
	skb_reset_transport_header(skb);
	udph = udp_hdr(skb);
	udph->source = htons(nn->local_port);
	udph->dest = htons(nn->remote_port);
	udph->len = htons(udp_len);
	udph->check = 0;
	udph->check = csum_tcpudp_magic(nn->local_ip,
					nn->remote_ip,
					udp_len, IPPROTO_UDP,
					csum_partial(udph, udp_len, 0));
	if (udph->check == 0)
		udph->check = CSUM_MANGLED_0;

	skb_push(skb, sizeof(*iph));
	skb_reset_network_header(skb);
	iph = ip_hdr(skb);

	/* iph->version = 4; iph->ihl = 5; */
	put_unaligned(0x45, (unsigned char *)iph);
	iph->tos	  = 0;
	put_unaligned(htons(ip_len), &(iph->tot_len));
	iph->id 	  = 0;
	iph->frag_off = 0;
	iph->ttl	  = 64;
	iph->protocol = IPPROTO_UDP;
	iph->check	  = 0;
	put_unaligned(nn->local_ip, &(iph->saddr));
	put_unaligned(nn->remote_ip, &(iph->daddr));
	iph->check	  = ip_fast_csum((unsigned char *)iph, iph->ihl);

	eth = (struct ethhdr *) skb_push(skb, ETH_HLEN);
	skb_reset_mac_header(skb);
	skb->protocol = eth->h_proto = htons(ETH_P_IP);
	//memcpy(eth->h_source, nn->dev->dev_addr, ETH_ALEN);
	memcpy(eth->h_source, nn->local_mac, ETH_ALEN);
	memcpy(eth->h_dest, nn->remote_mac, ETH_ALEN);

	skb->dev = nn->dev;
}

struct sk_buff* nctuss_xemacps_start_xmit_get_next_skb_buff(struct net_device *ndev);
static void communicate_with_emulator(struct sk_buff *skb, struct sk_buff **skb_reply)
{
	//ktime_t start_time_kt, current_time_kt;
	struct nctuss_emu_request *request;
	long poll_count = 0;
	static long poll_count_threshhold = 2000000;

	request = (struct nctuss_emu_request *)skb->data;
	
	Emudisk.skb_replied = false;
	Emudisk.current_waiting_reply_id = request->id;	
	request->id_check = request->id;

	printkk(KERN_ALERT "nctuss_emudisk: communicate_with_emulator: sending request->id=%d, type=%d, data1=%d, data2=%d, data3=%d\n",
		request->id, request->type, request->data1, request->data2, request->data3);

	skb_udp_finish(skb);

	resend:
	poll_count = 0;
	//printk(KERN_ALERT "1");
	//nctuss_send_skb(skb);
	
	nctuss_xemacps_send_skb(skb, Emudisk.ndev);
	
	//nctuss_send_udp_emacps(&Emudisk.nn, (char*)out_data, out_data_len);
	//printk(KERN_ALERT "2");

	//start_time_kt = ktime_get();
	while(Emudisk.skb_replied == false) {
		//printk(KERN_ALERT "3");
		nctuss_poll_emacps(Emudisk.ndev);
		//printk(KERN_ALERT "4");
		poll_count++;
		
		// When OS pausing is used, the system time will not advance..
		/* current_time_kt = ktime_get();
		if(current_time_kt.tv64 - start_time_kt.tv64 > 1000000000) { // 1 second response timeout
			printk(KERN_ALERT "nctuss_emudisk: communicate_with_emulator: resending... time=%llu\n", current_time_kt.tv64);
			goto resend;
		}
		*/

		if(poll_count >= poll_count_threshhold) {
			printkk(KERN_ALERT "nctuss_emudisk: communicate_with_emulator: resending... poll_count=%ld\n", poll_count);
			goto resend;
		}
	}
	emulator_return_reply_skb(skb);
	*skb_reply = Emudisk.skb;
	
}

/*
Return the next skb to send
*/
static struct sk_buff * emulator_get_next_skb(void)
{

	int header_len, eth_len, ip_len, udp_len;
	struct udphdr *udph;
	struct iphdr *iph;
//	struct ethhdr *eth;

	struct sk_buff *skb = nctuss_xemacps_start_xmit_get_next_skb_buff(Emudisk.ndev);

	udp_len = sizeof(*udph);
	ip_len = eth_len = udp_len + sizeof(*iph);
	header_len = eth_len + ETH_HLEN + NET_IP_ALIGN;

	skb_reserve(skb, header_len);

	return skb;
}



static void emulator_reset(void)
{
	struct nctuss_emu_request *request;
	struct nctuss_emu_reply *reply;
	struct sk_buff *skb, *reply_skb;

	skb = emulator_get_next_skb();
	skb->len += sizeof(struct nctuss_emu_request);
	request = (struct nctuss_emu_request *)(skb->data);

	request->type = RequestType_reset;
	request->id = Emudisk.udp_send_reply_id++;
	request->data1 = Emudisk.size;
	request->data2 = disk_channels;
	request->data3 = Emudisk.speed_factor;

	printkk(KERN_ALERT "nctuss_emudisk: emulator_reset\n");
	communicate_with_emulator(skb, &reply_skb);

	reply = (struct nctuss_emu_reply *)(reply_skb->data);
	if(reply->type != ReplyType_ok) {
		printk(KERN_ALERT "nctuss_emudisk: emulator_reset failed\n");
	}
	else {
		printkk(KERN_ALERT "nctuss_emudisk: emulator_reset OK\n");
	}
	
	emulator_return_reply_skb(reply_skb);
}

static void emulator_submit_request(u64 time, int rw, u32 sector, u32 count, u32 io_request_id)
{
	struct nctuss_emu_request *request;
	struct nctuss_emu_reply *reply;
	struct sk_buff *skb, *reply_skb;

	skb = emulator_get_next_skb();
	skb->len += sizeof(struct nctuss_emu_request);
	request = (struct nctuss_emu_request *)(skb->data);

	request->type = RequestType_submitReqeust;
	request->id = Emudisk.udp_send_reply_id++;
	request->data1 = (u32)(time >> 32);
	request->data2 = (u32)(time & 0xFFFFFFFF);
	request->data3 = rw;
	request->data4 = sector;
	request->data5 = count;
	request->data6 = io_request_id;

	printkk(KERN_ALERT "\nnctuss_emudisk: emulator_submit_request. time=%llu\n", time);
	communicate_with_emulator(skb, &reply_skb);

	reply = (struct nctuss_emu_reply *)(reply_skb->data);
	if(reply->type != ReplyType_ok) {
		printk(KERN_ALERT "nctuss_emudisk: emulator_submit_request failed\n");
	}
	else {
		printkk(KERN_ALERT "nctuss_emudisk: emulator_submit_request OK\n");
	}

	emulator_return_reply_skb(reply_skb);
}

static u64 current_max_get_responses_time = 0;
static void emulator_get_responses(u64 current_time, int *num_responses, u64 *response_time, u32 *io_request_ids)
{
	int i;
	struct nctuss_emu_request *request;
	struct nctuss_emu_reply *reply;
	struct sk_buff *skb, *reply_skb;

	skb = emulator_get_next_skb();
	skb->len += sizeof(struct nctuss_emu_request);
	request = (struct nctuss_emu_request *)(skb->data);	

	if(current_time >= current_max_get_responses_time) {
		current_max_get_responses_time = current_time;
	}
	else {
		printk(KERN_ALERT "nctuss_emudisk: emulator_get_responses: current_time (%llu) < current_max_get_responses_time(%llu)\n",
			current_time, current_max_get_responses_time);
	}

	request->type = RequestType_getNextResponses;
	request->id = Emudisk.udp_send_reply_id++;
	request->data1 = (u32)(current_time >> 32);
	request->data2 = (u32)(current_time & 0xFFFFFFFF);

	printkk(KERN_ALERT "\nnctuss_emudisk: emulator_get_responses. time=%llu\n", current_time);
	communicate_with_emulator(skb, &reply_skb);

	reply = (struct nctuss_emu_reply *)(reply_skb->data);
	if(reply->type != ReplyType_ok) {
		printk(KERN_ALERT "nctuss_emudisk: emulator_get_responses failed\n");
	}
	else {
		*num_responses = reply->data1;
		*response_time = ((u64)reply->data2 << 32) + reply->data3;
		for(i=0; i<(*num_responses); i++) {
			io_request_ids[i] = reply->io_request_ids[i];
		}
		printkk(KERN_ALERT "nctuss_emudisk: emulator_get_responses OK\n");
	}
	
	emulator_return_reply_skb(reply_skb);
}

#ifdef COMBINE_REQ_DATA
static void emulator_transfer_data(u32 read_write, u32 sector_num, char *data)
{
	/* transfers one sector */

	struct nctuss_emu_request *request;
	struct nctuss_emu_reply *reply;
	struct sk_buff *skb, *reply_skb;

	skb = emulator_get_next_skb();
	request = (struct nctuss_emu_request *)(skb->data);

	request->type = RequestType_readWriteData;
	request->id = Emudisk.udp_send_reply_id++;
	request->data1 = read_write; // read = 0, write = 1
	request->data2 = sector_num;

	if(read_write == 0) {
		/* read */
		skb->len += sizeof(struct nctuss_emu_request);
	}
	else {
		/* write */
		skb->len += (sizeof(struct nctuss_emu_request) + SECTOR_SIZE);
		memcpy(skb->data + sizeof(struct nctuss_emu_request), data, SECTOR_SIZE);
	}

	communicate_with_emulator(skb, &reply_skb);
	reply = (struct nctuss_emu_reply *)(reply_skb->data);
	if(reply->type != ReplyType_ok) {
		printk(KERN_ALERT "nctuss_emudisk: emulator_transfer_data failed.\n");
	}
	else {
		if(read_write == 0) {
			memcpy(data, reply_skb->data + sizeof(struct nctuss_emu_reply), SECTOR_SIZE);
		}
	}
	emulator_return_reply_skb(reply_skb);
}

#else
static void emulator_transfer_data(int read_write, int start_sector, int sector_num, struct sk_buff **skbs_for_read, struct sk_buff **skbs_for_write)
{
	struct nctuss_emu_request *request;
	struct nctuss_emu_reply *reply;	
	struct sk_buff *skb, *reply_skb;
	int i;

	if(read_write == 1) {
		/* write */

		for(i=0; i< sector_num; i++) {
			skb = skbs_for_write[i];
			skb->len += (sizeof(struct nctuss_emu_request) + SECTOR_SIZE);
			
			request = (struct nctuss_emu_request *)(skb->data); 
			
			request->type = RequestType_readWriteData;
			request->id = Emudisk.udp_send_reply_id++;
			request->data1 = 1; // read = 0, write = 1
			request->data2 = start_sector + i;

			communicate_with_emulator(skb, &reply_skb);
			reply = (struct nctuss_emu_reply *)(reply_skb->data);
			if(reply->type != ReplyType_ok) {
				printk(KERN_ALERT "nctuss_emudisk: emulator_transfer_data failed (for write)\n");
			}

			emulator_return_reply_skb(reply_skb);
			skbs_for_write[i] = emulator_get_next_skb();
		}
	}
	else {
		/* read */

		for(i=0; i<sector_num; i++) {
			skb = emulator_get_next_skb();
			skb->len += sizeof(struct nctuss_emu_request);
			
			request = (struct nctuss_emu_request *)(skb->data); 
			
			request->type = RequestType_readWriteData;
			request->id = Emudisk.udp_send_reply_id++;
			request->data1 = 0; // read = 0, write = 1
			request->data2 = start_sector + i;

			communicate_with_emulator(skb, &reply_skb);
			reply = (struct nctuss_emu_reply *)(reply_skb->data);
			if(reply->type != ReplyType_ok) {
				printk(KERN_ALERT "nctuss_emudisk: emulator_transfer_data failed (for read)\n");
			}
			
			skbs_for_read[i] = reply_skb;
		}
	}

}
#endif


//#pragma GCC pop_options


#ifdef USE_FIQ
extern volatile u32 nctuss_OS_paused;
extern volatile u32 nctuss_OS_paused_counter_count;
#else
static volatile u32 nctuss_OS_paused;
static volatile u32 nctuss_OS_paused_counter_count;
#endif


#ifdef USE_FIQ
static struct fiq_handler fiq_handler = {
	.name	= "nctuss_os_pause"
};

#else


/*
This is the function to be run on other CPUs to "pause" the OS.
*/
static void ipi_function(void)
{
	unsigned long flags;
	local_irq_save(flags);
	while(nctuss_OS_paused)
		cpu_relax();
	local_irq_restore(flags);	
}
#endif

#ifdef MEASURE_EMULATOR_KERNEL_OVERHEAD
static long pause_OS_caller;
static long pause_OS_start_time;
#endif

static unsigned long my_flags;
static void pause_OS(int caller)
{
#ifdef MEASURE_EMULATOR_KERNEL_OVERHEAD
	pause_OS_caller = caller;
	pause_OS_start_time = nctuss_gt_get_counter_value();
	writel(0x0, my_ipi_base+4*9); // This is the up counter
#endif


	local_irq_save(my_flags);
	
	if(unlikely(nctuss_OS_paused != 0)) {
		printk(KERN_ALERT "pause_OS: nctuss_OS_paused !=0, %d\n", nctuss_OS_paused);
	}
	
	nctuss_OS_paused = 1;
			
	/* Interrupt the other processor(s) */
#ifdef USE_FIQ
		int cpu;
		int count = 0;

		////unsigned int counter_value;
		////counter_value = nctuss_gt_get_counter_value();
	
		
		nctuss_OS_paused = 1;
		mb();

		cpu = smp_processor_id();


		struct pt_regs fiq_regs;
		
		cpu = smp_processor_id();
		
		get_fiq_regs(&fiq_regs);
		if(fiq_regs.ARM_r8!=cpu) {
			printk(KERN_ALERT "!!!!!!!!!!!!!!!!!! cpu=%d, ARM_r8=%lu\n", cpu, fiq_regs.ARM_r8);
		}


		if(cpu == 0) {
			writel(0x1, my_ipi_base + 8); // LED2
			writel(0x1, my_ipi_base+4);
		}
		else {
			writel(0x1, my_ipi_base + 12); // LED3	
			writel(0x1, my_ipi_base);
		}

		mb();
		writel(0x1, my_ipi_base + 28);

		while(nctuss_OS_paused==1) {
			//cpu_relax();
			if((count & 0xFFFFF) > 0x80000) {
				writel(0x1, my_ipi_base + 24);
			} else {
				writel(0x0, my_ipi_base + 24);
			}
			count++;
			mb();
		}
		
		nctuss_OS_paused = 1;
		mb();

		if(cpu == 0) {
			writel(0x0, my_ipi_base + 8);
		}
		else {
			writel(0x0, my_ipi_base + 12);
		}

		writel(0x0, my_ipi_base + 28);
		writel(0x0, my_ipi_base + 16);
		writel(0x0, my_ipi_base + 20);
		////printk(KERN_ALERT "FIQ took %d counter counts (%u, %u)\n", nctuss_OS_paused_counter_count - counter_value, nctuss_OS_paused_counter_count, counter_value);

		printkk(KERN_ALERT "pause_OS: CPU ID: %d\n", smp_processor_id());

		nctuss_gt_stop();
		nctuss_ttc_stop();
		preempt_disable();	

#else
		spin_lock(&nctuss_gt_spinlock);
		nctuss_gt_stop();
		
		//nctuss_l2x0_lock(); // Cache lock. TODO: Need to verify if it actually works.
		nctuss_ttc_stop();
			
		preempt_disable();		

		nctuss_smp_invoke_function(ipi_function);
#endif

	nctuss_OS_paused_counter_count = nctuss_gt_get_counter_value();


}

static void resume_OS(void)
{

	u32 counter_value = nctuss_gt_get_counter_value();
	if(unlikely(nctuss_OS_paused_counter_count != counter_value)) {
		printk(KERN_ALERT "nctuss_emudisk: resume_OS: nctuss_OS_paused_counter_count(%u) != counter_value(%u)\n",
			nctuss_OS_paused_counter_count, counter_value);
	}
	
	nctuss_OS_paused = 0;

#ifdef USE_FIQ
	mb();
	while(nctuss_OS_paused==0) {
		// wait until FIQ sets nctuss_OS_paused to 2
		mb();
	}
	mb();
	nctuss_OS_paused = 0;
	mb();
#endif
	printkk(KERN_ALERT "pause_OS (resume): CPU ID: %d\n", smp_processor_id());

	nctuss_gt_resume();
	spin_unlock(&nctuss_gt_spinlock);

	nctuss_ttc_resume();
	//nctuss_l2x0_unlock();
	
	preempt_enable_no_resched();	
	local_irq_restore(my_flags);

#ifdef MEASURE_EMULATOR_KERNEL_OVERHEAD
		nctuss_emudisk_stats[nctuss_emudisk_stats_num].from_who = pause_OS_caller;
		//nctuss_emudisk_stats[nctuss_emudisk_stats_num].data1 = nctuss_gt_get_counter_value() - pause_OS_start_time;
		nctuss_emudisk_stats[nctuss_emudisk_stats_num].data1 = readl(my_ipi_base+4*9);
		nctuss_emudisk_stats_num++;
#endif


}

static void nctuss_emudisk_softirq_done(struct request *req)
{
//	printk(KERN_ALERT "!!! nctuss_emudisk_softirq_done - Enter\n");
	unsigned long flags;
	bool enabled = false;
#ifdef DEBUG_FIQ
	writel(0x1, my_ipi_base + 12);
#endif
	blk_end_request_all(req, 0);

	spin_lock_irqsave(&(Emudisk.lock), flags);

#ifdef TRACE_STAT
	nctuss_emudisk_stats[nctuss_emudisk_stats_num].from_who = 3;
	nctuss_emudisk_stats[nctuss_emudisk_stats_num].data1 = 1;
	nctuss_emudisk_stats_num++;
#endif
	Emudisk.request_inflight--;

	if(queue_flag_test_and_clear(QUEUE_FLAG_STOPPED, req->q)) {
		//printk(KERN_ALERT "blk_start_queue\n");	
		enabled = true;
		blk_start_queue(req->q);
	}

	//spin_unlock_bh(&(Emudisk.lock));
	spin_unlock_irqrestore(&(Emudisk.lock),flags);
#ifdef DEBUG_FIQ
	writel(0x0, my_ipi_base + 12);
#endif


	//printk(KERN_ALERT "!!! nctuss_emudisk_softirq_done - Exit\n");
#ifdef TRACE_STAT
	nctuss_emudisk_stats[nctuss_emudisk_stats_num].from_who = 3;
	nctuss_emudisk_stats[nctuss_emudisk_stats_num].data1 = 2;
	nctuss_emudisk_stats_num++;
#endif

}

int nctuss_emudisk_getgeo(struct block_device *block_device, struct hd_geometry *geo) {

	long size;

	/* We have no real geometry, of course, so make something up. */

	nctuss_emudisk_t *dev = block_device->bd_disk->private_data;
	size = dev->size * 1;
//	geo->cylinders = (size & ~0x3f) >> 6;
//	geo->heads = 4;
//	geo->sectors = 16;
	geo->cylinders = size / 63 / 255;
	geo->heads = 255;
	geo->sectors = 63;
	geo->start = 0;
	return 0;
}


static struct block_device_operations nctuss_emudisk_ops = {
	.getgeo = nctuss_emudisk_getgeo
};

#if defined(TRACE_STAT) || defined(MEASURE_EMULATOR_KERNEL_OVERHEAD)
static void *emudisk_seq_start(struct seq_file *s, loff_t *pos)
{
	if(*pos >= nctuss_emudisk_stats_num)
		return NULL; /* No more to read */
	return nctuss_emudisk_stats + *pos;
}

static void *emudisk_seq_next(struct seq_file *s, void *v, loff_t *pos)
{
	(*pos)++;
	if(*pos >= nctuss_emudisk_stats_num)
		return NULL;
	return nctuss_emudisk_stats + *pos;
}

static void emudisk_seq_stop(struct seq_file *s, void *v)
{
}

static int emudisk_seq_show(struct seq_file *s, void *v)
{
	int i;
	
	nctuss_emudisk_stats_t *stat = (nctuss_emudisk_stats_t *) v;

#ifdef MEASURE_EMULATOR_KERNEL_OVERHEAD
	seq_printf(s, "from_who:\t%ld\n", stat->from_who);
	seq_printf(s, "request_id:\t%ld\n", stat->request_id);
	seq_printf(s, "data1:\t%ld\n", stat->data1);
#else
	seq_printf(s, "from_who:\t%ld\n", stat->from_who);
	seq_printf(s, "request_id:\t%ld\n", stat->request_id);
	seq_printf(s, "data1:\t%ld\n", stat->data1);

	for(i=0; i<disk_channels; i++) {
		seq_printf(s, "i=%d request_id=%d\n", i, stat->pending_request_ids[i]);
	}
#endif

	seq_printf(s,"\n");

	return 0;
}

static struct seq_operations emudisk_seq_ops = {
	.start = emudisk_seq_start,
	.next = emudisk_seq_next,
	.stop = emudisk_seq_stop,
	.show = emudisk_seq_show
};

static int emudisk_proc_open(struct inode *inode, struct file *file)
{
	return seq_open(file, &emudisk_seq_ops);
}

static struct file_operations emudisk_proc_ops = {
	.owner = THIS_MODULE,
	.open = emudisk_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = seq_release
};
#endif

/*
* Actual I/O processing.
*/
#ifdef COMBINE_REQ_DATA
static void nctuss_emudisk_transfer(struct request *req, char *data)
{
	unsigned long nbytes = blk_rq_bytes(req);
	unsigned long offset = 0;


	struct req_iterator iter;
	struct bio_vec *bvec;
	unsigned long flags;
	size_t size;
	unsigned char *buf;

	if(unlikely(nbytes > MAX_REQUEST_SECTOR_NUM * SECTOR_SIZE)) {
		printk(KERN_ALERT "nctuss_emuisk: nctuss_emudisk_transfer: nbytes(%lu) > MAX_REQUEST_SECTOR_NUM * SECTOR_SIZE\n", nbytes);
		while(1);
	}

	rq_for_each_segment(bvec, req, iter) {
		size = bvec->bv_len;
		buf = bvec_kmap_irq(bvec, &flags);

		if(rq_data_dir(req) == WRITE) {
			memcpy(data + offset, buf, size);
		}
		else {
			memcpy(buf, data + offset, size);
		}
		offset+=size;
		
		bvec_kunmap_irq(buf, &flags);
	}

	
}
#else
static void nctuss_emudisk_transfer(struct request *req, int req_sectors, struct sk_buff *skbs[MAX_REQUEST_SECTOR_NUM])
{
	//unsigned long offset = blk_rq_pos(req) * SECTOR_SIZE;
	unsigned long nbytes = blk_rq_bytes(req);

	struct req_iterator iter;
	struct bio_vec *bvec;
	unsigned long flags;
	size_t size;
	unsigned char *buf, *buf_ori;
	struct sk_buff *skb;
	int bytes_remaining_in_skb_data, skb_data_offset;
	int current_skb = 0;

	if(unlikely(nbytes > MAX_REQUEST_SECTOR_NUM * SECTOR_SIZE)) {
		printk(KERN_ALERT "nctuss_emudisk: nctuss_emudisk_transfer: nbytes(%ld) > MAX_REQUEST_SECTOR_NUM * SECTOR_SIZE", nbytes);
		while(1);
	}
	if(unlikely(req_sectors > MAX_REQUEST_SECTOR_NUM)) {
		printk(KERN_ALERT "nctuss_emudisk: nctuss_emudisk_transfer: req_sectors(%d) > MAX_REQUEST_SECTOR_NUM\n", req_sectors);
		while(1);
	}

	bytes_remaining_in_skb_data = 0;

	rq_for_each_segment(bvec, req, iter) {
		size = bvec->bv_len;
		buf = bvec_kmap_irq(bvec, &flags);
		buf_ori = buf;

		while(size>0) {

			if(bytes_remaining_in_skb_data==0) {
				skb = skbs[current_skb++];
				if(unlikely(skb->nctuss_data == 0)) {
					printk(KERN_ALERT "nctuss_emudisk: nctuss_emudisk_transfer: skb->nctuss_data == 0\n");
					//while(1);
				}
				bytes_remaining_in_skb_data = SECTOR_SIZE;
				if(rq_data_dir(req) == WRITE) {
					skb_data_offset = sizeof(struct nctuss_emu_request);
					skb->len += sizeof(struct nctuss_emu_request) + SECTOR_SIZE;
				}
				else {
					skb_data_offset = sizeof(struct nctuss_emu_reply);
				}
			}

			if(size<=bytes_remaining_in_skb_data) {
				if(rq_data_dir(req) == WRITE) {
					memcpy(skb->data + skb_data_offset, buf, size);
				}
				else {
					memcpy(buf, skb->data + skb_data_offset, size);
				}				

				buf+=size;
				skb_data_offset+=size;
				bytes_remaining_in_skb_data-=size;
				size=0;
			}
			else {
				if(rq_data_dir(req) == WRITE) {
					memcpy(skb->data + skb_data_offset, buf, bytes_remaining_in_skb_data);
				}
				else {
					memcpy(buf, skb->data + skb_data_offset, bytes_remaining_in_skb_data);					
				}

				buf+=bytes_remaining_in_skb_data;
				skb_data_offset+=bytes_remaining_in_skb_data;
				size-=bytes_remaining_in_skb_data;
				bytes_remaining_in_skb_data=0;
			}
		}

		bvec_kunmap_irq(buf_ori, &flags);
	}
	
}
#endif

//#define USE_IRQ_SAVE 1

static int nctuss_emudisk_processing_task_func(void *dummy)
{
	struct request *req[MAX_DISK_REQUESTS_IN_PARALLEL];
	ktime_t current_time_kt, sleep_until_time_kt;
	int ret;
	int i, j;
	u64 next_response_time;
	unsigned int next_response_num;
	struct sk_buff **skbs_for_read[MAX_DISK_REQUESTS_IN_PARALLEL], **skbs_for_write;
	int req_sectors[MAX_DISK_REQUESTS_IN_PARALLEL];
	char *data;

#ifdef USE_IRQ_SAVE
	unsigned long flags;
#endif

#ifdef USE_IRQ_SAVE
	spin_lock_irqsave(&(Emudisk.lock), flags);
#else
	spin_lock_bh(&(Emudisk.lock));
#endif


	//printk(KERN_ALERT "!!!!!nctuss_emudisk_processing_task_func, emulator_reset()\n");
	//emulator_reset();
	//emulator_get_responses(current_time_kt.tv64, &next_response_num, &next_response_time, Emudisk.next_response_to_reply_ids);
	
	//Emudisk.next_response_to_reply_time_kt.tv64 = ULLONG_MAX;
	//Emudisk.next_response_to_reply_num = 0;

	while(1) {
		printkk(KERN_ALERT "nctuss_emudisk_processing_task_func - 1\n");

		if(Emudisk.next_response_to_reply_num == 0) {
			set_current_state(TASK_INTERRUPTIBLE);
			////wakeup_softirqd(); // From softirq.c
#ifdef DEBUG_FIQ
			writel(0x1, my_ipi_base + 16);
#endif

#ifdef USE_IRQ_SAVE
			spin_unlock_irqrestore(&(Emudisk.lock),flags);
#else
			spin_unlock_bh(&(Emudisk.lock));
#endif
			schedule_hrtimeout(NULL, HRTIMER_MODE_ABS); // sleep until interrupted
			printkk(KERN_ALERT "nctuss_emudisk_processing_task_func - 1.4\n");
#ifdef USE_IRQ_SAVE			
			spin_lock_irqsave(&(Emudisk.lock), flags);
#else
			spin_lock_bh(&(Emudisk.lock));
#endif

#ifdef DEBUG_FIQ
			writel(0x0, my_ipi_base + 16);
#endif

		}

		printkk(KERN_ALERT "nctuss_emudisk_processing_task_func - 1.5\n");
		
		resleep:
		sleep_until_time_kt = Emudisk.next_response_to_reply_time_kt;
		set_current_state(TASK_INTERRUPTIBLE);
		
		////wakeup_softirqd(); // From softirq.c		
#ifdef USE_IRQ_SAVE
		spin_unlock_irqrestore(&(Emudisk.lock),flags);
#else
		spin_unlock_bh(&(Emudisk.lock));
#endif		
		ret = schedule_hrtimeout(&sleep_until_time_kt, HRTIMER_MODE_ABS);

		if(unlikely(Emudisk.stopping == true)) {
			break;
		}

		printkk(KERN_ALERT "nctuss_emudisk_processing_task_func - 2\n");

#ifdef USE_IRQ_SAVE
		spin_lock_irqsave(&(Emudisk.lock), flags);
#else
		spin_lock_bh(&(Emudisk.lock));
#endif

		if(unlikely(ret==-EINTR)) {
			printkk(KERN_ALERT "nctuss_emudisk_processing_task_func - ret==-EINTR\n");
			goto resleep;
		}

		process_next_without_sleep:

		current_time_kt = ktime_get();

		if(unlikely(Emudisk.latest_response_time > current_time_kt.tv64)) {
			printk(KERN_ALERT "nctuss_ramdisk: nctuss_emudisk_processing_task_func: Emudisk.latest_response_time > current_time_kt.tv64\n");
			while(1);
		}
		if(unlikely(Emudisk.latest_response_time > Emudisk.next_response_to_reply_time_kt.tv64)) {
			printk(KERN_ALERT "nctuss_ramdisk: nctuss_emudisk_processing_task_func: Emudisk.latest_response_time > Emudisk.next_response_to_reply_time_kt.tv64\n");
			while(1);
		}

		/* Process completed requests */
		if(unlikely(Emudisk.next_response_to_reply_num == 0)) {
			printk("!!!! next_response_to_reply_num == 0\n");
			while(1);
		}

		for(i=0; i<Emudisk.next_response_to_reply_num; i++) {

			nctuss_get_request_id_to_req_mapping(Emudisk.next_response_to_reply_ids[i], &(req[i]), &(req_sectors[i]), &(skbs_for_read[i]), &skbs_for_write, &data);
			Emudisk.latest_response_time = Emudisk.next_response_to_reply_time_kt.tv64;
			//spin_unlock_bh(&(Emudisk.lock));
			//spin_unlock_irqrestore(&(Emudisk.lock),flags);
			//printkk(KERN_ALERT "nctuss_emudisk_processing_task_func - 3\n");
			
			/* Data transfer */
			if(rq_data_dir(req[i]) == READ) {
#ifdef COMBINE_REQ_DATA
				nctuss_emudisk_transfer(req[i], data);
#else
				nctuss_emudisk_transfer(req[i], req_sectors[i], skbs_for_read[i]);
#endif
			}

			//spin_lock_bh(&(Emudisk.lock));
			//spin_lock_irqsave(&(Emudisk.lock), flags);
			printkk(KERN_ALERT "nctuss_emudisk_processing_task_func - 4\n");

#ifdef TRACE_STAT
			nctuss_emudisk_stats[nctuss_emudisk_stats_num].from_who = 2;
			nctuss_emudisk_stats[nctuss_emudisk_stats_num].request_id = Emudisk.next_response_to_reply_ids[i];		
			nctuss_emudisk_stats_num++;
#endif

			blk_complete_request(req[i]);

		}

		/* Find next request to complete */
		pause_OS(PAUSE_OS_CALLER_PROCESSING_TASK_GET_NEXT_RESPONSE);

		/* Return skbs to driver */
#ifndef COMBINE_REQ_DATA
		for(i=0; i<Emudisk.next_response_to_reply_num; i++) {
			if(rq_data_dir(req[i]) == READ) {
				for(j=0; j<req_sectors[i]; j++) {
					if((skbs_for_read[i][j])==0 || (skbs_for_read[i][j])->nctuss_data==0) {
						printk(KERN_ALERT "!! i=%d, j=%d\n", i, j);
						printk(KERN_ALERT "!! i=%d, j=%d\n", Emudisk.next_response_to_reply_num, req_sectors[i]);					
					}
					emulator_return_reply_skb((skbs_for_read[i][j]));
				}
			}
		}
#endif

		printkk(KERN_ALERT "nctuss_emudisk_processing_task_func - 5\n"); 

		printkk(KERN_ALERT "nctuss_emudisk_processing_task_func:\n");
		if(Emudisk.latest_response_time < current_max_get_responses_time) {
			printk(KERN_ALERT "!!!! - 1\n");
		}
		emulator_get_responses(Emudisk.latest_response_time, &next_response_num, &next_response_time, Emudisk.next_response_to_reply_ids);
		printkk(KERN_ALERT "nctuss_emudisk_processing_task_func - 6\n");
		if(next_response_num!=0) {
			if(unlikely(next_response_time < Emudisk.latest_response_time)) {
				printk(KERN_ALERT "nctuss_emudisk: nctuss_emudisk_processing_task_func: next_response_time (%llu) < Emudisk.latest_response_time (%llu)\n",
					next_response_time, Emudisk.latest_response_time);
				while(1);
			}
			Emudisk.next_response_to_reply_num = next_response_num;
			Emudisk.next_response_to_reply_time_kt.tv64 = next_response_time;
			current_time_kt = ktime_get();

			// TODO: test the effect of process_next_without_sleep.
			
			if(Emudisk.next_response_to_reply_time_kt.tv64 < current_time_kt.tv64) {
				printkk(KERN_ALERT "nctuss_emudisk_processing_task_func - 7\n");
				resume_OS();
				goto process_next_without_sleep;
			}
			
			
		}
		else {
			Emudisk.next_response_to_reply_num = 0;
		}
		printkk(KERN_ALERT "nctuss_emudisk_processing_task_func - 8\n");
		resume_OS();
	}
	
	return 0;
}

static void nctuss_emudisk_strategy(struct request_queue *q)
{
	// NOTE: Emudisk.lock (which should be q->queue_lock?) is accquired by kernel when entering this function.
	
	struct request *req;
	ktime_t current_time_kt;
	int i;
	bool new_request_submitted = false, need_to_wake_up_reply_thread = false;
	unsigned int sector_count;

	u64 next_response_time;
	unsigned int next_response_num;
	u32 next_response_ids[MAX_DISK_REQUESTS_IN_PARALLEL];

	unsigned long req_offset;
	unsigned long req_nbytes;
	char *data;

	struct sk_buff **skbs_for_read, **skbs_for_write;

/*
	struct req_iterator iter;
	struct bio_vec *bvec;
	unsigned long flags;
	size_t size;
	unsigned char *buf;
	long offset;	
*/

	// TODO: test the effect of putting the current time into the while loop (so that each request will have different start time(Same for ramdisk).)
#ifndef DIFFERENT_SUBMIT_TIME
	current_time_kt = ktime_get();
#endif

	printkk(KERN_ALERT "nctuss_emudisk: Entering nctuss_emudisk_strategy\n");
#ifdef DEBUG_FIQ
	writel(0x1, my_ipi_base + 24);
#endif
	

	while(true) {

#ifdef DIFFERENT_SUBMIT_TIME
		current_time_kt = ktime_get();
#endif


#ifdef TRACE_STAT
		nctuss_emudisk_stats[nctuss_emudisk_stats_num].from_who = 1;
		nctuss_emudisk_stats[nctuss_emudisk_stats_num].request_id = Emudisk.udp_send_reply_id;
		nctuss_emudisk_stats[nctuss_emudisk_stats_num].data1 = 1;
		nctuss_emudisk_stats_num++;
#endif


		req = blk_fetch_request(q);
		if(req==NULL) {
			break;
		}
#ifdef TRACE_STAT
		nctuss_emudisk_stats[nctuss_emudisk_stats_num].from_who = 1;
		nctuss_emudisk_stats[nctuss_emudisk_stats_num].request_id = Emudisk.udp_send_reply_id;
		nctuss_emudisk_stats[nctuss_emudisk_stats_num].data1 = 2;
#endif
		if(unlikely(req->cmd_type != REQ_TYPE_FS)) {
			printk (KERN_NOTICE "nctuss_ramdisk:nctuss_emudisk_strategy: Skip non-CMD request\n");
			blk_end_request_all(req, -EIO);
			continue;
		}


		
		/* Submit request to emulator */
		if(unlikely(current_time_kt.tv64 <= Emudisk.latest_response_time )) {
			printk(KERN_ALERT "nctuss_emudisk: nctuss_emudisk_strategy: current_time_kt (%llu) <= latest_response_time (%lld)\n",
				current_time_kt.tv64, Emudisk.latest_response_time);
			while(1);
		}

		req_offset = blk_rq_pos(req);
		req_nbytes = blk_rq_bytes(req);
		sector_count = (blk_rq_bytes(req)+SECTOR_SIZE-1)/SECTOR_SIZE;

		nctuss_put_request_id_to_req_mapping(Emudisk.udp_send_reply_id, req, sector_count, &skbs_for_read, &skbs_for_write, &data);

		/* Transfer data to/from emulator */
		
		if(rq_data_dir(req) == WRITE) {
			printkk(KERN_ALERT "nctuss_emudisk: nctuss_emudisk_strategy: before nctuss_emudisk_transfer\n");
#ifdef COMBINE_REQ_DATA
			nctuss_emudisk_transfer(req, data);
#else
			nctuss_emudisk_transfer(req, sector_count, skbs_for_write);
#endif
			printkk(KERN_ALERT "nctuss_emudisk: nctuss_emudisk_strategy: after nctuss_emudisk_transfer\n");
		}
/*
		offset = 0;
		rq_for_each_segment(bvec, req, iter) {
			size = bvec->bv_len;
			buf = bvec_kmap_irq(bvec, &flags);

			if(rq_data_dir(req) == WRITE) {
				memcpy(data + offset, buf, size);
			}
			offset+=size;
			bvec_kunmap_irq(buf, &flags);
		}
*/

		/* Pause OS */

		pause_OS(PAUSE_OS_CALLER_STRAGETY_TRANSFER_DATA);
		emulator_submit_request(current_time_kt.tv64, rq_data_dir(req)==READ?0:1, blk_rq_pos(req), sector_count, Emudisk.udp_send_reply_id);
		new_request_submitted = true;

		//printk(KERN_ALERT "nctuss_emudisk_strategy: before global clock value: %u\n", nctuss_gt_get_counter_value());
#ifdef COMBINE_REQ_DATA
		for(i=0; i<req_nbytes; i+=SECTOR_SIZE) {
			emulator_transfer_data(rq_data_dir(req)==WRITE ? 1:0, (req_offset+i/SECTOR_SIZE), data+i);
		}
#else
		emulator_transfer_data(rq_data_dir(req)==WRITE ? 1:0, req_offset, sector_count, skbs_for_read, skbs_for_write);
#endif

		//printk(KERN_ALERT "nctuss_emudisk_strategy: after global clock value: %u\n", nctuss_gt_get_counter_value());

		resume_OS();
		
		Emudisk.request_inflight++;


		if(Emudisk.request_inflight >= disk_channels) {
			//printk(KERN_ALERT "blk_stop_queue\n");
			blk_stop_queue(q);
			break;
		}

		
		req = blk_peek_request(q);
		if(req==NULL) {
			break;
		}


	}

#ifdef DEBUG_FIQ	
	writel(0x1, my_ipi_base + 28);
#endif


	
	if(likely(new_request_submitted /*&& current_time_kt.tv64 < Emudisk.next_response_to_reply_time_kt.tv64*/)) {
		/* Pause OS */
		pause_OS(PAUSE_OS_CALLER_STRAGETY_GET_NEXT_RESPONSE); 

		/* Get next response */
		// Only need to consider for responses after current_time. These are the possible responses that will be earlier than the current pending response.
		if(unlikely(current_time_kt.tv64 < current_max_get_responses_time)) {
			printk(KERN_ALERT "nctuss_emudisk: nctuss_emudisk_strategy:  current_time_kt.tv64 < current_max_get_responses_time\n");
		}
		//emulator_get_responses(current_time_kt.tv64, &next_response_num, &next_response_time, next_response_ids);
		emulator_get_responses(Emudisk.latest_response_time, &next_response_num, &next_response_time, next_response_ids);
		if(unlikely(next_response_num == 0)) {
			printk(KERN_ALERT "nctuss_emudisk: nctuss_emudisk_strategy: next_response_num==0\n");
		}
		if(next_response_num > 0 && 
			(Emudisk.next_response_to_reply_num == 0
			|| Emudisk.next_response_to_reply_time_kt.tv64 > next_response_time
			|| (Emudisk.next_response_to_reply_time_kt.tv64 == next_response_time && Emudisk.next_response_to_reply_num < next_response_num))) {

			if(Emudisk.next_response_to_reply_num == 0 || Emudisk.next_response_to_reply_time_kt.tv64 > next_response_time) {
				need_to_wake_up_reply_thread = true;
			}

			Emudisk.next_response_to_reply_time_kt.tv64 = next_response_time;
			Emudisk.next_response_to_reply_num = next_response_num;

			for(i=0; i<next_response_num; i++) {
				Emudisk.next_response_to_reply_ids[i] = next_response_ids[i];
			}			
		}

		/* Resume OS */
		resume_OS();	

	}


#ifdef DEBUG_FIQ
	writel(0x0, my_ipi_base + 28);	
#endif

	
	/* Wake up processing process if necessarry */
	//printk(KERN_ALERT "nctuss_emudisk_strategy: need_to_wake_up_reply_thread=%d\n", need_to_wake_up_reply_thread); 
	if(need_to_wake_up_reply_thread == true) {
		//printk(KERN_ALERT "need_to_wake_up_reply_thread\n");
		if(unlikely(Emudisk.processing_task->state != TASK_INTERRUPTIBLE)) {
			printkk(KERN_ALERT "nctuss_emudisk: Emudisk.processing_task not sleeping(1)...state=%ld\n", Emudisk.processing_task->state);
		}
		if(unlikely(wake_up_process(Emudisk.processing_task)==0)) {
			printkk(KERN_ALERT "nctuss_emudisk: Emudisk.processing_task not sleeping(2)...state=%ld\n", Emudisk.processing_task->state);
		}
	}


	printkk(KERN_ALERT "nctuss_emudisk: Exiting nctuss_emudisk_strategy\n");

#ifdef DEBUG_FIQ
	writel(0x0, my_ipi_base + 24);
#endif

	
}

static void setup_nctuss_emudisk_processing_task(void)
{
	int ret;
	static const struct sched_param param = {
		.sched_priority = MAX_USER_RT_PRIO/2,
		//.sched_priority = 0, // 0 is the highest priority
	};

	Emudisk.processing_task = kthread_create(nctuss_emudisk_processing_task_func, NULL, "nctuss_emudisk_processing");
	if(IS_ERR(Emudisk.processing_task)) {
		printk(KERN_ALERT "nctuss_emuisk: processing task creation failed.\n");
		while(1) {};
	}

	
	ret = sched_setscheduler(Emudisk.processing_task, SCHED_FIFO, &param);
	if(ret < 0) {
		printk(KERN_ALERT "nctuss_emudisk: processing task sched_setscheduler failed.\n");
		while(1) {};		
	}

	wake_up_process(Emudisk.processing_task);

	while(Emudisk.processing_task->state != TASK_INTERRUPTIBLE) {
		cpu_relax();
	}

}

#ifdef USE_FIQ
static void set_fiq_registers(void *info)
{
	struct pt_regs fiq_regs;
	int cpu; 
	void *gt_counter_base = nctuss_gt_get_gt_counter_base();

	cpu = smp_processor_id();

	// TODO: need atomic access on my_ipi.. or change to use one register per cpu.
	//fiq_regs.ARM_r8 = (1 << cpu); // bit mask for ... !!

	fiq_regs.ARM_r8 = cpu;
	fiq_regs.ARM_r9 = (unsigned long)my_ipi_base;
	fiq_regs.ARM_r10 = (unsigned long)&nctuss_OS_paused;
	fiq_regs.ARM_fp = (unsigned long)gt_counter_base; // r11
	fiq_regs.ARM_ip = (unsigned long)&nctuss_OS_paused_counter_count; // r12

	set_fiq_regs(&fiq_regs);

	printk(KERN_ALERT "set_fiq_registers on CPU(%d) ok.\n", cpu);
}
#endif
static int __init emudisk_init(void)
{
	int dev_major;
	unsigned char *data;
	int i, j;
#ifdef USE_FIQ
	extern unsigned char nctuss_fiq_start, nctuss_fiq_end;
	unsigned int fiq_handler_length;
#endif

#if defined(TRACE_STAT) || defined(MEASURE_EMULATOR_KERNEL_OVERHEAD)
	struct proc_dir_entry *proc_entry;

	nctuss_emudisk_stats_num = 0;
#endif

	if(disk_channels > MAX_DISK_REQUESTS_IN_PARALLEL) {
		printk(KERN_ALERT "nctuss_emudisk: disk_channels > %d\n", MAX_DISK_REQUESTS_IN_PARALLEL);
		goto err_channel_num_not_supported;
	}

	// Use this the control the LEDs
	my_ipi_base = nctuss_get_my_ipi_base();
	if(!my_ipi_base) {
		printk(KERN_ALERT "nctuss_emudisk: ioremap for my_ipi_base at 0x6A000000 failed\n");
		return -1;
	}

#ifdef USE_FIQ
	enable_fiq(0);

	if (claim_fiq(&fiq_handler)) {
		printk(KERN_ALERT "nctuss_emudisk: couldn't claim FIQ.\n");
		goto err_no_fiq;
	}
	
	fiq_handler_length = &nctuss_fiq_end - &nctuss_fiq_start;
	set_fiq_handler((void *)&nctuss_fiq_start, fiq_handler_length);

	preempt_disable();
	set_fiq_registers(NULL); // set fiq register on self
	smp_call_function(set_fiq_registers, NULL, 1); // set fiq registers on all other CPUs.
	preempt_enable_no_resched();
#endif

#if 0 // TEST FIQ
	int cpu, flag=0;
	unsigned long flags;


for(j=0; j<3000; j++) {
	preempt_disable();
	local_irq_save(flags);

	cpu = smp_processor_id();

	struct pt_regs fiq_regs;

	cpu = smp_processor_id();

	get_fiq_regs(&fiq_regs);
	if(fiq_regs.ARM_r8!=cpu) {
		printk(KERN_ALERT "!!!!!!!!!!!!!!!!!! cpu=%d, ARM_r8=%lu\n", cpu, fiq_regs.ARM_r8);
	}
	//printk(KERN_ALERT "Firing FIQ, (I am CPU%d)\n", cpu);

	if(cpu == 0) {
		writel(0x1, my_ipi_base+4);
	}
	else {
		writel(0x1, my_ipi_base);
	}
	smp_mb();
	dsb_sev();
	

	for(i=0; i<10000; i++) {
		//printk(KERN_ALERT "Delay...%d\n", i);
		udelay(1);
	}
	if(flag==0) {
		writel(0x1, my_ipi_base+8);
		writel(0x0, my_ipi_base+12);
	}
	else {
		writel(0x0, my_ipi_base+8);
		writel(0x1, my_ipi_base+12);
	}
	flag = flag==0?1:0;

	if(cpu == 0) {
		writel(0x0, my_ipi_base+4);
		if(readl(my_ipi_base+4)!=0) {
			printk(KERN_ALERT "!!!!!! 1\n");
		}
	}
	else {
		writel(0x0, my_ipi_base);
		if(readl(my_ipi_base)!=0) {
			printk(KERN_ALERT "!!!!!! 2\n");
		}
	}
	smp_mb();
	dsb_sev();
	
	//printk(KERN_ALERT "FIQ should be completed by now (I am CPU%d)\n", cpu);

	local_irq_restore(flags);
	preempt_enable_no_resched();	
}

	printk(KERN_ALERT "OK!\n", cpu);

	return 0;

#endif


	nctuss_OS_paused = false;

	memset(&Emudisk, 0, sizeof(nctuss_emudisk_t));

	
	Emudisk.speed_factor = speed_factor;

	Emudisk.ndev = dev_get_by_name(&init_net, "eth0");
	if(!Emudisk.ndev) {
		printk(KERN_ALERT "nctuss_emudisk: No ethernet device eth0\n");
		return -ENODEV;
	}
	Emudisk.nn.dev = Emudisk.ndev;
#if 0
	Emudisk.nn.local_port = 5135;
	Emudisk.nn.local_ip = in_aton("10.48.101.70");
	Emudisk.nn.remote_port = 5134;
	Emudisk.nn.remote_ip = in_aton("10.48.101.63");
	Emudisk.nn.remote_mac[0] = 0x08;
	Emudisk.nn.remote_mac[1] = 0x00;
	Emudisk.nn.remote_mac[2] = 0x27;
	Emudisk.nn.remote_mac[3] = 0x46;
	Emudisk.nn.remote_mac[4] = 0x88;
	Emudisk.nn.remote_mac[5] = 0x3d;
	Emudisk.nn.local_mac[0] = 0x00;
	Emudisk.nn.local_mac[1] = 0x0a;
	Emudisk.nn.local_mac[2] = 0x35;
	Emudisk.nn.local_mac[3] = 0x00;
	Emudisk.nn.local_mac[4] = 0x01;
	Emudisk.nn.local_mac[5] = 0x22;
	Emudisk.udp_send_reply_id = 123;
#else
	Emudisk.nn.local_port = 5135;
	switch(server_port_offset) {
		case 0:
			Emudisk.nn.local_ip = in_aton("10.48.101.20");
		break;
		case 1:
			Emudisk.nn.local_ip = in_aton("10.48.101.21");
		break;
		case 2:
			Emudisk.nn.local_ip = in_aton("10.48.101.22");
		break;
		case 3:
			Emudisk.nn.local_ip = in_aton("10.48.101.23");
		break;
		case 4:
			Emudisk.nn.local_ip = in_aton("10.48.101.24");
		break;
		case 5:
			Emudisk.nn.local_ip = in_aton("10.48.101.25");
		break;
		case 6:
			Emudisk.nn.local_ip = in_aton("10.48.101.26");
		break;
		case 7:
			Emudisk.nn.local_ip = in_aton("10.48.101.27");
		break;
		case 8:
			Emudisk.nn.local_ip = in_aton("10.48.101.28");
		break;
		case 9:
			Emudisk.nn.local_ip = in_aton("10.48.101.29");
		break;
		case 10:
			Emudisk.nn.local_ip = in_aton("10.48.101.30");
		break;
		case 11:
			Emudisk.nn.local_ip = in_aton("10.48.101.31");
		break;
		case 12:
			Emudisk.nn.local_ip = in_aton("10.48.101.32");
		break;

	}

	Emudisk.nn.remote_port = 5134 + server_port_offset;
	Emudisk.nn.remote_ip = in_aton("10.48.101.68");
	Emudisk.nn.remote_mac[0] = 0x3c;
	Emudisk.nn.remote_mac[1] = 0x97;
	Emudisk.nn.remote_mac[2] = 0x0e;
	Emudisk.nn.remote_mac[3] = 0x6b;
	Emudisk.nn.remote_mac[4] = 0xd6;
	Emudisk.nn.remote_mac[5] = 0x01;
	Emudisk.nn.local_mac[0] = 0x00;
	Emudisk.nn.local_mac[1] = 0x0a;
	Emudisk.nn.local_mac[2] = 0x35;
	Emudisk.nn.local_mac[3] = 0x00;
	Emudisk.nn.local_mac[4] = 0x01;
	switch(server_port_offset) {
		case 0:
			Emudisk.nn.local_mac[5] = 0x20;
		break;
		case 1:
			Emudisk.nn.local_mac[5] = 0x21;
		break;
		case 2:
			Emudisk.nn.local_mac[5] = 0x22;
		break;
		case 3:
			Emudisk.nn.local_mac[5] = 0x23;
		break;
		case 4:
			Emudisk.nn.local_mac[5] = 0x24;
		break;
		case 5:
			Emudisk.nn.local_mac[5] = 0x25;
		break;
		case 6:
			Emudisk.nn.local_mac[5] = 0x26;
		break;
		case 7:
			Emudisk.nn.local_mac[5] = 0x27;
		break;
		case 8:
			Emudisk.nn.local_mac[5] = 0x28;
		break;
		case 9:
			Emudisk.nn.local_mac[5] = 0x29;
		break;
		case 10:
			Emudisk.nn.local_mac[5] = 0x30;
		break;
		case 11:
			Emudisk.nn.local_mac[5] = 0x31;
		break;
		case 12:
			Emudisk.nn.local_mac[5] = 0x32;
		break;
	}

	Emudisk.udp_send_reply_id = 123;
#endif


	////return 0;
	
	dev_major = register_blkdev(0, "nctuss_emudisk");

	if(dev_major <=0) {
		printk(KERN_WARNING "nctuss_emudisk: unable to get major number");
		return -EBUSY;
	}

	/* Initialize Emudisk */

	Emudisk.size = 0x19000000/SECTOR_SIZE; // 400MB
//	Emudisk.size = 0x10000000/SECTOR_SIZE; // 256MB

	printk(KERN_ALERT "nctuss_emudisk: disk_size(sectors) = %ld\n", Emudisk.size);	

	spin_lock_init(&Emudisk.lock);

	Emudisk.stopping = false;

	/* Create request queue */
	Emudisk.queue = blk_init_queue(nctuss_emudisk_strategy, &(Emudisk.lock));
	if(Emudisk.queue == NULL) {
		printk(KERN_WARNING "nctuss_emudisk: error on blk_init_queue\n");
		goto err_init_queue;
	}
	blk_queue_max_hw_sectors(Emudisk.queue, MAX_REQUEST_SECTOR_NUM);
	blk_queue_max_segments(Emudisk.queue, MAX_REQUEST_SECTOR_NUM);
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, Emudisk.queue); 

	Emudisk.gd = alloc_disk(16);
	if(!Emudisk.gd) {
		printk(KERN_WARNING "nctuss_emudisk: alloc_disk() failed");
		goto err_no_gendisk;
	}

	/* Ioremap */
	// HINT: use memmap=nnG$ssG at in boot parameter to reserver memory.
	//ON T61: 0x300000000\$0x100000000


	data = (unsigned char *)ioremap_nocache(memory_storage_start, memory_storage_size);
	if(data==NULL) {
		goto err_ioremap;
	} else {
		Emudisk.data = data;
	}

	if(open_ethernet_channel(Emudisk.data, (char *)memory_storage_start)) { 
		printk(KERN_ALERT "nctuss_emudisk: Open Ethernet channel failed\n");
		return -EBUSY;
	}
	
	emulator_reset();


	for(i=0; i<disk_channels; i++) {
#ifndef COMBINE_REQ_DATA		
		for(j=0; j<MAX_REQUEST_SECTOR_NUM; j++) {
			request_id_to_req_mapping[i].skbs_for_write[j] = emulator_get_next_skb();
		}
#endif

#ifdef COMBINE_REQ_DATA
		// NCTUSS_SKB_POOL_SIZE = (512 * 8 + XEMACPS_RECV_BD_CNT) . x8 to be safe
		request_id_to_req_mapping[i].data = (char *)(Emudisk.data + (MAX_REQUEST_SECTOR_NUM * SECTOR_SIZE)*i + (512 * 8 + 128) * 2048 * 8);
#endif
	}
	

	Emudisk.gd->queue = Emudisk.queue;
	Emudisk.gd->private_data = &Emudisk;
	Emudisk.major = Emudisk.gd->major = dev_major;
	Emudisk.gd->first_minor = 0;
	set_capacity(Emudisk.gd, Emudisk.size);
	snprintf(Emudisk.gd->disk_name, DISK_NAME_LEN, "emudisk0");
	Emudisk.gd->fops = &nctuss_emudisk_ops;

	blk_queue_softirq_done(Emudisk.queue, nctuss_emudisk_softirq_done);

	setup_nctuss_emudisk_processing_task();

	add_disk(Emudisk.gd);

#if defined(TRACE_STAT) || defined(MEASURE_EMULATOR_KERNEL_OVERHEAD)
	/* seq file */
	proc_entry = proc_create("emudiskseq", 0, NULL, &emudisk_proc_ops); //create_proc_entry("emudiskseq", 0, NULL);
	if(proc_entry == NULL) {
		printk(KERN_ALERT "nctuss_emudisk: proc_create failed\n");
	}
#endif

	return 0;

	err_ioremap:
	err_no_gendisk:
		blk_cleanup_queue(Emudisk.queue);
	err_init_queue:
		unregister_blkdev(dev_major, "emudisk0");
		
		return -ENOMEM;
#ifdef USE_FIQ
	err_no_fiq:
		return -ENXIO;
#endif
	err_channel_num_not_supported:
		return -EINVAL;

}

static void __exit emudisk_exit(void)
{

	del_gendisk(Emudisk.gd);
	put_disk(Emudisk.gd);

	blk_cleanup_queue(Emudisk.queue);
	unregister_blkdev(Emudisk.major, "nctuss_emudisk");

	Emudisk.stopping = true;
	kthread_stop(Emudisk.processing_task);

	iounmap(Emudisk.data);

	close_ethernet_channel();

#ifdef USE_FIQ
	disable_fiq(0);
	release_fiq(&fiq_handler);
#endif
	
    printk(KERN_ALERT "NCTUSS EmuDisk module unload\n");
}


module_init(emudisk_init);
module_exit(emudisk_exit);
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ming-Ju Wu");
MODULE_DESCRIPTION("NCTU Storage Simulator - Emu Disk Mod");
