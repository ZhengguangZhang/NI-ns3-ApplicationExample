/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2019 National Instruments
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * Author: Vincent Kotzsch <vincent.kotzsch@ni.com>
 *         Clemens Felber <clemens.felber@ni.com>
 */

#ifndef SRC_NI_MODEL_COMMON_L1_L2_API_NI_API_H_
#define SRC_NI_MODEL_COMMON_L1_L2_API_NI_API_H_

#include <cstdint>   // integer types

//======================================================================================
// NIAPI message headers
//======================================================================================


//--------------------------------------------------------------------------------------
// general message header -- 8 bytes
//--------------------------------------------------------------------------------------

typedef struct sGenMsgHdr {
  uint32_t msgType;           // message type ID
  uint32_t refId;             // reference ID,  e.g. Running message sequence number
  uint32_t instId;            // instance ID
  uint32_t bodyLength;        // message body length in bytes
} GenMsgHdr;




#endif /* SRC_NI_MODEL_COMMON_L1_L2_API_NI_API_H_ */
