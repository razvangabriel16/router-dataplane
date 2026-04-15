#include <arpa/inet.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include "protocols.h"
#include "queue.h"
#include "trie.h"
#include "lib.h"

#define ETHERTYPE_IPv4 0x0800  //RFC 5342 Internet Protocol Version 4 (IPv4)
#define ETHERTYPE_IPv6 0x86DD  //RFC 5342 Internet Protocol Version 6 (IPv6)
#define ETHERTYPE_ARP 0x0806  //RFC 5342 Address Resolution Protocol (ARP)

#define RTABLES_LEN 1 //only for assignment
/*
	15 characters for max IPv4 addr (12 + 3 for dots)
	2 digits for interfaces (10 maximum interfaces: realist standard router)
	total: 15 * 3 + 2 = 47, let's say gross 50
*/
#define MAX_LINE_LENGTH 50 
#define MAX_ARP_CACHE_LENGTH 256

#define MAXI(a,b) ((a) > (b) ? (a) : (b))
#define MINI(a,b) ((a) > (b) ? (b) : (a))

int count_bits(uint32_t n) {
	int count = 0;
	while (n) {
		n = n & (n - 1);
		count++;
	}
	return count;
}

int cmp(const void *a, const void *b){
	return count_bits(((struct route_table_entry*)b)->mask) - 
			count_bits(((struct route_table_entry*)a)->mask);
}

struct route_table_entry* get_best_route_linear(uint32_t ip_dest, struct route_table_entry* rtable, size_t len){
	for(int i = 0; i < len; ++i)
		if ((ip_dest & rtable[i].mask) == rtable[i].prefix)
			return &rtable[i];
	return NULL;
}

struct route_table_entry* get_best_route_trie(uint32_t ip_dest, trienode* root){
	size_t ssize = sizeof(uint32_t) * 8;
	uint8_t bits[ssize];
	ip_dest = ntohl(ip_dest);
    for (int k = 0; k < ssize; k++) {
		bits[k] = (ip_dest >> (31 - k)) & 1;
	}
	return search_trie(root, bits, ssize);
}

size_t max_rtable_entries(char** files, int no_files){
	char* line = malloc(MAX_LINE_LENGTH * sizeof(*line));
	size_t max_lines = 0;
	for(int i = 0; i < no_files; ++i){
		FILE* f_rtable = fopen(files[i], "r");
		DIE(!f_rtable, "file doesn't exist");
		size_t no_lines = 0;
		while (fgets(line, MAX_LINE_LENGTH, f_rtable)) {
			line[strcspn(line, "\n")] = '\0';
			no_lines++;
		}
		max_lines = MAXI(max_lines, no_lines);
	}
	return max_lines;
}

struct arp_table_entry* get_mac_entry(uint32_t ip_dest, struct arp_table_entry* arp_cache, size_t arp_cache_len){
	size_t cur_len = MINI(arp_cache_len, MAX_ARP_CACHE_LENGTH);
	for(int i = 0; i < cur_len; ++i)
		if(arp_cache[i].ip == ip_dest)
			return &arp_cache[i];
	return NULL;
}

void update_arp_cache(struct arp_table_entry* arp_cache, size_t* arp_cache_len, uint32_t ip, uint8_t mac[6]){
	struct arp_table_entry new_arp_entry;
	new_arp_entry.ip = ip;
	memcpy(new_arp_entry.mac, mac, 6);
	arp_cache[MINI(*arp_cache_len, MAX_ARP_CACHE_LENGTH)] = new_arp_entry;
	(*arp_cache_len)++;
}

void send_icmp_error(char* buf, size_t interface, uint8_t mtype, uint8_t mcode) {
    char error[MAX_PACKET_LEN] = {0};
    struct ether_hdr* eth_header = (struct ether_hdr*)buf;
    struct ip_hdr* ip_header = (struct ip_hdr*)(buf + sizeof(struct ether_hdr));

    struct ether_hdr* error_eth = (struct ether_hdr*)error;
    memcpy(error_eth->ethr_dhost, eth_header->ethr_shost, 6);
    memcpy(error_eth->ethr_shost, eth_header->ethr_dhost, 6);
    error_eth->ethr_type = htons(ETHERTYPE_IPv4);

    struct ip_hdr* error_ip = (struct ip_hdr*)(error + sizeof(struct ether_hdr));
    error_ip->ver = 4;
    error_ip->ihl = 5;
    error_ip->tos = 0;
    error_ip->ttl = 64;
    error_ip->proto = IPPROTO_ICMP;
    error_ip->id = 0;
    error_ip->frag = 0;
    error_ip->dest_addr = ip_header->source_addr;
    uint32_t my_ip;
    inet_pton(AF_INET, get_interface_ip(interface), &my_ip);
    error_ip->source_addr = my_ip;
    error_ip->tot_len = htons(sizeof(struct ip_hdr) + sizeof(struct icmp_hdr) + sizeof(struct ip_hdr) + 8);
    error_ip->checksum = 0;
    error_ip->checksum = htons(checksum((uint16_t*)error_ip, sizeof(struct ip_hdr)));

    struct icmp_hdr* error_icmp = (struct icmp_hdr*)(error + sizeof(struct ether_hdr) + sizeof(struct ip_hdr));
    error_icmp->mtype = mtype;
    error_icmp->mcode = mcode;
    error_icmp->check = 0;
    memcpy((uint8_t*)error_icmp + sizeof(struct icmp_hdr), ip_header, sizeof(struct ip_hdr) + 8);
    error_icmp->check = htons(checksum((uint16_t*)error_icmp, sizeof(struct icmp_hdr) + sizeof(struct ip_hdr) + 8));

    size_t pkt_len = sizeof(struct ether_hdr) + sizeof(struct ip_hdr) + sizeof(struct icmp_hdr) + sizeof(struct ip_hdr) + 8;
    send_to_link(pkt_len, error, interface);
}

int main(int argc, char *argv[])
{
	char buf[MAX_PACKET_LEN];

	// Do not modify this line
	init(argv + 2, argc - 2);

	//parse the routing tables
	//char* files_names[] = {"rtable0.txt", "rtable1.txt"}; //placed in.rodata
	char* files_names[] = {argv[1]}; //for assignment run
	struct route_table_entry** rtables = malloc(RTABLES_LEN * sizeof(*rtables));
	uint32_t rtables_sizes[RTABLES_LEN] = {0};
	size_t max_rows_per_file = max_rtable_entries(files_names, RTABLES_LEN);
	printf("Max rows is %zu\n", max_rows_per_file);

	trienode** tries = malloc(RTABLES_LEN * sizeof(*tries));
	for (int i = 0; i < RTABLES_LEN; ++i) {
		rtables[i] = malloc(max_rows_per_file * sizeof(**rtables));
		if(!rtables[i]){
			for(int j = 0; j < i; ++j)
				free(rtables[j]);
			free(rtables);
			fprintf(stderr, "couldn't malloc an rtable\n");
			exit(EXIT_FAILURE);
		}
		rtables_sizes[i] = read_rtable(files_names[i], rtables[i]);
		printf("%d\n", rtables_sizes[i]);

		qsort(rtables[i], rtables_sizes[i], sizeof(**rtables), cmp);
		
		tries[i] = NULL;
		for (uint32_t j = 0; j < rtables_sizes[i]; j++) {
   			int prefix_len = count_bits(rtables[i][j].mask);
    		uint8_t bits[sizeof(uint32_t) * 8];
			uint32_t prefix_host = ntohl(rtables[i][j].prefix);
    		for (int k = 0; k < prefix_len; k++) {
        		bits[k] = (prefix_host >> (31 - k)) & 1;
    		}
    		trie_insert(&tries[i], bits, prefix_len, &rtables[i][j]);
		}
	}

	struct arp_table_entry* arp_cache = malloc(MAX_ARP_CACHE_LENGTH * sizeof(*arp_cache));
	DIE(!arp_cache, "ARP Cache failed\n");
	size_t arp_cache_len = 0;

	queue unsent_packets = create_queue();

	printf("***\n");

	while (1) {

		size_t interface;
		size_t len;

		interface = recv_from_any_link(buf, &len);
		DIE(interface < 0, "recv_from_any_links\n");
		//We have received a packet!
		struct ether_hdr* eth_header = (struct ether_hdr*)buf;
		struct ip_hdr* ip_header = (struct ip_hdr*)(buf + sizeof(struct ether_hdr));

		//Split the handling of the packet by it's Ethernet frame payload' type	
		switch ((ntohs(eth_header->ethr_type))) {
  			case ETHERTYPE_IPv4:
    			printf("IPv4 Packet\n");
				//Verifying if the IP header is still valid
				if(checksum((uint16_t*)ip_header, sizeof(struct ip_hdr)) != 0)
					continue;
				char *ip_str = get_interface_ip(interface);
				uint32_t my_ip;
				if (inet_pton(AF_INET, ip_str, &my_ip) != 1) {
    				fprintf(stderr, "inet_pton failed");
    				exit(EXIT_FAILURE);
				}
				if(ip_header->dest_addr == my_ip) {
					//trimite icmp
					if(ip_header->proto == IPPROTO_ICMP && 
						((struct icmp_hdr*)((uint8_t*)ip_header + sizeof(struct ip_hdr)))->mtype == 8 &&
							((struct icmp_hdr*)((uint8_t*)ip_header + sizeof(struct ip_hdr)))->mcode == 0
						) {
						struct icmp_hdr* icmp_header = (struct icmp_hdr*)((uint8_t*)ip_header + sizeof(struct ip_hdr));
						icmp_header->mtype = 0;
						icmp_header->check = 0;
						//works only for fixed size IP headers, more general instead sizeof(ip_hdr) is ip_header->ihl * 4
						size_t payload_len = ntohs(ip_header->tot_len) - sizeof(struct ip_hdr)- sizeof(struct icmp_hdr);
						icmp_header->check =  htons(checksum((uint16_t*)icmp_header, sizeof(struct icmp_hdr) + payload_len));

						ip_header->dest_addr ^= ip_header->source_addr; 
						ip_header->source_addr ^= ip_header->dest_addr;
						ip_header->dest_addr ^= ip_header->source_addr;

						ip_header->checksum = 0;
						ip_header->checksum = htons(checksum((uint16_t*)ip_header, sizeof(struct ip_hdr)));

						memcpy(eth_header->ethr_dhost, eth_header->ethr_shost, 6);
						get_interface_mac(interface, eth_header->ethr_shost);
						size_t packet_len = ntohs(ip_header->tot_len) + sizeof(struct ether_hdr);
						send_to_link(packet_len, buf, interface);
					}
					continue;
				}
				if(ip_header->ttl > 1){
					ip_header->ttl -= 1;
					ip_header->checksum = 0;
					ip_header->checksum = htons(checksum((uint16_t*)ip_header, sizeof(struct ip_hdr)));
				} else {
					//send icmp with ttl messages
					send_icmp_error(buf, interface, 11, 0);
					continue;
				}
				struct route_table_entry* best_route = get_best_route_trie(ip_header->dest_addr, tries[0]);
				if(!best_route) {
					//send icmp with destination unreachable
					send_icmp_error(buf, interface, 3, 0);
					continue;
				}
				uint32_t lookup_ip = best_route->next_hop;
				if (lookup_ip == 0)
    				lookup_ip = ip_header->dest_addr;
				struct arp_table_entry* best_mac = get_mac_entry(lookup_ip, arp_cache, arp_cache_len);
				if(!best_mac){
					//put it in queue
					//queue_enq(unsent_packets, buf) at the next recv_link will overwrite the queue
					uint8_t* pkt_copy = malloc(MAX_PACKET_LEN * sizeof(*pkt_copy));
					memcpy(pkt_copy, buf, MAX_PACKET_LEN);
					queue_enq(unsent_packets, pkt_copy);

					//send ARP request for that specific IP addr
					char arp_req[MAX_PACKET_LEN] = {0};
					memset(((struct ether_hdr*)arp_req)->ethr_dhost, 0xFF, 6);
					get_interface_mac(best_route->interface, ((struct ether_hdr*)arp_req)->ethr_shost);
					((struct ether_hdr*)arp_req)->ethr_type = htons(ETHERTYPE_ARP);

					struct arp_hdr* arp_header = (struct arp_hdr*)((uint8_t*)arp_req + sizeof(struct ether_hdr));
					arp_header->hw_len = 6;
					arp_header->hw_type = htons(1);
					arp_header->proto_type = htons(0x0800);
					arp_header->proto_len = 4;
					arp_header->opcode = htons(1);
					get_interface_mac(best_route->interface, arp_header->shwa);
					uint32_t my_ip_new;
					inet_pton(AF_INET, get_interface_ip(best_route->interface), &my_ip_new);
					arp_header->sprotoa = my_ip_new; //already in netw order
					memset(arp_header->thwa, 0, 6);
					//arp_header->tprotoa = best_route->next_hop; //already in netw order
					arp_header->tprotoa = lookup_ip;
					send_to_link(sizeof(struct ether_hdr) + sizeof(struct arp_hdr), arp_req, best_route->interface);
					continue;
				} else {
					//send it
					memcpy(eth_header->ethr_dhost, best_mac->mac, 6);
					get_interface_mac(best_route->interface, eth_header->ethr_shost);
					size_t packet_len = ntohs(ip_header->tot_len) + sizeof(struct ether_hdr); //MAX_PACKET_LEN may include junk
					send_to_link(len, buf, best_route->interface);
				}
    			break;
  			case ETHERTYPE_ARP:	
    			printf("ARP Frame\n");
				//receive arp reply
				//update the arp table
				//verify the queue and send those messages
				//when dequeue we need to free the memory
				struct arp_hdr* arp_header = (struct arp_hdr*)(buf + sizeof(struct ether_hdr));
				if (ntohs(arp_header->opcode) == 2) {
    				uint32_t ip_reply = arp_header->sprotoa;
    				update_arp_cache(arp_cache, &arp_cache_len, ip_reply, arp_header->shwa);

    				queue newqueue = create_queue();
    				while (!queue_empty(unsent_packets)) {
        				void* pkt = queue_deq(unsent_packets);
        				struct ether_hdr* eth = (struct ether_hdr*)pkt;
        				struct ip_hdr* ip = (struct ip_hdr*)((uint8_t*)pkt + sizeof(struct ether_hdr));

        				struct route_table_entry* route = get_best_route_trie(ip->dest_addr, tries[0]);
        				uint32_t pkt_lookup_ip = route->next_hop;
						if (pkt_lookup_ip == 0)
    						pkt_lookup_ip = ip->dest_addr;
						if (route && pkt_lookup_ip == ip_reply) {
    						memcpy(eth->ethr_dhost, arp_header->shwa, 6);
    						get_interface_mac(route->interface, eth->ethr_shost);
    						send_to_link(sizeof(struct ether_hdr) + ntohs(ip->tot_len), pkt, route->interface);
    						free(pkt);
						} else {
    						queue_enq(newqueue, pkt);
						}
    				}
    				unsent_packets = newqueue;
				} else if (ntohs(arp_header->opcode) == 1) {
    				uint32_t my_ip_arp;
    				inet_pton(AF_INET, get_interface_ip(interface), &my_ip_arp);
    				if (arp_header->tprotoa != my_ip_arp)
        				continue;
    				memcpy(eth_header->ethr_dhost, eth_header->ethr_shost, 6);
    				get_interface_mac(interface, eth_header->ethr_shost);
    				((struct ether_hdr*)buf)->ethr_type = htons(ETHERTYPE_ARP);

    				arp_header->opcode = htons(2);
    				memcpy(arp_header->thwa, arp_header->shwa, 6);
    				arp_header->tprotoa = arp_header->sprotoa;
    				get_interface_mac(interface, arp_header->shwa);
    				arp_header->sprotoa = my_ip_arp;

    				send_to_link(sizeof(struct ether_hdr) + sizeof(struct arp_hdr), buf, interface);
				}
    			break;
  			default:
    			printf("Ignored packet (unknown ETHERNET frame type)");
		}

	}
	return 0;
}

