/** Definition of bt_bus_can, an abstracted CAN device driver.
 *
 * \file bus_can.h
 * \author Brian Zenowich
 * \author Traveler Hauptman
 * \author Sam Clanton
 * \author Christopher Dellin
 * \author Dan Cody
 * \date 2003-2009
 */

/* Copyright 2003, 2004, 2005, 2006, 2007, 2008, 2009
 *           Barrett Technology <support@barrett.com> */

/* This file is part of libbarrett.
 *
 * This version of libbarrett is free software: you can redistribute it
 * and/or modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This version of libbarrett is distributed in the hope that it will be
 * useful, but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this version of libbarrett.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 * Further, non-binding information about licensing is available at:
 * <http://wiki.barrett.com/libbarrett/wiki/LicenseNotes>
 */

#include <stdio.h>
#include <errno.h>
#include <string.h>

#include <syslog.h>
#include <linux/version.h>
#include <signal.h>


#ifdef CANTYPE_SOCKET
# include <rtdm/rtcan.h>

typedef int HANDLE;
#endif

#if defined(CANTYPE_PEAKISA) || defined(CANTYPE_PEAKPCI)
# include <libpcan.h>
#endif

#ifdef CANTYPE_ESD
# include "ntcan.h"
#endif


#include <barrett/cdlbt/bus_can.h>
#include <barrett/cdlbt/bus.h>
#include <barrett/cdlbt/os.h>

#ifdef CANTYPE_ESD
# define TX_QUEUE_SIZE       (32)
# define RX_QUEUE_SIZE       (32)
# define TX_TIMEOUT          (50)
# define RX_TIMEOUT          (50)
#endif

/* CAN stuff */
//#define MAX_NODES (31) /* For iteration */
#define MAX_NODES (11) /* xxx Temp Hack for BH8-280 demo */


#define AB_NEXT_IDX(i) (((i) + 1) % ASYNC_BUF_SIZE)

#define BORDER(Value,Min,Max) \
   ((Value)<(Min))?(Min):(((Value)>(Max))?(Max):(Value))

//long jointPosition[32];

/** Read a message from the CAN device.
 *
 * \param[in] dev bt_bus_can device to use
 * \param[out] id The ID of the received node
 * \param[out] len The length of the received data
 * \param[out] data The data received
 * \param[in] blocking Whether to do a blocking read
 * \retval 0 Success
 * \retval 1 Returned empty
 * \retval 2 Error reading message
 */
static int read_msg(struct bt_bus_can * dev, int * id, int * len,
                    unsigned char * data, int blocking);


/** Write a message to the CAN device
 *
 * \param[in] dev bt_bus_can device to use
 * \param[in] id The ID to send to
 * \param[in] len The length of the message to send
 * \param[in] data The data to send
 * \param[in] blocking Whether to do a blocking write
 * \retval 0 Success
 * \retval 1 Error sending message
 */
static int write_msg(struct bt_bus_can * dev, int id, char len,
                     unsigned char * data, int blocking);


/** Parse a message into an ID, a property, and a value.
 *
 * \param[in] msgid The message ID
 * \param[in] len The length of the message
 * \param[in] message_data The message data to parse
 * \param[out] id Location into which to put the node ID
 * \param[out] property Location into which to put the associated property
 * \param[out] ispacked Whether the message is in packed format
 * \param[out] value1 Location into which to put the value
 * \param[out] value2 Location into which to put the second value (if ispacked) (can be NULL)
 * \retval 0 Success
 * \retval 1 Illegal message header
 */
static int parse_msg(int msgid, int len, unsigned char * message_data,
                     int * id, int * property, int * ispacked, long * value1, long * value2);


/** Compile a message from a property and value
 *
 * \param[in] property The property to use
 * \param[in] value The value to use
 * \param[out] data The location to save the compiled message
 * \paran[out] len The location to save the message length
 * \retval 0 Success
 */
static int compile_msg(int property, long value, unsigned char * data,
                       int * len);


#ifdef CANTYPE_ESD
/** Instruct the ESD CAN device to allow messages from an ID
 *
 * \note This is only enabled for ESD CAN devices.
 */
static void allow_msg(struct bt_bus_can * dev, int id, int mask);
#endif


struct bt_bus_can_dev {
   HANDLE handle;
};


int bt_bus_can_create(struct bt_bus_can ** devptr, int port)
{
   long err;

   struct bt_bus_can * dev;
   dev = (struct bt_bus_can *) malloc(sizeof(struct bt_bus_can));
   if (!dev)
   {
      syslog(LOG_ERR,"%s: Out of memory.",__func__);
      (*devptr) = 0;
      return -1;
   }
    
   /* Initialize */
   dev->dev = 0;
   dev->mutex = 0;
   dev->iterator = 0;

   dev->dev = (struct bt_bus_can_dev *) malloc(sizeof(struct bt_bus_can_dev));
   if (!dev->dev)
   {
      syslog(LOG_ERR,"%s: Out of memory.",__func__);
      bt_bus_can_destroy(dev);
      (*devptr) = 0;
      return -1;
   }

   dev->dev->handle = 0;

   bt_os_mutex_create(&dev->mutex, BT_OS_RT);
   if (!dev->mutex)
   {
      syslog(LOG_ERR,"%s: Out of memory.",__func__);
      bt_bus_can_destroy(dev);
      (*devptr) = 0;
      return -1;
   }
   
   bt_os_mutex_create(&dev->async_mutex, BT_OS_RT);
   if (!dev->async_mutex)
   {
      syslog(LOG_ERR,"%s: Out of memory.",__func__);
      bt_bus_can_destroy(dev);
      (*devptr) = 0;
      return -1;
   }

   dev->abr_idx = 0;
   dev->abw_idx = 0;


#ifdef CANTYPE_SOCKET
   char devname[10];
   struct ifreq ifr;
   struct sockaddr_can to_addr;
   nanosecs_rel_t timeout;

   sprintf(devname, "rtcan%d", port);
   syslog(LOG_ERR, "CAN device = %s", devname);

	/* Create the socket */
	err = rt_dev_socket(PF_CAN, SOCK_RAW, CAN_RAW);
   if (err < 0) {
		syslog(LOG_ERR, "rt_dev_socket: %s\n", strerror(-err));
		syslog(LOG_ERR, "%s: rt_dev_socket(): cannot open device with type=socket, port=%d", __func__, port);
		bt_bus_can_destroy(dev);
		return -1;
   }
   dev->dev->handle = err;

	strncpy(ifr.ifr_name, devname, IFNAMSIZ);

   err = rt_dev_ioctl(dev->dev->handle, SIOCGIFINDEX, &ifr);
   if (err < 0) {
		syslog(LOG_ERR, "rt_dev_ioctl(SIOCGIFINDEX): %s\n", strerror(-err));
		bt_bus_can_destroy(dev);
		return -1;
   }

   memset(&to_addr, 0, sizeof(to_addr));
   to_addr.can_ifindex = ifr.ifr_ifindex;
   to_addr.can_family = AF_CAN;

	err = rt_dev_bind(dev->dev->handle, (struct sockaddr *)&to_addr, sizeof(to_addr));
	if (err < 0) {
	    syslog(LOG_ERR, "rt_dev_bind: %s\n", strerror(-err));
	    bt_bus_can_destroy(dev);
	    return -1;
	}

	timeout = (nanosecs_rel_t)RTDM_TIMEOUT_INFINITE;
	err = rt_dev_ioctl(dev->dev->handle, RTCAN_RTIOC_RCV_TIMEOUT, &timeout);
	if (err) {
	    syslog(LOG_ERR, "rt_dev_ioctl(RCV_TIMEOUT): %s\n", strerror(-err));
	    err = rt_dev_close(dev->dev->handle);
	    return -1;
	}

#endif /* CANTYPE_SOCKET */

#ifdef CANTYPE_PEAKISA
   /* assign ports and irqs to buses
    * needs to be updated to read ports from cat /proc/pcan/ */
   switch (port)
   {
      case 0:
         dev->dev->handle = CAN_Open(HW_ISA_SJA, 0x300, 7);
         break;
      case 1:
         dev->dev->handle = CAN_Open(HW_ISA_SJA, 0x320, 5);
         break;
      default:
         syslog(LOG_ERR, "%s: incorrect bus number, cannot open port %d",
                __func__,port);
         bt_bus_can_destroy(dev);
         (*devptr) = 0;
         return -1;
   }

   if (!dev->dev->handle)
   {
      syslog(LOG_ERR,
             "%s: CAN_Open(): cannot open device with type=isa, "
             "port=%s, irq=%s",
             __func__,
             (port==0) ? "0x300" : "0x320", 
             (port==0) ? "7" : "5");
      bt_bus_can_destroy(dev);
      (*devptr) = 0;
      return -1;
   }
#endif /* CANTYPE_PEAKISA */

#ifdef CANTYPE_PEAKPCI
   dev->dev->handle = CAN_Open(HW_PCI, (port + 1));
   if (!dev->dev->handle)
   {
      syslog(LOG_ERR,
             "%s: CAN_Open(): cannot open device with type=pci, port=%d",
             __func__,port);
      bt_bus_can_destroy(dev);
      (*devptr) = 0;
      return -1;
   }
#endif /* CANTYPE_PEAKPCI */
      
#if defined(CANTYPE_PEAKISA) || defined(CANTYPE_PEAKPCI)

   /* Clear Status */
   CAN_Status(dev->dev->handle);

   err = CAN_Init(dev->dev->handle, CAN_BAUD_1M, CAN_INIT_TYPE_ST);
   if (err)
   {
      syslog(LOG_ERR, "%s: CAN_Init() failed with %d,", __func__,errno);
      bt_bus_can_destroy(dev);
      (*devptr) = 0;
      return -1;
   }
   
   CAN_ResetFilter(dev->dev->handle);
   CAN_MsgFilter(dev->dev->handle, 0x0000, 0x053F, MSGTYPE_STANDARD);
   
#endif /* CANTYPE_PEAK */

#ifdef CANTYPE_ESD
   /* Opening can for esd. */
   err = canOpen(port, 0, TX_QUEUE_SIZE, RX_QUEUE_SIZE, TX_TIMEOUT,
                 RX_TIMEOUT, &(dev->dev->handle));
   if(err != NTCAN_SUCCESS)
   {
      syslog(LOG_ERR, "%s: canOpen() failed with error %ld", __func__, err);
      dev->dev->handle = 0;
      bt_bus_can_destroy(dev);
      (*devptr) = 0;
      return -1;
   }

   /* 1 = 1Mbps, 2 = 500kbps, 3 = 250kbps*/
   err = canSetBaudrate(dev->dev->handle, 0);
   if(err != 0)
   {
      syslog(LOG_ERR, "initCAN(): canSetBaudrate() failed with error %ld",
             err);
      bt_bus_can_destroy(dev);
      (*devptr) = 0;
      return -1;
   }
   
   allow_msg(dev, 0x0000, 0x03E0); /* Messages sent directly to host*/
   allow_msg(dev, 0x0403, 0x03E0); /* Group 3 messages*/
   allow_msg(dev, 0x0406, 0x03E0); /* Group 6 messages*/
#endif /* CANTYPE_ESD */
   
   /* Note: Removed private acceptance mask filter stuff. */

   (*devptr) = dev;
   return 0;
}


int bt_bus_can_destroy(struct bt_bus_can * dev)
{
   if (dev->dev) {
      if (dev->dev->handle) {
#ifdef CANTYPE_SOCKET
         struct  ifreq ifr;
         can_mode_t *mode;

         mode = (can_mode_t *)&ifr.ifr_ifru;
         *mode = CAN_MODE_STOP;
         rt_dev_ioctl(dev->dev->handle, SIOCSCANMODE, &ifr);
         rt_dev_close(dev->dev->handle);
#endif
#if defined(CANTYPE_PEAKISA) || defined(CANTYPE_PEAKPCI)
      CAN_Close(dev->dev->handle);
#endif
#ifdef CANTYPE_ESD
      canClose(dev->dev->handle);
#endif
      }

      free(dev->dev);
      dev->dev = NULL;
   }

   if (dev->mutex)
      bt_os_mutex_destroy(dev->mutex);
   if (dev->async_mutex)
      bt_os_mutex_destroy(dev->async_mutex);
   free(dev);
   return 0;
}


//int bt_bus_can_clearmsg(struct bt_bus_can * dev)
//{
//#if defined(CANTYPE_PEAKISA) || defined(CANTYPE_PEAKPCI)
//   long err;
//   int pendread;
//   int pendwrite;
//   int id, len;
//   unsigned char data[8];
//
//   pendread = 1;
//   err = LINUX_CAN_Extended_Status(dev->dev->handle, &pendread, &pendwrite);
//
//   while (pendread)
//   {
//      err = read_msg(dev, &id, &len, data, 1);
//      err = LINUX_CAN_Extended_Status(dev->dev->handle, &pendread, &pendwrite);
//   }
//   return 0;
//
//#else  // CANTYPE_ESD and CANTYPE_SOCKET
//
//   int id, len;
//   unsigned char data[8];
//
//   /*find a better way of clearing the bus*/
//   while (read_msg(dev, &id, &len, data, 0) == 0)
//   {
//      syslog(LOG_ERR, "Cleared unexpected message from CANbus.");
//      bt_os_usleep(1);
//   }
//   return 0;
//
//#endif
//}


int bt_bus_can_iterate_start(struct bt_bus_can * dev)
{
   dev->iterator = 0;
   return 0;
}

int bt_bus_can_iterate_next(struct bt_bus_can * dev, int * nextid,
                            int * nextstatus)
{
   int id;
   
   bt_os_rt_set_mode_hard();
   bt_os_mutex_lock(dev->mutex);
   
   for (id=dev->iterator; id<MAX_NODES; id++)
   {
      int ret;
      unsigned char data[8];
      int id_in;
      int property_in;
      long status_in;
      
      // don't iterate over the force-torque sensor
      if (id == BT_BUS_PUCK_ID_FT) {
    	  continue;
      }

      /* Compile the packet*/
      data[0] = 5; /* STAT = 5 */
      
      /* Send the packet*/
      ret = write_msg(dev, BT_BUS_CAN_NODE2ADDR(id), 1, data, 1);
      
      /* Wait 1ms*/
      bt_os_usleep(1000);
      
      /* Try to get 1 reply (non-blocking read)*/
	 ret = bt_bus_can_async_read(dev, &id_in, &property_in, &status_in, NULL, NULL, 0, 1);
	 if (ret) {
		// most likely we didn't receive a reply because the puck doesn't exist
		if (ret != 3) {
			syslog(LOG_ERR, "%s: bt_bus_can_async_read error = %d",__func__,ret);
		}
	 } else if (status_in >= 0)
	 {
		/* We found a live one! */
		bt_os_mutex_unlock(dev->mutex);
		dev->iterator = id + 1;
		*nextid = id;
		*nextstatus = status_in;
		return 1;
	 }
   }
   
   bt_os_mutex_unlock(dev->mutex);
   
   /* Success! */
   return 0;
}

int bt_bus_can_async_read(struct bt_bus_can * dev, int * id, int * property, long * value1, long * value2, unsigned char* data, int blocking, int manual_update)
{
	while (1) {
		if (manual_update) {
			int tmp_id, tmp_len;
			unsigned char data[8];

			if ( !read_msg(dev, &tmp_id, &tmp_len, data, 0) ) {
				syslog(LOG_ERR, "%s: Received an unexpected position message from the WAM.",__func__);
				return 4;
			}
		}

		if (dev->abr_idx == dev->abw_idx) {  // the buffer is empty
			if (blocking) {
				bt_os_usleep(1000);
				continue;
			} else {
				return 3;
			}
		} else {
			dev->abr_idx = AB_NEXT_IDX(dev->abr_idx);
			if (data != NULL) {
				*id = dev->async_buf[dev->abr_idx].id;
				*value1 = dev->async_buf[dev->abr_idx].len;
				memcpy(data, dev->async_buf[dev->abr_idx].data, (*value1) * sizeof(unsigned char));
				 return 0;
			} else {
				int err, ispacked;

				err = parse_msg(
						dev->async_buf[dev->abr_idx].id,
						dev->async_buf[dev->abr_idx].len,
						dev->async_buf[dev->abr_idx].data,
						id, property, &ispacked, value1, value2);

				if (err) {
					return err;
	//			} else if (ispacked) {
	//				syslog(LOG_ERR, "%s: Received an unexpected packed message.",__func__);
	//				return 2;
	//			} else if (*id == BT_BUS_PUCK_ID_WAMSAFETY  &&  *property == 8) {
	//				// this is a Shift-Activate (*value == 2) or Shift-Idle (*value == 0) message
	//				continue;
				} else {
					return 0;
				}
			}
		}
	}
}

int bt_bus_can_async_get_property(struct bt_bus_can * dev, int id, int property)
{
	int err;
	unsigned char data[8];

	/* Compile the packet*/
	data[0] = (unsigned char)property;

	/* Send the packet*/
	bt_os_mutex_lock(dev->mutex);
	err = write_msg(dev, BT_BUS_CAN_NODE2ADDR(id), 1, data, 1);
	bt_os_mutex_unlock(dev->mutex);

	if (err) {
		syslog(LOG_ERR, "%s: write_msg error = %d",__func__,err);
		return 1;
	}

	return 0;
}

int bt_bus_can_get_property(struct bt_bus_can * dev, int id, int property,
		long * reply1, long * reply2, int manual_update)
{
   int err;
   int id_in;
   int property_in;

   bt_os_mutex_lock(dev->async_mutex);
   err = bt_bus_can_async_get_property(dev, id, property);
   if (err)
   {
      bt_os_mutex_unlock(dev->async_mutex);
      syslog(LOG_ERR, "%s: bt_bus_can_async_get_property error = %d",__func__,err);
      return 1;
   }

   /* Wait for 1 reply*/
   err = bt_bus_can_async_read(dev, &id_in, &property_in, reply1, reply2, NULL, 1, manual_update);
   bt_os_mutex_unlock(dev->async_mutex);
   
   if (err)
   {
      syslog(LOG_ERR, "%s: bt_bus_can_async_read error = %d",__func__,err);
      return 1;
   }

   /* Check that the id and property match */
   if((id != id_in) || (property != property_in))
   {
      syslog(LOG_ERR, "%s: returned id or property do not match",__func__);
      return 2;
   }
   
   /* Success! */
   return 0;
}


int bt_bus_can_set_property(struct bt_bus_can * dev, int id, int property,
                            long value)
{
//   long            response;
   unsigned char   data[8];
   int             len;
   int             err;

   /*syslog(LOG_ERR, "About to compile setProperty, property = %d",
            property);
   // Compile 'set' packet*/
   err = compile_msg(property, value, data, &len);

   /*syslog(LOG_ERR, "After compilation data[0] = %d", data[0]);*/
   data[0] |= 0x80; /* Set the 'Set' bit*/

   /* Send the packet*/
   bt_os_mutex_lock(dev->mutex);
   err = write_msg(dev, (id & 0x0400) ? id : BT_BUS_CAN_NODE2ADDR(id),
                   len, data, 1);
   bt_os_mutex_unlock(dev->mutex);

//   /* BUG: This will not verify properties from groups of pucks*/
//   if(verify)
//   {
//      /* Get the new value of the property*/
//      bt_bus_can_get_property(dev, id, property, &response, 0);
//
//      /* Compare response to value*/
//      if(response != value)
//         return 2;
//   }
   return 0;
}


int bt_bus_can_get_packed(struct bt_bus_can * dev, int group, int how_many,
                          long * data, int prop)
{
   int             err;
   unsigned char   packet[8];

   /* Compile the packet*/
   packet[0] = (unsigned char)prop;

   bt_os_mutex_lock(dev->mutex);

   /* Send the packet*/
   err = write_msg(dev, BT_BUS_CAN_GROUPID(group), 1, packet, 1);

   /* Wait for each reply */
   while(how_many)
   {
      int len;
      int msgID;
      int id;
      int in_property;
      int ispacked;
      long reply;
      
      /* Read a message */
      err = read_msg(dev, &msgID, &len, packet, 1);
      if (err)
      {
         /* Timeout or other error*/
         bt_os_mutex_unlock(dev->mutex);
         return(err);
      }
       
      /* Parse the reply*/
      err = parse_msg(msgID, len, packet, &id, &in_property, &ispacked, &reply, NULL);
      if(ispacked)
      {
         data[id] = reply;
         how_many--;
      }
      else
      {
         syslog(LOG_ERR, "getPositions(): Asked group %d for position, received property %d = %ld from id %d",
                group, in_property, reply, id);
      }

   }
   bt_os_mutex_unlock(dev->mutex);
   return 0;
}


int bt_bus_can_set_torques(struct bt_bus_can * dev, int group, int * values,
                           int torque_prop)
{
   unsigned char   data[8];
   int             err;
   int             i;

   /* Bound the torque commands */
   for (i = 0; i < 4; i++)
      values[i] = BORDER(values[i], -8191, 8191);

   /* Special value-packing compilation: Packs (4) 14-bit values into 8 bytes */
   /*     0        1        2        3        4        5        6        7    */
   /* ATPPPPPP AAAAAAaa aaaaaaBB BBBBbbbb bbbbCCCC CCcccccc ccDDDDDD dddddddd */

   data[0] = torque_prop | 0x80; /* Set the "Set" bit */
   data[1] = (unsigned char)(( values[0] >> 6) & 0x00FF);
   data[2] = (unsigned char)(((values[0] << 2) & 0x00FC) | ((values[1] >> 12) & 0x0003) );
   data[3] = (unsigned char)(( values[1] >> 4) & 0x00FF);
   data[4] = (unsigned char)(((values[1] << 4) & 0x00F0) | ((values[2] >> 10) & 0x000F) );
   data[5] = (unsigned char)(( values[2] >> 2) & 0x00FF);
   data[6] = (unsigned char)(((values[2] << 6) & 0x00C0) | ((values[3] >> 8) & 0x003F) );
   data[7] = (unsigned char)( values[3] & 0x00FF);

   /* Send the data */
   bt_os_mutex_lock(dev->mutex);
   err = write_msg(dev, BT_BUS_CAN_GROUPID(group), 8, data, 1);
   bt_os_mutex_unlock(dev->mutex);
   
   return 0;
}


static int read_msg(struct bt_bus_can * dev, int * id, int * len,
                    unsigned char * data, int blocking)
{
#ifdef CANTYPE_SOCKET

	int i, ret;
	   struct can_frame frame;

	   /* Read a message back from the CAN bus */
	   //syslog(LOG_ERR, "rt_dev_recv: about to read");
	   if(blocking){
		   ret = rt_dev_recv(dev->dev->handle, (void *)&frame, sizeof(can_frame_t), 0);  // can_frame != can_frame_t, but this is how the example does it...
		}else{
			ret = rt_dev_recv(dev->dev->handle, (void *)&frame, sizeof(can_frame_t), MSG_DONTWAIT);
		}

	   if (ret < 0) {
		    switch (ret) {
		    case -ETIMEDOUT:
			    syslog(LOG_ERR, "%s: rt_dev_recv: timed out", __func__);
			    return(1);
			break;
		    case -EBADF:
			    syslog(LOG_ERR, "%s: rt_dev_recv: aborted because socket was closed", __func__);
			    return(2);
			case -EAGAIN: // -EWOULDBLOCK
				//syslog(LOG_ERR, "rt_dev_recv: no data available during non-blocking read");
			    return(2);
			break;
		    default:
				syslog(LOG_ERR, "%s: rt_dev_recv: %s\n", __func__, strerror(-ret));
				return(2);
		    }
		}
		//syslog(LOG_ERR, "rt_dev_recv: read %d bytes", frame.can_dlc);

		*id = frame.can_id;
		*len = frame.can_dlc;
		for (i = 0; i < frame.can_dlc; i++) {
			data[i] = frame.data[i];
		}

		if (frame.can_id & CAN_ERR_FLAG) {
			if (frame.can_id & CAN_ERR_BUSOFF)
				syslog(LOG_ERR, "%s: bus-off", __func__);
			if (frame.can_id & CAN_ERR_CRTL)
				syslog(LOG_ERR, "%s: controller problem", __func__);
			return(2);
		}

#endif /* CANTYPE_SOCKET */
#if defined(CANTYPE_PEAKISA) || defined(CANTYPE_PEAKPCI)
   
   long err;
   int i;
   TPCANRdMsg msg;

   bt_os_rt_set_mode_hard();
   if(blocking)
      err = LINUX_CAN_Read(dev->dev->handle, &msg);
   else
   {
      /* check if a message is pending, if not wait for a period and try again and return */
      int pendread;
      int pendwrite;
      err = LINUX_CAN_Extended_Status(dev->dev->handle, &pendread, &pendwrite);
      if(pendread)
         err = LINUX_CAN_Read(dev->dev->handle, &msg);
      else
      {
         bt_os_usleep(1000);
         err = LINUX_CAN_Extended_Status(dev->dev->handle, &pendread, &pendwrite);
         if(pendread)
            err = LINUX_CAN_Read(dev->dev->handle, &msg);
         else
            return 1; /* returned empty */
      }
   }
   if(err)
   {
      syslog(LOG_ERR, "%s: error: %ld",__func__,err);
      return 2;
   }

   /* Success! */
   (*id) = msg.Msg.ID;
   (*len) = msg.Msg.LEN;
   for(i = 0; i < msg.Msg.LEN; i++)
      data[i] = msg.Msg.DATA[i];
   
#endif
#ifdef CANTYPE_ESD
   
   long err;
   int i;
   long msgCt;
   CMSG msg;
   
   if(blocking)
      err = canRead(dev->dev->handle, &msg, &msgCt, 0);
   else
      err = canTake(dev->dev->handle, &msg, &msgCt);
   if(err != NTCAN_SUCCESS)
   {
      if(err == NTCAN_RX_TIMEOUT)
         return 1;
      else
         return 2;
   }
   if(msgCt != 1)
      return 1; /* No message received, return err */
   
   /* Success! */
   (*id) = msg.id;
   (*len) = msg.len;
   for(i = 0; i < msg.len; i++)
      data[i] = msg.data[i];
   
#endif

	// if the message contains a position from the WAM (nodes 1-7), the id is of the form 0b100***00011
	// (this mask also catches node 0, but that's us and we neither send to group 3 nor see our own messages)
	if ( (*id & 0x71F) == 0x403 ) {
		// return normally
		return 0;
	} else {  // the message is an asynchronous response
		if (*id != 0x540) {  // ignore broadcasts from the safety puck
			// put this message in the asynchronous buffer...
			int i = AB_NEXT_IDX(dev->abw_idx);
			dev->async_buf[i].id = *id;
			dev->async_buf[i].len = *len;
			memcpy(dev->async_buf[i].data, data, *len);
			dev->abw_idx = i;
		}

		// ...and try again for the synchronous position message
		return read_msg(dev, id, len, data, blocking);
	}
}


static int write_msg(struct bt_bus_can * dev, int id, char len,
                     unsigned char * data, int blocking)
{
#ifdef CANTYPE_SOCKET

	int i, ret;
	   struct can_frame frame;
	   frame.can_id = id;
	   frame.can_dlc = len;
	   for(i = 0; i < len; i++)
	      frame.data[i] = data[i];

		  //syslog(LOG_ERR, "rt_dev_recv: about to send");
		ret = rt_dev_send(dev->dev->handle, (void *)&frame, sizeof(can_frame_t), 0);
		if (ret < 0) {
		    switch (ret) {
		    case -ETIMEDOUT:
			    syslog(LOG_ERR, "%s: rt_dev_send: timed out", __func__);
			    return(1);
			break;
		    case -EBADF:
			    syslog(LOG_ERR, "%s: rt_dev_send: aborted because socket was closed", __func__);
			    return(2);
			case -EAGAIN: // -EWOULDBLOCK
				syslog(LOG_ERR, "%s: rt_dev_send: data would block during non-blocking send (output buffer full)", __func__);
			    return(2);
			break;
		    default:
				syslog(LOG_ERR, "%s: rt_dev_send: %s\n", __func__, strerror(-ret));
				return(2);
		    }
		}
	 return(0);

#endif /* CANTYPE_SOCKET */
#if defined(CANTYPE_PEAKISA) || defined(CANTYPE_PEAKPCI)

   long err;
   int i;
   TPCANMsg  msg;
   
   /* Construct message */
   msg.ID = id;
   msg.MSGTYPE = MSGTYPE_STANDARD;
   msg.LEN = len & 0x0F;
   for(i = 0; i < len; i++)
      msg.DATA[i] = data[i];

   /* make sure that write is in primary mode */
   bt_os_rt_set_mode_hard();
   if(blocking)
      err = CAN_Write(dev->dev->handle, &msg);
   else
   {
      /*non-blocking, check to see if bus is full or sending errors, if not send, else return*/
      int pendread;
      int pendwrite = 1;
      err = LINUX_CAN_Extended_Status(dev->dev->handle, &pendread, &pendwrite);
      if (err != CAN_ERR_OK)
         syslog(LOG_ERR, "%s: error while trying to get status",__func__);
      else
         err = CAN_Write(dev->dev->handle, &msg);
   }
   if(err)
   {
      syslog(LOG_ERR, "%s: canSend error: %ld",__func__,err);
      return 1;
   }
   return 0;

#endif
#ifdef CANTYPE_ESD
   
   long err;
   int i;
   long msgCt;
   CMSG msg;

   /* Construct message */
   msg.id = id;
   msg.len = len & 0x0F;
   for(i = 0; i < len; i++)
      msg.data[i] = data[i];
   
   msgCt = 1;
   if(blocking)
      err = canWrite(dev->dev->handle, &msg, &msgCt, 0);
   else
      err = canSend(dev->dev->handle, &msg, &msgCt);

   if(err != NTCAN_SUCCESS)
   {
      syslog(LOG_ERR, "%s: canWrite/Send() failed with error %lu", __func__,err);
      return 1;
   }
   return 0;
   
#endif
}

static int parse_msg(int msgid, int len, unsigned char * message_data,
                     int * id, int * property, int * ispacked, long * value1, long * value2)
{
   int i;
   int dataHeader;
   
   *ispacked=0;

   (*id) = BT_BUS_CAN_ADDR2NODE(msgid);
   if ((*id) == -1)
      syslog(LOG_ERR,"msgID:%x ",msgid);


   if ( (msgid & 0x041F) == 0x0408  ||  (msgid & 0x041F) == 0x0409 ) {  // tactile feedback
	   *property = 106;
	   *ispacked = 1;

	   *value1 = 0;
	   *value1 |= ( (long)message_data[0] << 24) & 0xFF000000;
	   *value1 |= ( (long)message_data[1] << 16) & 0x00FF0000;
	   *value1 |= ( (long)message_data[2] << 8 ) & 0x0000FF00;
	   *value1 |= ( (long)message_data[3] ) & 0x000000FF;

	   if (value2 != NULL) {
		   *value2 = 0;
		   *value2 |= ( (long)message_data[4] << 24) & 0xFF000000;
		   *value2 |= ( (long)message_data[5] << 16) & 0x00FF0000;
		   *value2 |= ( (long)message_data[6] << 8 ) & 0x0000FF00;
		   *value2 |= ( (long)message_data[7] ) & 0x000000FF;
	   }

	   return 0;
   }

   dataHeader = ((message_data[0] >> 6) & 0x0002) | ((msgid & 0x041F) == 0x0403) | ((msgid & 0x041F) == 0x0407);
   /*message_data[0] &= 0x7F;*/
   /*syslog(LOG_ERR,"Entering parsemessage");*/
   switch (dataHeader)
   {
   case 3:  /* Data is a packed 22-bit position, acceleration, etc SET */
      *value1 = 0x00000000;
      *value1 |= ( (long)message_data[0] << 16) & 0x003F0000;
      *value1 |= ( (long)message_data[1] << 8 ) & 0x0000FF00;
      *value1 |= ( (long)message_data[2] ) & 0x000000FF;

      if (*value1 & 0x00200000) /* If negative */
         *value1 |= 0xFFC00000; /* Sign-extend */

      if (value2 != NULL) {
		  *value2 = 0;
		  *value2 |= ( (long)message_data[3] << 16) & 0x003F0000;
		  *value2 |= ( (long)message_data[4] << 8 ) & 0x0000FF00;
		  *value2 |= ( (long)message_data[5] ) & 0x000000FF;

		  if (*value2 & 0x00200000) /* If negative */
			 *value2 |= 0xFFC00000; /* Sign-extend */
      }
         
      *ispacked=1;
      if ( (msgid & 0x41F) == 0x403 ) {  // if it's sent to group 3
    	  *property = 48;  // position feedback
      } else {
    	  *property = -1;
      }
      /*syslog(LOG_ERR,"Received packed set property: %d from node: %d value:%d",*property,*node,*value);*/
      break;
   case 2:  /* Data is normal, SET */
      *property = message_data[0] & 0x7F;
      //syslog(LOG_ERR, "Received property: %d", *property);
      /* Store the value, second byte of message is zero (for DSP word alignment) */
      *value1 = message_data[len-1] & 0x80 ? -1L : 0;
      for (i = len-1; i >= 2; i--)
         *value1 = *value1 << 8 | message_data[i];

      /*syslog(LOG_ERR, "Received normal set property: %d from node: %d value:%d", *property, *node, *value);*/
      /*syslog(LOG_ERR,"parsemessage after %d",value);*/
      break;
   case 0:  /* Assume firmware request (GET) */
         *property = -(message_data[0] & 0x7F); /* A negative (or zero) property means GET */
      *value1 = 0;
      /*syslog(LOG_ERR, "Received normal get property: %d from node: %d value:%d", *property, *node, *value);*/
      break;
   default:
         syslog(LOG_ERR, "<Illegal Message Header> %d\n", dataHeader);
      return 1;
   }
   /*if (*property != 8) syslog(LOG_ERR,"Value in parsemessage is: %d",*value);*/
   return 0;

}


static int compile_msg(int property, long value, unsigned char * data,
                       int * len)
{
   int i;

   /* Insert the property */
   data[0] = property;
   data[1] = 0; /* To align the values for the tater's DSP */

   /* Append the value */
   for (i = 2; i < 6; i++)
   {
      data[i] = (char)(value & 0x000000FF);
      value >>= 8;
   }

   /* Record the proper data length */
   (*len) = 6; /*(dataType[property] & 0x0007) + 2;*/

   return 0;
}


#ifdef CANTYPE_ESD
static void allow_msg(struct bt_bus_can * dev, int id, int mask)
{
   int i;
   for(i = 0; i < 2048; i++)
      if((i & ~mask) == id)
         canIdAdd(dev->dev->handle, i);
}
#endif