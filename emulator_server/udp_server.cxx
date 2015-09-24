/***  UDP Server(udpserver.c)   
 *
 * 利用 socket 介面設計網路應用程式
 * 程式啟動後等待 client 端連線，連線後印出對方之 IP 位址
 * 並顯示對方所傳遞之訊息，並回送給 Client 端。
 *
 */
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/errno.h>
#include <arpa/inet.h>
#include <climits>
#include <unistd.h>
#include <signal.h>


#include <db_cxx.h>


//#define printf(fmt, ...) (0)

#define USE_MEMORY_STORAGE
#define MEMORY_STORAGE_SIZE 400 * 1024 * 1024 /* 400 MB */

#define SERV_PORT 5134

#define MAXNAME 1024

#define SECTOR_SIZE 512

static int request_size_kb[8] = {512, 1024, 2048, 4096, 8192, 16384, 32768, 65536};
static int iorate_table_random[2][8] = {{194672, 189513, 186798, 176876, 159182, 141700, 104546, 66149}, {898, 899, 898, 862, 778, 795, 450, 370}}; // unit: 1/100 iops
//static int iorate_table_random[2][8] = {{19467, 19467, 19467, 19467, 19467, 19467, 19467, 19467}, {19467, 19467, 19467, 19467, 19467, 19467, 19467, 19467}}; // unit: 1/100 iops

static int response_time_table_random[2][8];

static int iorate_table_seq[2][8] = {{522101, 488278, 429834, 371532, 298845, 281019, 146861, 71935}, {108649, 104858, 101045, 87071, 73645, 289488, 143416, 72945}}; // unit: 1/100 iops
//static int iorate_table_seq[2][8] = {{19467, 19467, 19467, 19467, 19467, 19467, 19467, 19467}, {19467, 19467, 19467, 19467, 19467, 19467, 19467, 19467}}; // unit: 1/100 iops

static int response_time_table_seq[2][8];

static long total_random_request_count = 0;
static long total_sequential_request_count = 0;
static long total_rollback_request_count = 0;


#define read_latency_ 1000000
#define write_latency_ 2000000

extern int errno;

using namespace std;

#ifdef USE_MEMORY_STORAGE
char memory_storage[MEMORY_STORAGE_SIZE]; // 400MB
#endif


long current_max_request_id;

void printf_cond( const char * format, ... ) {

	return;
	
	if(current_max_request_id > 500) {
		if(current_max_request_id % 5000 !=0) {
			return;
		}
	}
	
	va_list args;
	va_start(args, format);
	vfprintf(stdout, format, args);
	va_end(args);
}

struct ssd_channel
{
	long io_request_id;
	long completion_time;

	long request_last_sector;
	long request_rw;
};

static const int MAX_EMULATOR_CHANNELS = 32;

class MySSDSim
{

public:
	
	
	long size_; // disk size in sectors
	long channels_; // the number of concurrent requests that can be handled.
	long speed_factor_;
	
	long current_time_; // the current simulation time. in nano seconds.

	long most_frontier_simulation_time; // used for calculating rollback counts.
	
	struct ssd_channel ssd_channels[MAX_EMULATOR_CHANNELS];
	
	MySSDSim(void)
	{
	}

	void setup(long size, long channels, long speed_factor)
	{
		int i;

		if(channels > MAX_EMULATOR_CHANNELS) {
			perror("channels > MAX_EMULATOR_CHANNELS!\n");
			return;
		}

#ifdef USE_MEMORY_STORAGE
		if(size > MEMORY_STORAGE_SIZE) {
			printf("MySSDSim:setup() size(%ld) > MEMORY_STORAGE_SIZE\n", size);
			while(1);
		}
#endif
		size_ = size;
		channels_ = channels;
		current_time_ = 0;
		speed_factor_ = speed_factor;

		most_frontier_simulation_time = 0;
		
		for(i=0; i<channels_; i++) {
			ssd_channels[i].io_request_id = -1; // -1 means channel not in use.
			ssd_channels[i].completion_time = 0;
		}
	}

	long submit_request(long time, long rw, long sector, long count, long io_request_id)
	{
		int i;
		long oldest_completion_time;
		long oldest_completion_time_pos;

		bool is_sequential_request;
		int request_size_index;
		int nbytes;


/*
		if(time >= current_time_) {
			current_time_ = time;
		}
		else {
			printf("submit_request: time (%ld) < current_time_ (%ld)\n", time, current_time_);
		}
*/
		oldest_completion_time = LONG_MAX;
		for(i=0; i<channels_; i++) {
			if(ssd_channels[i].completion_time < oldest_completion_time) {
				oldest_completion_time = ssd_channels[i].completion_time;
				oldest_completion_time_pos = i;
			}
		}

		if(oldest_completion_time >= time) {
			return -1;
			printf("submit_request cannot find slot. time=%ld\n", time);
			for(i=0; i<channels_; i++) {
				printf("ssd_channels[i].completion_time = %ld, io_request_id = %ld\n", ssd_channels[i].completion_time, ssd_channels[i].io_request_id);
			}
		}


		/* Calculate response time */

		// Check if request is sequential read/write
		is_sequential_request = false;
		for(i=0; i<channels_; i++) {
			if(ssd_channels[i].request_rw == rw && ssd_channels[i].request_last_sector == sector) {
				is_sequential_request = true;
			}
		}

		nbytes = count * SECTOR_SIZE;
		request_size_index = -1;
		for(i=0; i<8; i++) {
			if(nbytes <= request_size_kb[i]) {
				request_size_index = i;
				break;
			}
		}
		if(request_size_index == -1) {
			printf("request_size_index == -1, nbytes=%d\n", nbytes);
			while(1);
		}

		
		ssd_channels[oldest_completion_time_pos].io_request_id = io_request_id;
		if(is_sequential_request == true) {
			total_sequential_request_count++;
				
			if(rw == 1) { // WRITE
				ssd_channels[oldest_completion_time_pos].completion_time = time + response_time_table_seq[1][request_size_index] / 1000 * speed_factor_;
				ssd_channels[oldest_completion_time_pos].request_rw = 1;
				ssd_channels[oldest_completion_time_pos].request_last_sector = sector + count;
			}
			else {
				ssd_channels[oldest_completion_time_pos].completion_time = time + response_time_table_seq[0][request_size_index] / 1000 * speed_factor_;
				ssd_channels[oldest_completion_time_pos].request_rw = 0;
				ssd_channels[oldest_completion_time_pos].request_last_sector = sector + count;
			}
		}
		else {
			total_random_request_count++;
			
			if(rw == 1) { // WRITE
				ssd_channels[oldest_completion_time_pos].completion_time = time + response_time_table_random[1][request_size_index] / 1000 * speed_factor_;
				ssd_channels[oldest_completion_time_pos].request_rw = 1;
				ssd_channels[oldest_completion_time_pos].request_last_sector = sector + count;
			}
			else {
				ssd_channels[oldest_completion_time_pos].completion_time = time + response_time_table_random[0][request_size_index] / 1000 * speed_factor_;
				ssd_channels[oldest_completion_time_pos].request_rw = 0;
				ssd_channels[oldest_completion_time_pos].request_last_sector = sector + count;
			}
		}


		// compute rollback count
		if(time < most_frontier_simulation_time) {
			total_rollback_request_count++;
		}
		if(ssd_channels[oldest_completion_time_pos].completion_time > most_frontier_simulation_time) {
			most_frontier_simulation_time = ssd_channels[oldest_completion_time_pos].completion_time;
		}
			

		/*		
		if(rw==0) {
			ssd_channels[oldest_completion_time_pos].completion_time = time + read_latency_ * speed_factor_;
			ssd_channels[oldest_completion_time_pos].io_request_id = io_request_id;
		}
		else {
			ssd_channels[oldest_completion_time_pos].completion_time = time + write_latency_ * speed_factor_;
			ssd_channels[oldest_completion_time_pos].io_request_id = io_request_id;
		}
		*/

	
		return 0;
	}

	/**
		current_time: the current system time. requests that have completion time earlier than current_time is considered done (will not be considered for response). 
		*num_response: the number of response at time
		*time: the time that the next response(s) will happen
		*io_request_id: an array of reqeusts that will complete at time
	*/
	long get_next_responses(long current_time, long *num_response, long *time, long *io_request_ids)
	{
		int i;
		long next_response_time = LONG_MAX;
		long next_response_channel = -1;

		if(current_time >= current_time_) {
			current_time_ = current_time;
		}
		else {
			printf("get_next_responses: time (%ld) < current_time_ (%ld)\n", current_time, current_time_);
		}


		for(i=0; i<channels_; i++) {
			if(ssd_channels[i].completion_time > current_time && ssd_channels[i].completion_time < next_response_time) {
				next_response_time = ssd_channels[i].completion_time;
				next_response_channel = i;
			}
				
		}

		if(next_response_channel!=-1) {
			*num_response = 0;
			*time = next_response_time;
		
			for(i=0; i<channels_; i++) {
				if(ssd_channels[i].completion_time == next_response_time) {
					(*num_response)++;
					*io_request_ids = ssd_channels[i].io_request_id;
					io_request_ids++;
				}
			}
		}
		else {
			*num_response = 0;
			*time = 0;
			*io_request_ids = 0;
		}

		return 0;
	}

	void dump_channel_data()
	{
		int i;

		printf("current_time_ = %ld\n", current_time_);

		for(i=0; i<channels_; i++) {
			printf("Channel %d\n", i);
			printf("  completion_time = %ld\n", ssd_channels[i].completion_time);
			printf("  io_request_id = %ld\n", ssd_channels[i].io_request_id);				
		}		
	}

	~MySSDSim()
	{
		// Dump IO channels
		dump_channel_data();
	}

};


class MyDb
{
public:
    // Constructor requires a path to the database,
    // and a database name.
    MyDb(std::string &dbName)
    	: db_(NULL, 0),
    	dbFileName_(dbName),
    	cFlags_(DB_CREATE)
    {
	    try
	    {
	        // Redirect debugging information to std::cerr
	        db_.set_error_stream(&std::cerr);

	        // Open the database
	        db_.open(NULL, dbFileName_.c_str(), NULL, DB_BTREE, cFlags_, 0);

	    }
	    // DbException is not a subclass of std::exception, so we
	    // need to catch them both.
	    catch(DbException &e)
	    {
	        std::cerr << "Error opening database: " << dbFileName_ << "\n";
	        std::cerr << e.what() << std::endl;
	    }
	    catch(std::exception &e)
	    {
	        std::cerr << "Error opening database: " << dbFileName_ << "\n";
	        std::cerr << e.what() << std::endl;
	    }  	
    }

    // Our destructor just calls our private close method.
    ~MyDb() { close(); }

    inline Db &getDb() {return db_;}

	inline void put_sector(long sector, char *buf)
	{
#ifdef USE_MEMORY_STORAGE
		memcpy(&memory_storage[sector*SECTOR_SIZE], buf, SECTOR_SIZE);
#else
		Dbt key(&sector, sizeof(sector));
		Dbt data(buf, SECTOR_SIZE);
		int ret = db_.put(NULL, &key, &data, 0);
#endif
	}

	inline void get_sector(long sector, char *buf)
	{
#ifdef USE_MEMORY_STORAGE
		memcpy(buf, &memory_storage[sector*SECTOR_SIZE], SECTOR_SIZE);
#else

		Dbt key, data;

		key.set_data(&sector);
		key.set_size(sizeof(sector));

		data.set_data(buf);
		data.set_ulen(SECTOR_SIZE);
		data.set_flags(DB_DBT_USERMEM);

		int ret = db_.get(NULL, &key, &data, 0);
#endif		
	}

private:
    Db db_;
    std::string dbFileName_;
    u_int32_t cFlags_;

    // Make sure the default constructor is private
    // We don't want it used.
    MyDb() : db_(NULL, 0) {}

    // We put our database close activity here.
    // This is called from our destructor. In
    // a more complicated example, we might want
    // to make this method public, but a private
    // method is more appropriate for this example.
    void close()
    {
	    // Close the db
	    try
	    {
	        db_.close(0);
	        std::cout << "Database " << dbFileName_
	                  << " is closed." << std::endl;
	    }
	    catch(DbException &e)
	    {
	        std::cerr << "Error closing database: " << dbFileName_ << "\n";
	        std::cerr << e.what() << std::endl;
	    }
	    catch(std::exception &e)
	    {
	        std::cerr << "Error closing database: " << dbFileName_ << "\n";
	        std::cerr << e.what() << std::endl;
	    }    
    }
};


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
	unsigned int id;
	unsigned int type;
	unsigned int data1;
	unsigned int data2;
	unsigned int data3;
	unsigned int data4;	
	unsigned int data5;
	unsigned int data6;
	unsigned int id_check;
};

struct nctuss_emu_reply { 
	unsigned int id;
	unsigned int type;
	unsigned int data1;
	unsigned int data2;
	unsigned int data3;
	unsigned int data4;	
	//Used for replying "RequestType_getNextResponses". The ids of the requets that will complete next.
	unsigned int io_request_ids[MAX_EMULATOR_CHANNELS]; 
	unsigned int id_check;
};


MySSDSim mySSDSim;

void handler(int) {
	int cmd;
		
	mySSDSim.dump_channel_data();

	printf("What to do next? 1=continue, 2=exit\n");
	scanf("%d", &cmd);

	if(cmd==1) {
		return;
	}
	else if(cmd==2) {
		exit(0);
	}
}

int main(int argc, char **argv)
{
	int socket_fd;	 /* file description into transport */
	int recfd; /* file descriptor to accept 	   */
	int length; /* length of address structure		*/
	int nbytes; /* the number of read **/
	char buf[BUFSIZ];
	long time64;

	int socket_port_offset = 0;
	int ch;
	int i, j;


	struct sockaddr_in myaddr; /* address of this service */
	struct sockaddr_in client_addr; /* address of client	*/

	struct nctuss_emu_request *request;
	struct nctuss_emu_reply *reply;
	char reply_data[sizeof(struct nctuss_emu_reply) +  SECTOR_SIZE];
	int reply_length;
	reply = (struct nctuss_emu_reply *)reply_data;


	/* */
	for(j=0; j<2; j++) {
		for(i=0; i<8; i++) {
			response_time_table_random[j][i] = ((1000000000 / iorate_table_random[j][i]) * 100);
			response_time_table_seq[j][i] = ((1000000000 / iorate_table_seq[j][i]) * 100);

			//response_time_table_random[j][i] = 0;
			//response_time_table_seq[j][i] = 0;
		}
	}

	// Parse command line options
	while((ch=getopt(argc, argv, "p:"))!=-1) {
		printf("optind: %d\n", optind);
		switch(ch) {
				case 'p':
					printf("Have options: -p\n");
					printf("The argument of -p is %s\n", optarg);
					socket_port_offset = strtol(optarg, (char **)NULL, 10);
					printf("socket_port_offset = %d\n", socket_port_offset);
					break;
		}
	}

	////struct nctuss_emu_ops emu_ops;

	signal(SIGINT, &handler);


	mySSDSim = MySSDSim();
	

	current_max_request_id = 0;
	
	string db_name("nctuss_db1.db");
	MyDb myDb(db_name);


	/*                              
	 *      Get a socket into UDP/IP
	 */
	if ((socket_fd = socket(AF_INET, SOCK_DGRAM, 0)) <0) {
		perror ("socket failed");
		exit(1);
	}

	/*
	 *    Set up our address
	 */
	bzero ((char *)&myaddr, sizeof(myaddr));
	myaddr.sin_family = AF_INET;
	myaddr.sin_addr.s_addr = htonl(INADDR_ANY);
	myaddr.sin_port = htons(SERV_PORT+socket_port_offset);


		/*
	*     Bind to the address to which the service will be offered
	*/
	if (bind(socket_fd, (struct sockaddr *)&myaddr, sizeof(myaddr)) <0) {
		perror ("bind failed\n");
		exit(1);
	}

	/*
	 * Loop continuously, waiting for datagrams
	 * and response a message
	 */
	length = sizeof(client_addr);
	printf("Server is ready to receive !!\n");
	printf("Can strike Cntrl-c to stop Server >>\n");

	while (1) {
		if ((nbytes = recvfrom(socket_fd, buf, MAXNAME, 0, 
			(struct sockaddr *) &client_addr, (socklen_t *)&length)) <0) {
			perror ("could not read datagram!!");
			continue;
		}

  		printf_cond("Received data form %s : %d\n", inet_ntoa(client_addr.sin_addr), htons(client_addr.sin_port)); 

		if(nbytes < sizeof(struct nctuss_emu_request)) {
			printf("nbytes (%d) < sizeof(struct nctuss_emu_request)!!", nbytes);
			perror ("nbytes < sizeof(struct nctuss_emu_request)!!");
			continue;
		}
		
		request = (struct nctuss_emu_request *)buf;
		printf_cond(" === request->type=%d, id=%d\n", request->type, request->id);

		if(request->type == RequestType_reset) {
			printf("Received RequestType_reset\n");
			printf("total_random_request_count = %ld\n", total_random_request_count);
			printf("total_sequential_request_count = %ld\n", total_sequential_request_count);
			printf("total_rollback_request_count = %ld\n", total_rollback_request_count);
			total_random_request_count = 0;
			total_sequential_request_count = 0;
			total_rollback_request_count = 0;
				
				
			current_max_request_id = 0;
			mySSDSim.setup(request->data1, request->data2, request->data3);
		}
		else {
			// Ignore duplicate requests
			if(request->id < current_max_request_id) {
				printf("!!!! request->id (%d) < current_max_request_id (%ld)\n", request->id, current_max_request_id);
				while(1);
				continue;
			}
			else if(request->id == current_max_request_id) {
				// resend the previous reply
				if (sendto(socket_fd, reply, reply_length, 0, (struct sockaddr *)&client_addr, length) < 0) {
					perror("Could not send datagram!!\n");
					continue;
				}
				continue;
			}
			else {
				if(request->id != request->id_check) {
					printf("!!!!!!!!!!!!!!!! request->id (%d) ! request->id_check (%d)\n", request->id, request->id_check);
					while(1);
				}
				
				current_max_request_id = request->id;
			}
		}
		
		switch(request->type) {
			case RequestType_reset:				
				reply->type = ReplyType_ok;				
				reply->id = request->id;
				break;
				
			case RequestType_submitReqeust:
				time64 = ((unsigned long)request->data1 << 32) + request->data2;
				if(mySSDSim.submit_request(time64, request->data3, request->data4, request->data5, request->data6)<0) {
					reply->type = ReplyType_error;
					reply->id = request->id;
				}
				else {
					reply->type = ReplyType_ok;
					reply->id = request->id;
				}
				break;

			case RequestType_getNextResponses:
				int i;
				long num_response, time, io_request_ids[MAX_EMULATOR_CHANNELS];
				time64 = ((unsigned long)request->data1 << 32) + request->data2;
				if(mySSDSim.get_next_responses(time64, &num_response, &time, io_request_ids)<0) {
					reply->type = ReplyType_error;
					reply->id = request->id;
				}
				else {
					reply->type = ReplyType_ok;
					reply->id = request->id;
					printf_cond("!! time=%ld, num_response = %ld, response_time=%ld\n", time64, num_response, time);
					reply->data1 = num_response;
					reply->data2 = (unsigned int)(time >> 32);
					reply->data3 = (unsigned int)(time & 0xFFFFFFFF);
					for(i=0; i<num_response; i++) {
						printf_cond("!! io_request_ids[%d] = %ld\n", i, io_request_ids[i]);
						reply->io_request_ids[i] = io_request_ids[i];
/*
						if(io_request_ids[i] == 2755 || io_request_ids[i] == 2765) {
							printf("!!!!!!!!!!!!!!!!!!!!!!!!!!!!! iorequest_id[%d] = %ld\n", i, io_request_ids[i]);
							mySSDSim.dump_channel_data();

						}
*/

					}
				}
				break;

			case RequestType_readWriteData:
				if(request->data1 == 0) {
					// read request
					myDb.get_sector(request->data2, reply_data + sizeof(struct nctuss_emu_reply));
					reply->type = ReplyType_ok;
					reply->id = request->id;
				}
				else {	
					// write request
					if(nbytes < sizeof(struct nctuss_emu_request) + SECTOR_SIZE) {
						perror("nbytes not enough, abort write request.\n");
						continue;
					}
					myDb.put_sector(request->data2, buf + sizeof(struct nctuss_emu_request));
					reply->type = ReplyType_ok;
					reply->id = request->id;
				}
				break;
		}
		
		/* return to client */
		reply->id_check = reply->id;
		if(request->type == RequestType_readWriteData && request->data1 == 0) {
			reply_length = SECTOR_SIZE + sizeof(struct nctuss_emu_reply);
		}
		else {
			reply_length = sizeof(struct nctuss_emu_reply);
		}

		printf_cond("Reply to client: reply->type=%d, id=%d ", reply->type, reply->id);

		//sleep(1);
		
		if (sendto(socket_fd, reply, reply_length, 0, (struct sockaddr *)&client_addr, length) < 0) {
			perror("Could not send datagram!!\n");
			continue;
		}
		
		//printf_cond("Can Strike Crtl-c to stop Server >>\n");
	}

	return 0;
}


