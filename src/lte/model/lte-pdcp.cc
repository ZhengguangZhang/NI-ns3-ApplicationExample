/* -*-  Mode: C++; c-file-style: "gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2011-2012 Centre Tecnologic de Telecomunicacions de Catalunya (CTTC)
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
 * Author: Manuel Requena <manuel.requena@cttc.es>
 */

#include "ns3/log.h"
#include "ns3/simulator.h"

#include "ns3/lte-pdcp.h"
#include "ns3/lte-pdcp-header.h"
#include "ns3/lte-pdcp-sap.h"
#include "ns3/lte-pdcp-tag.h"

#include "ns3/lwa-tag.h"
#include "ns3/lwip-tag.h"
#include "ns3/pdcp-lcid.h"

#include "ns3/ni-module.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("LtePdcp");

class LtePdcpSpecificLteRlcSapUser : public LteRlcSapUser
{
public:
  LtePdcpSpecificLteRlcSapUser (LtePdcp* pdcp);

  // Interface provided to lower RLC entity (implemented from LteRlcSapUser)
  virtual void ReceivePdcpPdu (Ptr<Packet> p);

private:
  LtePdcpSpecificLteRlcSapUser ();
  LtePdcp* m_pdcp;
};

LtePdcpSpecificLteRlcSapUser::LtePdcpSpecificLteRlcSapUser (LtePdcp* pdcp)
  : m_pdcp (pdcp)
{
}

LtePdcpSpecificLteRlcSapUser::LtePdcpSpecificLteRlcSapUser ()
{
}

void
LtePdcpSpecificLteRlcSapUser::ReceivePdcpPdu (Ptr<Packet> p)
{
  m_pdcp->DoReceivePdu (p);
}

///////////////////////////////////////

NS_OBJECT_ENSURE_REGISTERED (LtePdcp);

LtePdcp::LtePdcp ()
  : pdcp_decisionlwa(0),
    pdcp_decisionlwip(0),
    m_pdcpSapUser (0),
    m_rlcSapProvider (0),
    m_rnti (0),
    m_lcid (0),
    m_txSequenceNumber (0),
    m_rxSequenceNumber (0)
{
  NS_LOG_FUNCTION (this);
  m_pdcpSapProvider = new LtePdcpSpecificLtePdcpSapProvider<LtePdcp> (this);
  m_rlcSapUser = new LtePdcpSpecificLteRlcSapUser (this);
}

LtePdcp::~LtePdcp ()
{
  NS_LOG_FUNCTION (this);
}

TypeId
LtePdcp::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::LtePdcp")
    .SetParent<Object> ()
    .AddConstructor<LtePdcp> ()
    .SetGroupName("Lte")
    .AddTraceSource ("TxPDU",
                     "PDU transmission notified to the RLC.",
                     MakeTraceSourceAccessor (&LtePdcp::m_txPdu),
                     "ns3::LtePdcp::PduTxTracedCallback")
    .AddTraceSource ("RxPDU",
                     "PDU received.",
                     MakeTraceSourceAccessor (&LtePdcp::m_rxPdu),
                     "ns3::LtePdcp::PduRxTracedCallback")
    .AddTraceSource ("TxPDUtrace",//modifications for trace
                     "PDU to be transmit.",//modifications for trace
                     MakeTraceSourceAccessor (&LtePdcp::m_pdcptxtrace),//modifications for trace
                     "ns3::Packet::TracedCallback")//modifications for trace
    .AddAttribute ("PDCPDecLwa",
                     "PDCP LWA or LWIP decision variable",
                     UintegerValue (0),
                     MakeUintegerAccessor (&LtePdcp::pdcp_decisionlwa),
                     MakeUintegerChecker<uint32_t>())
    .AddAttribute ("PDCPDecLwip",
                     "PDCP LWIP decision variable",
                     UintegerValue (0),
                     MakeUintegerAccessor (&LtePdcp::pdcp_decisionlwip),
                     MakeUintegerChecker<uint32_t>())
    ;
  return tid;
}

void
LtePdcp::DoDispose ()
{
  NS_LOG_FUNCTION (this);
  delete (m_pdcpSapProvider);
  delete (m_rlcSapUser);
}


void
LtePdcp::SetRnti (uint16_t rnti)
{
  NS_LOG_FUNCTION (this << (uint32_t) rnti);
  m_rnti = rnti;
}

void
LtePdcp::SetLcId (uint8_t lcId)
{
  NS_LOG_FUNCTION (this << (uint32_t) lcId);
  m_lcid = lcId;
}

void
LtePdcp::SetLtePdcpSapUser (LtePdcpSapUser * s)
{
  NS_LOG_FUNCTION (this << s);
  m_pdcpSapUser = s;
}

LtePdcpSapProvider*
LtePdcp::GetLtePdcpSapProvider ()
{
  NS_LOG_FUNCTION (this);
  return m_pdcpSapProvider;
}

void
LtePdcp::SetLteRlcSapProvider (LteRlcSapProvider * s)
{
  NS_LOG_FUNCTION (this << s);
  m_rlcSapProvider = s;
}

LteRlcSapUser*
LtePdcp::GetLteRlcSapUser ()
{
  NS_LOG_FUNCTION (this);
  return m_rlcSapUser;
}

LtePdcp::Status 
LtePdcp::GetStatus ()
{
  Status s;
  s.txSn = m_txSequenceNumber;
  s.rxSn = m_rxSequenceNumber;
  return s;
}

void 
LtePdcp::SetStatus (Status s)
{
  m_txSequenceNumber = s.txSn;
  m_rxSequenceNumber = s.rxSn;
}

////////////////////////////////////////

void
LtePdcp::DoTransmitPdcpSdu (Ptr<Packet> p)
{
  NS_LOG_FUNCTION (this << m_rnti << (uint32_t) m_lcid << p->GetSize ());

  NI_LOG_DEBUG("eNB send PDCP data for lcid=" << (uint32_t) m_lcid << ", m_rnti=" <<  m_rnti << ", packet size=" << p->GetSize ());

  LtePdcpHeader pdcpHeader;
  pdcpHeader.SetSequenceNumber (m_txSequenceNumber);

  m_txSequenceNumber++;
  if (m_txSequenceNumber > m_maxPdcpSn)
    {
      m_txSequenceNumber = 0;
    }

  pdcpHeader.SetDcBit (LtePdcpHeader::DATA_PDU);

  NS_LOG_LOGIC ("PDCP header: " << pdcpHeader);
  p->AddHeader (pdcpHeader);

  // Sender timestamp
  PdcpTag pdcpTag (Simulator::Now ());
  p->AddPacketTag (pdcpTag);
  m_txPdu (m_rnti, m_lcid, p->GetSize ());

  LteRlcSapProvider::TransmitPdcpPduParameters params;
  params.rnti = m_rnti;
  params.lcid = m_lcid;
  params.pdcpPdu = p;

  LwaTag LWATag;
  LwipTag LWIPTag;
  PdcpLcid lcidtag;

  //NI API CHANGE: read out values from remote control database and overwrite local decision variables
  if (pdcp_decisionlwa != g_RemoteControlEngine.GetPdb()->getParameterLwaDecVariable())
    {
	  pdcp_decisionlwa = g_RemoteControlEngine.GetPdb()->getParameterLwaDecVariable();
	  NI_LOG_CONSOLE_DEBUG("NI.RC:LWA value changed! LWA value is : " << pdcp_decisionlwa);
    }
  if (pdcp_decisionlwip != g_RemoteControlEngine.GetPdb()->getParameterLwipDecVariable())
    {
	  pdcp_decisionlwip = g_RemoteControlEngine.GetPdb()->getParameterLwipDecVariable();
	  NI_LOG_CONSOLE_DEBUG("NI.RC:LWIP value changed! LWIP value is : " << pdcp_decisionlwip);
    }

  // switch between the lwa/lwip modes
  if ((pdcp_decisionlwa>0)&&(pdcp_decisionlwip==0)&&(m_lcid>=3)){
      if(pdcp_decisionlwa==1){
          // split packets between lwa und default lte link
          if((m_packetCounter%2)==0){
              m_rlcSapProvider->TransmitPdcpPdu (params);
          }
          if((m_packetCounter%2)==1){
              LWATag.Set(pdcp_decisionlwa);
              lcidtag.Set((uint32_t)m_lcid);
              p->AddPacketTag (LWATag);
              p->AddByteTag (lcidtag);
              m_pdcptxtrace(params.pdcpPdu);
          }
          m_packetCounter++;
      } else {
          // all packets routed over lwa (all drb)
          LWATag.Set(pdcp_decisionlwa);
          lcidtag.Set((uint32_t)m_lcid);
          p->AddPacketTag (LWATag);
          p->AddByteTag (lcidtag);
          m_pdcptxtrace(params.pdcpPdu);
      }
  } else if ((pdcp_decisionlwa==0)&&(pdcp_decisionlwip>0)&&(m_lcid>=3)) {
      // all packets routed over lwip
      LWIPTag.Set(pdcp_decisionlwip);
      lcidtag.Set((uint32_t)m_lcid);
      p->AddPacketTag (LWIPTag);
      p->AddByteTag (lcidtag);
      m_pdcptxtrace(params.pdcpPdu);
  } else {
      // default lte link - used also always for srb (lcid 0/1/2)
      m_rlcSapProvider->TransmitPdcpPdu (params);
  }
}

void
LtePdcp::DoReceivePdu (Ptr<Packet> p)
{
  NS_LOG_FUNCTION (this << m_rnti << (uint32_t) m_lcid << p->GetSize ());

  // Receiver timestamp
  PdcpTag pdcpTag;
  Time delay;
  NS_ASSERT_MSG (p->PeekPacketTag (pdcpTag), "PdcpTag is missing");
  p->RemovePacketTag (pdcpTag);
  delay = Simulator::Now() - pdcpTag.GetSenderTimestamp ();
  m_rxPdu(m_rnti, m_lcid, p->GetSize (), delay.GetNanoSeconds ());

  LtePdcpHeader pdcpHeader;
  p->RemoveHeader (pdcpHeader);
  NS_LOG_LOGIC ("PDCP header: " << pdcpHeader);

  m_rxSequenceNumber = pdcpHeader.GetSequenceNumber () + 1;
  if (m_rxSequenceNumber > m_maxPdcpSn)
    {
      m_rxSequenceNumber = 0;
    }

  LtePdcpSapUser::ReceivePdcpSduParameters params;
  params.pdcpSdu = p;
  params.rnti = m_rnti;
  params.lcid = m_lcid;
  m_pdcpSapUser->ReceivePdcpSdu (params);
}


} // namespace ns3
