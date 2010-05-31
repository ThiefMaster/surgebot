#include "global.h"
#include "sock.h"

#include <linux/netfilter.h>		/* for NF_ACCEPT */
#include <linux/ip.h>
#include <netinet/tcp.h>

#include <libnetfilter_queue/libnetfilter_queue.h>

void nfqueue_init();
void nfqueue_fini();
static void nfq_sock_event(struct sock *sock, enum sock_event event, int err);
static int nfq_cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfa, void *data);

extern const char *get_streamtitle();
extern void set_current_title(const char *title);

static struct nfq_handle *h = NULL;
static struct nfq_q_handle *qh = NULL;
static struct sock *nfq_sock = NULL;

void nfqueue_init()
{
	struct nfnl_handle *nh;
	int fd;

	debug("opening nfq library handle");
	h = nfq_open();
	if(!h)
	{
		log_append(LOG_WARNING, "error during nfq_open()");
		return;
	}

	debug("unbinding existing nf_queue handler for AF_INET (if any)");
	if(nfq_unbind_pf(h, AF_INET) < 0)
	{
		log_append(LOG_WARNING, "error during nfq_unbind_pf()");
		return;
	}

	debug("binding nfnetlink_queue as nf_queue handler for AF_INET");
	if(nfq_bind_pf(h, AF_INET) < 0)
	{
		log_append(LOG_WARNING, "error during nfq_bind_pf()");
		return;
	}

	debug("binding this socket to queue '0'");
	qh = nfq_create_queue(h, 0, &nfq_cb, NULL);
	if(!qh)
	{
		log_append(LOG_WARNING, "error during nfq_create_queue()");
		return;
	}

	debug("setting copy_packet mode");
	if(nfq_set_mode(qh, NFQNL_COPY_PACKET, 0xffff) < 0)
	{
		log_append(LOG_WARNING, "can't set packet_copy mode");
		return;
	}

	nh = nfq_nfnlh(h);
	fd = nfnl_fd(nh);

	nfq_sock = sock_create(SOCK_NOSOCK, nfq_sock_event, NULL);
	sock_set_fd(nfq_sock, fd);
}

void nfqueue_fini()
{
	if(qh)
	{
		debug("unbinding from nf queue 0");
		nfq_destroy_queue(qh);
	}

	if(h)
	{
		debug("closing nfq library handle");
		nfq_close(h);
	}

	if(nfq_sock)
	{
		nfq_sock->fd = -1;
		sock_close(nfq_sock);
	}
}

static void nfq_sock_event(struct sock *sock, enum sock_event event, int err)
{
	int rv;
	char buf[4096];

	assert(event == EV_READ);

 	if((rv = recv(sock->fd, buf, sizeof(buf), 0)) && rv >= 0)
		nfq_handle_packet(h, buf, rv);
}

static unsigned int get_data_offset(char *data)
{
	struct iphdr *ip = (struct iphdr *)data;
	struct tcphdr *tcp = (struct tcphdr *)(data + (ip->ihl << 2));
	return ((ip->ihl << 2) + (tcp->doff << 2));
}

static void show_pkt_data(char *data, int len)
{
	unsigned int offset = get_data_offset(data);
	char *tmp = strdup(data + offset);
	tmp[len-offset] = '\0';
	debug("data='%s'", tmp);
	free(tmp);
}

static unsigned short TransChecksum(struct iphdr * ipHeader)
{
	unsigned short * buffer;
	unsigned int size;
	unsigned long cksum;
	cksum = 0;
	cksum += (ipHeader->saddr & 0xffff) + ((ipHeader->saddr>>16) & 0xffff); /* ip_src */
	cksum += (ipHeader->daddr & 0xffff) + ((ipHeader->daddr>>16) & 0xffff); /* ip_dest */
	cksum += (unsigned short)((ipHeader->protocol << 8) & 0xff00);
	cksum += htons(htons(ipHeader->tot_len)-(4 * ipHeader->ihl));
	size = htons(ipHeader->tot_len) - (4 * ipHeader->ihl); /* previously checked for sanity */
	buffer = (unsigned short *)((unsigned char *)ipHeader + (4 * ipHeader->ihl));
	while (size > 1)
	{
		cksum += *buffer++;
		size -= sizeof(unsigned short);
	}
	if (size)
		cksum += *(unsigned char*)buffer & 0x00ff;
	cksum = (cksum >>16) + (cksum & 0xffff);
	cksum += (cksum >>16);
	cksum = (~cksum);
	cksum = cksum ? cksum : 0xffff;

	return (unsigned short)cksum;
}


static void fix_pkt_chksum(char *data, int len)
{
	struct iphdr *iph = ((struct iphdr *)data);
	struct tcphdr *tcp = (struct tcphdr *)(data + (iph->ihl << 2));
	//debug("old len: %u", htons(iph->tot_len));
	iph->tot_len = ntohs(len);
	//debug("new len: %u", htons(iph->tot_len));
	//debug("old tcp chk: %u", tcp->check);
	iph->check = 0;
	tcp->check = 0;
	tcp->check = TransChecksum(iph);
	//debug("new tcp chk: %u", tcp->check);
}

static int htoi(char *s)
{
	int value;
	int c;

	c = ((unsigned char *)s)[0];
	if (ct_isupper(c))
		c = tolower(c);
	value = (c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10) * 16;

	c = ((unsigned char *)s)[1];
	if (ct_isupper(c))
		c = tolower(c);
	value += c >= '0' && c <= '9' ? c - '0' : c - 'a' + 10;

	return (value);
}


static const char *urldecode(char *str)
{
	static char buf[1024];
	char *dest, *data;
	int len;

	strncpy(buf, str, sizeof(buf));
	dest = buf;
	data = buf;
	len = strlen(buf);

	while (len--) {
		if (*data == '+') {
			*dest = ' ';
		}
		else if (*data == '%' && len >= 2 && ct_isxdigit((int) *(data + 1))
				 && ct_isxdigit((int) *(data + 2))) {
			*dest = (char) htoi(data + 1);
			data += 2;
			len -= 2;
		} else {
			*dest = *data;
		}
		data++;
		dest++;
	}
	*dest = '\0';
	return buf;
}

static unsigned char hexchars[] = "0123456789ABCDEF";
static char *urlencode(const char *s)
{
	register int x, y;
	int len = strlen(s);
	char *str;

	str = malloc(3 * len + 1);
	for (x = 0, y = 0; len--; x++, y++) {
		str[y] = (unsigned char) s[x];
		if ((str[y] < '0' && str[y] != '-' && str[y] != '.') ||
		    (str[y] < 'A' && str[y] > '9') ||
		    (str[y] > 'Z' && str[y] < 'a' && str[y] != '_') ||
		    (str[y] > 'z'))
		{
			str[y++] = '%';
			str[y++] = hexchars[(unsigned char) s[x] >> 4];
			str[y] = hexchars[(unsigned char) s[x] & 15];
		}
	}
	str[y] = '\0';
	return str;
}

/* returns packet id */
static u_int32_t handle_pkt(struct nfq_data *tb, u_int32_t *newlen, char **newdata)
{
	int id = 0;
	struct nfqnl_msg_packet_hdr *ph;
	//u_int32_t mark;
	int ret;
	char *data;
	static char buf[10240] = { 0 };

	*newlen = 0;
	*newdata = NULL;

	ph = nfq_get_msg_packet_hdr(tb);
	if (ph){
		id = ntohl(ph->packet_id);
	}

	/*
	mark = nfq_get_nfmark(tb);
	if (mark)
		debug("mark=%u", mark);
	*/

	ret = nfq_get_payload(tb, &data);
	if (ret >= 0)
	{
		debug("packet len: %d", ret);
		const char *new_title = get_streamtitle();

		unsigned int offset = get_data_offset(data);
		show_pkt_data(data, ret);
		char *http_start = data + offset;
		char *title_start = strstr(http_start, "&song=");
		if(title_start)
		{
			title_start += 6;
			char *title = strdup(title_start);
			char *title_end = strstr(title, "&url=");
			if(!title_end)
				title_end = strstr(title, " HTTP/");

			if(title_end)
			{
				*title_end = '\0';
				set_current_title(urldecode(title));
				free(title);
				char *new_title_enc = urlencode(new_title);
				memcpy(buf, data, offset); // copy tcp/ip headers
				memcpy(buf+offset, http_start, title_start - http_start);
				*(buf+offset+(title_start-http_start)) = '\0';
				char *http_ptr = buf + offset;
				strcat(http_ptr, new_title_enc);
				strcat(http_ptr, "&url=http%3A%2F%2F HTTP/1.0\r\n");
				strcat(http_ptr, "User-Agent: Mozilla/3.0 (compatible)\r\n\r\n");
				*newlen = offset + strlen(http_ptr);
				free(new_title_enc);

				fix_pkt_chksum(buf, *newlen);
				*newdata = buf;
			}
		}
	}

	//fputc('\n', stdout);

	return id;
}


int nfq_cb(struct nfq_q_handle *qh, struct nfgenmsg *nfmsg, struct nfq_data *nfa, void *data)
{
	u_int32_t newlen;
	char *newdata;
	u_int32_t id = handle_pkt(nfa, &newlen, &newdata);
	show_pkt_data(newdata, newlen);
	debug("=======================================");
	return nfq_set_verdict(qh, id, NF_ACCEPT, newlen, (unsigned char *)newdata);
}
