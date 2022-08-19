/* dev.c */
/*
    Copyright (C) 1992  Ross Biro

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2, or (at your option)
    any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA. 

    The Author may be reached as bir7@leland.stanford.edu or
    C/O Department of Mathematics; Stanford University; Stanford, CA 94305
*/

#include <asm/segment.h>
#include <asm/system.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/string.h>
#include <linux/mm.h>
#include <linux/socket.h>
#include <netinet/in.h>
#include <asm/memory.h>
#include "dev.h"
#include "eth.h"
#include "timer.h"
#include "ip.h"
#include "tcp.h"
#include "sock.h"
#include <linux/errno.h>
#include "arp.h"

#undef DEV_DEBUG
#ifdef DEV_DEBUG
#define PRINTK printk
#else
#define PRINTK dummy_routine
#endif


static  unsigned long
min(unsigned long a, unsigned long b)
{
	if (a < b) return (a);
	return (b);
}

/*
 * 相同packet_type{}->type 的协议允许有多个，当存在多个的时候，
 * 前边的packet_type{}->copy 需要设置为 1.
 */
void
dev_add_pack (struct packet_type *pt)
{
	struct packet_type *p1;
	pt->next = ptype_base;

	/* see if we need to copy it. */
	for (p1 = ptype_base; p1 != NULL; p1 = p1->next)
	{
		/* 如果已经注册了相同类型的协议处理函数，此处需要设置copy */
		if (p1->type == pt->type)
		{
			pt->copy = 1;
			break;
		}
	}

	ptype_base = pt;

}

void
dev_remove_pack (struct packet_type *pt)
{
	struct packet_type *lpt, *pt1;
	if (pt == ptype_base)
	{
		ptype_base = pt->next;
		return;
	}

	lpt = NULL;

	/* 需要考虑前边相同类型协议的copy是否需要设置 */
	for (pt1 = ptype_base; pt1->next != NULL; pt1 = pt1->next)
	{
		if (pt1->next == pt)
		{
			cli();
			if (!pt->copy && lpt) 
				lpt->copy = 0;
			pt1->next = pt->next;
			sti();
			return;
		}

		if (pt1->next->type == pt->type)
		{
			lpt = pt1->next;
		}
	}
}

struct device *
get_dev (char *name)
{
	struct device *dev;
	for (dev = dev_base; dev != NULL; dev=dev->next)
	{
		if (strcmp (dev->name, name) == 0) return (dev);
	}
	return (NULL);
}

void
dev_queue_xmit (struct sk_buff *skb, struct device *dev, int pri)
{
	struct sk_buff *skb2;
	PRINTK ("eth_queue_xmit (skb=%X, dev=%X, pri = %d)\n", skb, dev, pri);
	skb->dev = dev;
	if (pri < 0 || pri >= DEV_NUMBUFFS)
	{
		printk ("bad priority in dev_queue_xmit.\n");
		pri = 1;
	}

	/* 直接发送，如果可以正常发送就返回；否则将其放到缓冲队列中 */
	if (dev->hard_start_xmit(skb, dev) == 0)
	{
		return;
	}

	if (skb->next != NULL)
	{
		printk ("retransmitted packet still on queue. \n");
		return;
	}

	/* used to say it is not currently on a send list. */
	skb->next = NULL;

	/* 双向环链表 */
	/* put skb into a bidirectional circular linked list. */
	PRINTK ("eth_queue dev->buffs[%d]=%X\n",pri, dev->buffs[pri]);
	/* interrupts should already be cleared by hard_start_xmit. */
	cli();
	if (dev->buffs[pri] == NULL)
	{
		dev->buffs[pri]=skb;
		skb->next = skb;
		skb->prev = skb;
	}
	else
	{
		skb2=dev->buffs[pri];
		skb->next = skb2;
		skb->prev = skb2->prev;
		skb->next->prev = skb;
		skb->prev->next = skb;
	}
	sti();
}


/* this routine now just gets the data out of the card and returns.
   it's return values now mean.

   1 <- exit even if you have more packets.
   0 <- call me again no matter what.
  -1 <- last packet not processed, try again. */

/*
 * 报文接收函数，将buff 中的内容copy 到skb 中，进一步处理。
 * buff 不为空表示此时接收到了新的数据；buff 为空表示此时没有接收到
 * 新数据，仅用于处理backlog 中数据。
 *
 * 1	backlog 中已经没有数据可以处理了;
 * 	将报文放到backlog 过程中没有申请到内存，且此时backlog 为空.
 * 0	将数据放到backlog 中，没有处理，需要再次调用该函数才能处理;
 * 	报文正常处理，但是backlog 并不为空.
 * -1	没有正确处理buff 且此时backlog 不为空.
 */
int
dev_rint(unsigned char *buff, unsigned long len, int flags,
	 struct device * dev)
{
	struct sk_buff *skb=NULL;
	struct packet_type *ptype;
	unsigned short type;
	unsigned char flag =0;
	unsigned char *to;
	int amount;

	/* try to grab some memory. */
	if (len > 0 && buff != NULL)
	{
		skb = malloc (sizeof (*skb) + len);	/* 申请内存 */
		skb->mem_len = sizeof (*skb) + len;	/* 内存长度 */
		skb->mem_addr = skb;			/* 内存地址 */
	}

	/* firs we copy the packet into a buffer, and save it for later. */
	if (buff != NULL && skb != NULL)
	{
		if ( !(flags & IN_SKBUFF))
		{
			to = (unsigned char *)(skb+1);
			while (len > 0)
			{
				/*
				 * 此时的buff 位于dev->rmem_start、dev->rmem_end 的环形
				 * 缓冲区内，将这部分内容依次copy 到新申请的内存中.
				 */
				amount = min (len, (unsigned long) dev->rmem_end -
						(unsigned long) buff);
				memcpy (to, buff, amount);
				len -= amount;
				buff += amount;
				to += amount;
				if ((unsigned long)buff == dev->rmem_end)
					buff = (unsigned char *)dev->rmem_start;
			}
		}
		else
		{
			/* IN_SKBUFF 标识位表示传递过来的是一个skb_buff{} 结构体 */
			free_s (skb->mem_addr, skb->mem_len);
			skb = (struct sk_buff *)buff;
		}

		skb->len = len;
		skb->dev = dev;
		skb->sk = NULL;

		/* 将报文加入到一个双向环形链表中 */
		/* now add it to the dev backlog. */
		cli();
		if (dev->backlog == NULL)
		{
			skb->prev = skb;
			skb->next = skb;
			dev->backlog = skb;
		}
		else
		{
			skb->prev = dev->backlog->prev;
			skb->next = dev->backlog;
			skb->next->prev = skb;
			skb->prev->next = skb;
		}
		sti();
		/* 此处返回 0 表示只将报文加入到backlog 中并没有处理，所以需要再调用一次 */
		return (0);
	}

	/*
	 * TODO: 没理解什么时候会走入该判断分支
	 * skb != NULL 时必定 buff != NULL，所以一定会进入到上边的处理分支中，在上边分支中返回，
	 * 所以该分支应该进不去.
	 */
	if (skb != NULL)
		free_s (skb->mem_addr, skb->mem_len);

	/* anything left to process? */

	if (dev->backlog == NULL)
	{
		/* 此时已经没有报文接收了，直接返回 1 */
		if (buff == NULL)
		{
			sti();
			return (1);
		}

		/* TODO: 没理解什么时候会走入该判断分支，原因同上 */
		if (skb != NULL)
		{
			sti();
			return (-1);
		}

		/* 此时buff != NULL 但是 skb == NULL，是因为没有内存所以无法将包文放到backlog 中，返回 1 */
		sti();
		printk ("dev_rint:Dropping packets due to lack of memory\n");
		return (1);
	}

	/* 处理过程中，一次就只处理一个包 */
	skb= dev->backlog;
	if (skb->next == skb)
	{
		dev->backlog = NULL;
	}
	else
	{
		dev->backlog = skb->next;
		skb->next->prev = skb->prev;
		skb->prev->next = skb->next;
	}
	sti();

	/* bump the pointer to the next structure. */
	skb->h.raw = (unsigned char *)(skb+1) + dev->hard_header_len;
	skb->len -= dev->hard_header_len;

	/* convert the type to an ethernet type. */
	type = dev->type_trans (skb, dev);

	/* if there get to be a lot of types we should changes this to
	   a bunch of linked lists like we do for ip protocols. */
	for (ptype = ptype_base; ptype != NULL; ptype=ptype->next)
	{
		if (ptype->type == type)
		{
			struct sk_buff *skb2;
			/* copy the packet if we need to. */
			if (ptype->copy)
			{
				skb2 = malloc (skb->mem_len);
				if (skb2 == NULL) continue;
				memcpy (skb2, skb, skb->mem_len);
				skb2->mem_addr = skb2;
			}
			else
			{
				skb2 = skb;
				flag = 1;
			}

			ptype->func (skb2, dev, ptype);
		}
	}

	/*
	 * 如果flag 此时为0，表示此时没有进入到skb2 = skb 分支，也就是说skb 此时
	 * 是没有用处的，需要是放掉.
	 * 按照添加ptype 代码来看，这里应该是保持代码健壮性的，正常不会进入到该分支.
	 */
	if (!flag)
	{
		PRINTK ("discarding packet type = %X\n", type);
		free_skb (skb, FREE_READ);
	}

	/*
	 * 此时backlog 非空。
	 * 如果buff == NULL，表示此时仅仅是用于处理backlog 中报文，backlog 不为空，
	 * 所以需要重新调用，此时返回(0)。
	 * 如果buff != NULL，表示此时想要将报文放到backlog 中，但是没有成功，需要
	 * 重新处理，此时返回(-1)。
	 */
	if (buff == NULL)
		return (0);
	else
		return (-1);
}

/* This routine is called when an device interface is ready to
   transmit a packet.  Buffer points to where the packet should
   be put, and the routine returns the length of the packet.  A
   length of zero is interrpreted to mean the transmit buffers
   are empty, and the transmitter should be shut down. */

/*
 * 报文发送函数
 * 该函数做的就是将要发送的数据复制到buff 所指向的区域。
 * 对于回环网卡，就是本地申请的内存区域；对于实际的物理网卡，是物理
 * 网卡映射到本地的存储区域。
 */
unsigned long
dev_tint(unsigned char *buff,  struct device *dev)
{
	int i;
	int tmp;
	struct sk_buff *skb;

	/* 循环所有的缓冲区 */
	for (i=0; i < DEV_NUMBUFFS; i++)
	{
		/* 如果缓冲区不为空 */
		while (dev->buffs[i]!=NULL)
		{
			cli();
			/* 从双向循环队列中取出一个skb */
			skb=dev->buffs[i];
			if (skb->next == skb)
			{
				dev->buffs[i] = NULL;
			}
			else
			{
				dev->buffs[i]=skb->next;
				skb->prev->next = skb->next;
				skb->next->prev = skb->prev;
			}
			skb->next = NULL;
			skb->prev = NULL;
			sti();
			tmp = skb->len;
			if (!skb->arp)
			{
				if (dev->rebuild_header (skb+1, dev))
				{
					skb->dev = dev;
					arp_queue (skb);
					continue;
				}
			}

			if (tmp <= dev->mtu)
			{
				/*
				 * 准备了两种发送模式：
				 * 调用 send_packet()发送；将报文复制到buff区域
				 */
				if (dev->send_packet != NULL)
				{
					dev->send_packet(skb, dev);
				}
				if (buff != NULL)
					memcpy (buff, skb + 1, tmp);

				PRINTK (">>\n");
				print_eth ((struct enet_header *)(skb+1));
			}
			else
			{
				printk ("**** bug len bigger than mtu. \n");
			}

			if (skb->free)
			{
				free_skb(skb, FREE_WRITE);
			}

			/* 一次性只能发送一个包文 */
			if (tmp != 0)
				return (tmp);
		}
	}

	/* 没有包文发送时，返回 0 */
	PRINTK ("dev_tint returning 0 \n");
	return (0);
}

