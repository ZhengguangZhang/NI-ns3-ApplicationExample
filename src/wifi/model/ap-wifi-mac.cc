/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2006, 2009 INRIA
 * Copyright (c) 2009 MIRKO BANCHI
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
 * Authors: Mathieu Lacage <mathieu.lacage@sophia.inria.fr>
 *          Mirko Banchi <mk.banchi@gmail.com>
 */

#include "ap-wifi-mac.h"
#include "ns3/assert.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/pointer.h"
#include "ns3/boolean.h"
#include "wifi-phy.h"
#include "dcf-manager.h"
#include "mac-rx-middle.h"
#include "mac-tx-middle.h"
#include "mgt-headers.h"
#include "mac-low.h"
#include "amsdu-subframe-header.h"
#include "msdu-aggregator.h"

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("ApWifiMac");

NS_OBJECT_ENSURE_REGISTERED (ApWifiMac);

TypeId
ApWifiMac::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::ApWifiMac")
    .SetParent<RegularWifiMac> ()
    .SetGroupName ("Wifi")
    .AddConstructor<ApWifiMac> ()
    .AddAttribute ("BeaconInterval",
                   "Delay between two beacons",
                   TimeValue (MicroSeconds (102400)),
                   MakeTimeAccessor (&ApWifiMac::GetBeaconInterval,
                                     &ApWifiMac::SetBeaconInterval),
                   MakeTimeChecker ())
    .AddAttribute ("BeaconJitter",
                   "A uniform random variable to cause the initial beacon starting time (after simulation time 0) "
                   "to be distributed between 0 and the BeaconInterval.",
                   StringValue ("ns3::UniformRandomVariable"),
                   MakePointerAccessor (&ApWifiMac::m_beaconJitter),
                   MakePointerChecker<UniformRandomVariable> ())
    .AddAttribute ("EnableBeaconJitter",
                   "If beacons are enabled, whether to jitter the initial send event.",
                   BooleanValue (false),
                   MakeBooleanAccessor (&ApWifiMac::m_enableBeaconJitter),
                   MakeBooleanChecker ())
    .AddAttribute ("BeaconGeneration",
                   "Whether or not beacons are generated.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&ApWifiMac::SetBeaconGeneration,
                                        &ApWifiMac::GetBeaconGeneration),
                   MakeBooleanChecker ())
    .AddAttribute ("EnableNonErpProtection", "Whether or not protection mechanism should be used when non-ERP STAs are present within the BSS."
                   "This parameter is only used when ERP is supported by the AP.",
                   BooleanValue (true),
                   MakeBooleanAccessor (&ApWifiMac::m_enableNonErpProtection),
                   MakeBooleanChecker ())
  ;
  return tid;
}

ApWifiMac::ApWifiMac ()
{
  NS_LOG_FUNCTION (this);
  m_beaconDca = CreateObject<DcaTxop> ();
  m_beaconDca->SetAifsn (1);
  m_beaconDca->SetMinCw (0);
  m_beaconDca->SetMaxCw (0);
  m_beaconDca->SetLow (m_low);
  m_beaconDca->SetManager (m_dcfManager);
  m_beaconDca->SetTxMiddle (m_txMiddle);

  //Let the lower layers know that we are acting as an AP.
  SetTypeOfStation (AP);

  m_enableBeaconGeneration = false;

  // NI API CHANGE
  NI_LOG_DEBUG ("Create ApWifiMac");
  // create the NiWifiMacInterface object
  m_NiWifiMacInterface = CreateObject <NiWifiMacInterface> (NS3_AP);
  // create callbacks from ni wifi sublayer tx interface
  m_NiWifiMacInterface->SetNiApWifiRxDataEndOkCallback (MakeCallback (&ApWifiMac::Receive, this));

}

ApWifiMac::~ApWifiMac ()
{
  NS_LOG_FUNCTION (this);
  m_staList.clear();
  m_nonErpStations.clear ();
  m_nonHtStations.clear ();
}

void
ApWifiMac::DoDispose ()
{
  NS_LOG_FUNCTION (this);
  m_beaconDca = 0;
  m_enableBeaconGeneration = false;
  m_beaconEvent.Cancel ();
  RegularWifiMac::DoDispose ();
}

void
ApWifiMac::SetAddress (Mac48Address address)
{
  NS_LOG_FUNCTION (this << address);
  //As an AP, our MAC address is also the BSSID. Hence we are
  //overriding this function and setting both in our parent class.
  RegularWifiMac::SetAddress (address);
  RegularWifiMac::SetBssid (address);
}

void
ApWifiMac::SetBeaconGeneration (bool enable)
{
  NS_LOG_FUNCTION (this << enable);

  // NI API CHANGE
  NI_LOG_DEBUG ("ApWifiMac::SetBeaconGeneration: NiWifiDevType = " << m_NiWifiMacInterface->GetNiWifiDevType());

  if ((m_NiWifiMacInterface->GetNiWifiDevType() == NIAPI_AP) ||(m_NiWifiMacInterface->GetNiWifiDevType() == NIAPI_WIFI_ALL))
    {
      if (!enable)
        {
          m_beaconEvent.Cancel ();
        }
      else if (enable && !m_enableBeaconGeneration)
        {
          m_beaconEvent = Simulator::ScheduleNow (&ApWifiMac::SendOneBeacon, this);
        }
      m_enableBeaconGeneration = enable;
    }
}

bool
ApWifiMac::GetBeaconGeneration (void) const
{
  NS_LOG_FUNCTION (this);
  return m_enableBeaconGeneration;
}

Time
ApWifiMac::GetBeaconInterval (void) const
{
  NS_LOG_FUNCTION (this);
  return m_beaconInterval;
}

void
ApWifiMac::SetWifiRemoteStationManager (Ptr<WifiRemoteStationManager> stationManager)
{
  NS_LOG_FUNCTION (this << stationManager);
  m_beaconDca->SetWifiRemoteStationManager (stationManager);
  RegularWifiMac::SetWifiRemoteStationManager (stationManager);
}

void
ApWifiMac::SetLinkUpCallback (Callback<void> linkUp)
{
  NS_LOG_FUNCTION (this << &linkUp);
  RegularWifiMac::SetLinkUpCallback (linkUp);

  //The approach taken here is that, from the point of view of an AP,
  //the link is always up, so we immediately invoke the callback if
  //one is set
  linkUp ();
}

void
ApWifiMac::SetBeaconInterval (Time interval)
{
  NS_LOG_FUNCTION (this << interval);
  if ((interval.GetMicroSeconds () % 1024) != 0)
    {
      NS_LOG_WARN ("beacon interval should be multiple of 1024us (802.11 time unit), see IEEE Std. 802.11-2012");
    }
  m_beaconInterval = interval;
}

void
ApWifiMac::StartBeaconing (void)
{
  NS_LOG_FUNCTION (this);
  SendOneBeacon ();
}

int64_t
ApWifiMac::AssignStreams (int64_t stream)
{
  NS_LOG_FUNCTION (this << stream);
  m_beaconJitter->SetStream (stream);
  return 1;
}

bool
ApWifiMac::GetShortSlotTimeEnabled (void) const
{
  if (m_nonErpStations.size () != 0)
    {
      return false;
    }
  if (m_erpSupported == true && GetShortSlotTimeSupported () == true)
    {
      for (std::list<Mac48Address>::const_iterator i = m_staList.begin (); i != m_staList.end (); i++)
      {
        if (m_stationManager->GetShortSlotTimeSupported (*i) == false)
          {
            return false;
          }
      }
      return true;
    }
  return false;
}

bool
ApWifiMac::GetShortPreambleEnabled (void) const
{
  if (m_erpSupported || m_phy->GetShortPlcpPreambleSupported ())
    {
      for (std::list<Mac48Address>::const_iterator i = m_nonErpStations.begin (); i != m_nonErpStations.end (); i++)
      {
        if (m_stationManager->GetShortPreambleSupported (*i) == false)
          {
            return false;
          }
      }
      return true;
    }
  return false;
}

void
ApWifiMac::ForwardDown (Ptr<const Packet> packet, Mac48Address from,
                        Mac48Address to)
{
  NS_LOG_FUNCTION (this << packet << from << to);
  //If we are not a QoS AP then we definitely want to use AC_BE to
  //transmit the packet. A TID of zero will map to AC_BE (through \c
  //QosUtilsMapTidToAc()), so we use that as our default here.
  uint8_t tid = 0;

  //If we are a QoS AP then we attempt to get a TID for this packet
  if (m_qosSupported)
    {
      tid = QosUtilsGetTidForPacket (packet);
      //Any value greater than 7 is invalid and likely indicates that
      //the packet had no QoS tag, so we revert to zero, which'll
      //mean that AC_BE is used.
      if (tid > 7)
        {
          tid = 0;
        }
    }

  ForwardDown (packet, from, to, tid);
}

void
ApWifiMac::ForwardDown (Ptr<const Packet> packet, Mac48Address from,
                        Mac48Address to, uint8_t tid)
{
  NI_LOG_DEBUG("ApWifiMac::ForwardDown: using tid = " << static_cast<uint32_t>(tid));

  NS_LOG_FUNCTION (this << packet << from << to << static_cast<uint32_t> (tid));
  WifiMacHeader hdr;

  //For now, an AP that supports QoS does not support non-QoS
  //associations, and vice versa. In future the AP model should
  //support simultaneously associated QoS and non-QoS STAs, at which
  //point there will need to be per-association QoS state maintained
  //by the association state machine, and consulted here.
  if (m_qosSupported)
    {
      hdr.SetType (WIFI_MAC_QOSDATA);
      hdr.SetQosAckPolicy (WifiMacHeader::NORMAL_ACK);
      hdr.SetQosNoEosp ();
      hdr.SetQosNoAmsdu ();
      //Transmission of multiple frames in the same Polled TXOP is not supported for now
      hdr.SetQosTxopLimit (0);
      //Fill in the QoS control field in the MAC header
      hdr.SetQosTid (tid);
    }
  else
    {
      hdr.SetTypeData ();
    }

  if (m_htSupported || m_vhtSupported)
    {
      hdr.SetNoOrder ();
    }
  hdr.SetAddr1 (to);
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (from);
  hdr.SetDsFrom ();
  hdr.SetDsNotTo ();

  if (m_qosSupported)
    {
      // NI API CHANGE
      if (m_NiWifiMacInterface->GetNiApiEnable())
        {
          m_NiWifiMacInterface->NiStartTxCtrlDataFrame(packet, hdr);

          NI_LOG_DEBUG("ApWifiMac::ForwardDown: packet incl hdr sent to NI WiFi (QoS)");
        }
      else
        {
          //Sanity check that the TID is valid
          NS_ASSERT (tid < 8);
          m_edca[QosUtilsMapTidToAc (tid)]->Queue (packet, hdr);
        }
    }
  else
    {
      // NI API CHANGE
      if (m_NiWifiMacInterface->GetNiApiEnable())
        {
          m_NiWifiMacInterface->NiStartTxCtrlDataFrame(packet, hdr);

          NI_LOG_DEBUG("ApWifiMac::ForwardDown: packet incl hdr sent to NI WiFi (NQoS)");
        }
      else
        {
          m_dca->Queue (packet, hdr);
        }
    }
}

void
ApWifiMac::Enqueue (Ptr<const Packet> packet, Mac48Address to, Mac48Address from)
{
  NS_LOG_FUNCTION (this << packet << to << from);

  NI_LOG_DEBUG("ApWifiMac::Enqueue: to = " << to << ", from = " << from);

  // NI API CHANGE: Set destination address to BROADCAST to ensure forwarding of external packets
  to = Mac48Address("FF:FF:FF:FF:FF:FF");

  if (to.IsBroadcast () || m_stationManager->IsAssociated (to))
    {
      ForwardDown (packet, from, to);
    }
}

void
ApWifiMac::Enqueue (Ptr<const Packet> packet, Mac48Address to)
{
  NI_LOG_DEBUG("ApWifiMac::Enqueue: to = " << to << ", m_low->GetAddress () = " << m_low->GetAddress ());

  NS_LOG_FUNCTION (this << packet << to);
  //We're sending this packet with a from address that is our own. We
  //get that address from the lower MAC and make use of the
  //from-spoofing Enqueue() method to avoid duplicated code.
  Enqueue (packet, to, m_low->GetAddress ());
}

bool
ApWifiMac::SupportsSendFrom (void) const
{
  NS_LOG_FUNCTION (this);
  return true;
}

SupportedRates
ApWifiMac::GetSupportedRates (void) const
{
  NS_LOG_FUNCTION (this);
  SupportedRates rates;
  //If it is an HT-AP or VHT-AP, then add the BSSMembershipSelectorSet
  //The standard says that the BSSMembershipSelectorSet
  //must have its MSB set to 1 (must be treated as a Basic Rate)
  //Also the standard mentioned that at least 1 element should be included in the SupportedRates the rest can be in the ExtendedSupportedRates
  if (m_htSupported || m_vhtSupported)
    {
      for (uint32_t i = 0; i < m_phy->GetNBssMembershipSelectors (); i++)
        {
          rates.AddBssMembershipSelectorRate (m_phy->GetBssMembershipSelector (i));
        }
    }
  // 
  uint8_t nss = 1;  // Number of spatial streams is 1 for non-MIMO modes
  //Send the set of supported rates and make sure that we indicate
  //the Basic Rate set in this set of supported rates.
  for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
    {
      WifiMode mode = m_phy->GetMode (i);
      uint64_t modeDataRate = mode.GetDataRate (m_phy->GetChannelWidth (), false, nss);
      NS_LOG_DEBUG ("Adding supported rate of " << modeDataRate);
      rates.AddSupportedRate (modeDataRate);
      //Add rates that are part of the BSSBasicRateSet (manufacturer dependent!)
      //here we choose to add the mandatory rates to the BSSBasicRateSet,
      //except for 802.11b where we assume that only the non HR-DSSS rates are part of the BSSBasicRateSet
      if (mode.IsMandatory () && (mode.GetModulationClass () != WIFI_MOD_CLASS_HR_DSSS))
        {
          NS_LOG_DEBUG ("Adding basic mode " << mode.GetUniqueName ());
          m_stationManager->AddBasicMode (mode);
        }
    }
  //set the basic rates
  for (uint32_t j = 0; j < m_stationManager->GetNBasicModes (); j++)
    {
      WifiMode mode = m_stationManager->GetBasicMode (j);
      uint64_t modeDataRate = mode.GetDataRate (m_phy->GetChannelWidth (), false, nss);
      NS_LOG_DEBUG ("Setting basic rate " << mode.GetUniqueName ());
      rates.SetBasicRate (modeDataRate);
    }

  return rates;
}

DsssParameterSet
ApWifiMac::GetDsssParameterSet (void) const
{
  DsssParameterSet dsssParameters;
  if (m_dsssSupported)
    {
      dsssParameters.SetDsssSupported (1);
      dsssParameters.SetCurrentChannel (m_phy->GetChannelNumber ());
    }
  return dsssParameters;
}

CapabilityInformation
ApWifiMac::GetCapabilities (void) const
{
  CapabilityInformation capabilities;
  capabilities.SetShortPreamble (GetShortPreambleEnabled ());
  capabilities.SetShortSlotTime (GetShortSlotTimeEnabled ());
  return capabilities;
}

ErpInformation
ApWifiMac::GetErpInformation (void) const
{
  ErpInformation information;
  information.SetErpSupported (1);
  if (m_erpSupported)
    {
      information.SetNonErpPresent (!m_nonErpStations.empty ());
      information.SetUseProtection (GetUseNonErpProtection ());
      if (GetShortPreambleEnabled ())
        {
          information.SetBarkerPreambleMode (0);
        }
      else
        {
          information.SetBarkerPreambleMode (1);
        }
    }
  return information;
}

EdcaParameterSet
ApWifiMac::GetEdcaParameterSet (void) const
{
  EdcaParameterSet edcaParameters;
  edcaParameters.SetQosSupported (1);
  if (m_qosSupported)
    {
      Ptr<EdcaTxopN> edca;
      Time txopLimit;

      edca = m_edca.find (AC_BE)->second;
      txopLimit = edca->GetTxopLimit ();
      edcaParameters.SetBeAci(0);
      edcaParameters.SetBeCWmin(edca->GetMinCw ());
      edcaParameters.SetBeCWmax(edca->GetMaxCw ());
      edcaParameters.SetBeAifsn(edca->GetAifsn ());
      edcaParameters.SetBeTXOPLimit(txopLimit.GetMicroSeconds () / 32);
      
      edca = m_edca.find (AC_BK)->second;
      txopLimit = edca->GetTxopLimit ();
      edcaParameters.SetBkAci(1);
      edcaParameters.SetBkCWmin(edca->GetMinCw ());
      edcaParameters.SetBkCWmax(edca->GetMaxCw ());
      edcaParameters.SetBkAifsn(edca->GetAifsn ());
      edcaParameters.SetBkTXOPLimit(txopLimit.GetMicroSeconds () / 32);
      
      edca = m_edca.find (AC_VI)->second;
      txopLimit = edca->GetTxopLimit ();
      edcaParameters.SetViAci(2);
      edcaParameters.SetViCWmin(edca->GetMinCw ());
      edcaParameters.SetViCWmax(edca->GetMaxCw ());
      edcaParameters.SetViAifsn(edca->GetAifsn ());
      edcaParameters.SetViTXOPLimit(txopLimit.GetMicroSeconds () / 32);
      
      edca = m_edca.find (AC_VO)->second;
      txopLimit = edca->GetTxopLimit ();
      edcaParameters.SetVoAci(3);
      edcaParameters.SetVoCWmin(edca->GetMinCw ());
      edcaParameters.SetVoCWmax(edca->GetMaxCw ());
      edcaParameters.SetVoAifsn(edca->GetAifsn ());
      edcaParameters.SetVoTXOPLimit(txopLimit.GetMicroSeconds () / 32);
    }
  return edcaParameters;
}

HtOperations
ApWifiMac::GetHtOperations (void) const
{
  HtOperations operations;
  operations.SetHtSupported (1);
  if (m_htSupported)
    {
      if (!m_nonHtStations.empty ())
        {
          operations.SetHtProtection (MIXED_MODE_PROTECTION);
        }
      else
        {
          operations.SetHtProtection (NO_PROTECTION);
        }
    }
  return operations;
}

void
ApWifiMac::SendProbeResp (Mac48Address to)
{
  NI_LOG_DEBUG ("ApWifiMac::SendProbeResp: to " << to);

  NS_LOG_FUNCTION (this << to);
  WifiMacHeader hdr;
  hdr.SetProbeResp ();
  hdr.SetAddr1 (to);
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (GetAddress ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  Ptr<Packet> packet = Create<Packet> ();
  MgtProbeResponseHeader probe;
  probe.SetSsid (GetSsid ());
  probe.SetSupportedRates (GetSupportedRates ());
  probe.SetBeaconIntervalUs (m_beaconInterval.GetMicroSeconds ());
  probe.SetCapabilities (GetCapabilities ());
  m_stationManager->SetShortPreambleEnabled (GetShortPreambleEnabled ());
  m_stationManager->SetShortSlotTimeEnabled (GetShortSlotTimeEnabled ());
  if (m_dsssSupported)
    {
      probe.SetDsssParameterSet (GetDsssParameterSet ());
    }
  if (m_erpSupported)
    {
      probe.SetErpInformation (GetErpInformation ());
    }
  if (m_qosSupported)
    {
      probe.SetEdcaParameterSet (GetEdcaParameterSet ());
    }
  if (m_htSupported || m_vhtSupported)
    {
      probe.SetHtCapabilities (GetHtCapabilities ());
      probe.SetHtOperations (GetHtOperations ());
      hdr.SetNoOrder ();
    }
  if (m_vhtSupported)
    {
      probe.SetVhtCapabilities (GetVhtCapabilities ());
    }
  packet->AddHeader (probe);

  // NI API CHANGE
  if (m_NiWifiMacInterface->GetNiApiEnable())
    {
      //std::cout << "AP: Send probe!" << std::endl;
      m_NiWifiMacInterface->NiStartTxCtrlDataFrame(packet, hdr);
    }
  else
    {
      //The standard is not clear on the correct queue for management
      //frames if we are a QoS AP. The approach taken here is to always
      //use the DCF for these regardless of whether we have a QoS
      //association or not.
      m_dca->Queue (packet, hdr);
    }
}

void
ApWifiMac::SendAssocResp (Mac48Address to, bool success)
{
  NI_LOG_DEBUG ("ApWifiMac::SendAssocResp: to " << to << ", success = " << success);

  NS_LOG_FUNCTION (this << to << success);
  WifiMacHeader hdr;
  hdr.SetAssocResp ();
  hdr.SetAddr1 (to);
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (GetAddress ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  Ptr<Packet> packet = Create<Packet> ();
  MgtAssocResponseHeader assoc;
  StatusCode code;
  if (success)
    {
      code.SetSuccess ();
      m_staList.push_back (to);
    }
  else
    {
      code.SetFailure ();
    }
  assoc.SetSupportedRates (GetSupportedRates ());
  assoc.SetStatusCode (code);
  assoc.SetCapabilities (GetCapabilities ());
  if (m_erpSupported)
    {
      assoc.SetErpInformation (GetErpInformation ());
    }
  if (m_qosSupported)
    {
      assoc.SetEdcaParameterSet (GetEdcaParameterSet ());
    }
  if (m_htSupported || m_vhtSupported)
    {
      assoc.SetHtCapabilities (GetHtCapabilities ());
      assoc.SetHtOperations (GetHtOperations ());
      hdr.SetNoOrder ();
    }
  if (m_vhtSupported)
    {
      assoc.SetVhtCapabilities (GetVhtCapabilities ());
    }
  packet->AddHeader (assoc);

  // NI API CHANGE
  if (m_NiWifiMacInterface->GetNiApiEnable())
    {
      m_NiWifiMacInterface->NiStartTxCtrlDataFrame(packet, hdr);

      NI_LOG_CONSOLE_DEBUG("WIFI.AP: Send an association response!");
    }
  else
    {
      //The standard is not clear on the correct queue for management
      //frames if we are a QoS AP. The approach taken here is to always
      //use the DCF for these regardless of whether we have a QoS
      //association or not.
      m_dca->Queue (packet, hdr);
    }
}

void
ApWifiMac::SendOneBeacon (void)
{
  NI_LOG_DEBUG ("ApWifiMac::SendOneBeacon");

  NS_LOG_FUNCTION (this);
  WifiMacHeader hdr;
  hdr.SetBeacon ();
  hdr.SetAddr1 (Mac48Address::GetBroadcast ());
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (GetAddress ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  Ptr<Packet> packet = Create<Packet> ();
  MgtBeaconHeader beacon;
  beacon.SetSsid (GetSsid ());
  beacon.SetSupportedRates (GetSupportedRates ());
  beacon.SetBeaconIntervalUs (m_beaconInterval.GetMicroSeconds ());
  beacon.SetCapabilities (GetCapabilities ());
  m_stationManager->SetShortPreambleEnabled (GetShortPreambleEnabled ());
  m_stationManager->SetShortSlotTimeEnabled (GetShortSlotTimeEnabled ());
  if (m_dsssSupported)
    {
      beacon.SetDsssParameterSet (GetDsssParameterSet ());
    }
  if (m_erpSupported)
    {
      beacon.SetErpInformation (GetErpInformation ());
    }
  if (m_qosSupported)
    {
      beacon.SetEdcaParameterSet (GetEdcaParameterSet ());
    }
  if (m_htSupported || m_vhtSupported)
    {
      beacon.SetHtCapabilities (GetHtCapabilities ());
      beacon.SetHtOperations (GetHtOperations ());
      hdr.SetNoOrder ();
    }
  if (m_vhtSupported)
    {
      beacon.SetVhtCapabilities (GetVhtCapabilities ());
    }
  packet->AddHeader (beacon);

  // NI API CHANGE
  if (m_NiWifiMacInterface->GetNiApiEnable())
    {
      //std::cout << "AP: Send a beacon!" << std::endl;
      m_NiWifiMacInterface->NiStartTxCtrlDataFrame(packet, hdr);
    }
  else
    {
      //The beacon has it's own special queue, so we load it in there
      //std::cout << "AP: Send a beacon!" << std::endl;
      m_beaconDca->Queue (packet, hdr);
    }
  m_beaconEvent = Simulator::Schedule (m_beaconInterval, &ApWifiMac::SendOneBeacon, this);
  
  //If a STA that does not support Short Slot Time associates,
  //the AP shall use long slot time beginning at the first Beacon
  //subsequent to the association of the long slot time STA.
  if (m_erpSupported)
    {
    if (GetShortSlotTimeEnabled () == true)
      {
        //Enable short slot time
        SetSlot (MicroSeconds (9));
      }
    else
      {
        //Disable short slot time
        SetSlot (MicroSeconds (20));
      }
    }
}

void
ApWifiMac::TxOk (const WifiMacHeader &hdr)
{
  NI_LOG_DEBUG ("ApWifiMac::TxOk: hdr.IsAssocResp () = " << hdr.IsAssocResp ()
                    << ", m_stationManager->IsWaitAssocTxOk (hdr.GetAddr1 ()) = " << m_stationManager->IsWaitAssocTxOk (hdr.GetAddr1 ()));

  NS_LOG_FUNCTION (this);
  RegularWifiMac::TxOk (hdr);

  if (hdr.IsAssocResp ()
      && m_stationManager->IsWaitAssocTxOk (hdr.GetAddr1 ()))
    {
      NI_LOG_DEBUG ("ApWifiMac::TxOk: associated with address = " << hdr.GetAddr1 ());

      NS_LOG_DEBUG ("associated with sta=" << hdr.GetAddr1 ());
      m_stationManager->RecordGotAssocTxOk (hdr.GetAddr1 ());
    }
}

void
ApWifiMac::TxFailed (const WifiMacHeader &hdr)
{
  NS_LOG_FUNCTION (this);
  RegularWifiMac::TxFailed (hdr);

  if (hdr.IsAssocResp ()
      && m_stationManager->IsWaitAssocTxOk (hdr.GetAddr1 ()))
    {
      NI_LOG_DEBUG ("ApWifiMac::TxOk: association failed with sta=" << hdr.GetAddr1 ());

      NS_LOG_DEBUG ("assoc failed with sta=" << hdr.GetAddr1 ());
      m_stationManager->RecordGotAssocTxFailed (hdr.GetAddr1 ());
    }
}

void
ApWifiMac::Receive (Ptr<Packet> packet, const WifiMacHeader *hdr)
{
  NI_LOG_DEBUG("ApWifiMac::Receive");

  NS_LOG_FUNCTION (this << packet << hdr);

  Mac48Address from = hdr->GetAddr2 ();

  if (hdr->IsData ())
    {
      Mac48Address bssid = hdr->GetAddr1 ();

      NI_LOG_DEBUG("ApWifiMac::Receive: hdr->IsData ()"
          << ", !hdr->IsFromDs () = " << !hdr->IsFromDs ()
          << ", hdr->IsToDs () = " << hdr->IsToDs ()
          << ", bssid = " << bssid
          << ", GetAddress() = " << GetAddress ()
          << ", m_stationManager->IsAssociated (from) = " << m_stationManager->IsAssociated (from));

      if (!hdr->IsFromDs ()
          && hdr->IsToDs ()
          && bssid == GetAddress ()
          && (m_stationManager->IsAssociated (from)
          // NI API CHANGE: since WifiRemoteStationState is set by lower layer, we ignore it when using the ni api
          || m_NiWifiMacInterface->GetNiApiEnable()))
        {
          Mac48Address to = hdr->GetAddr3 ();

          NI_LOG_DEBUG("ApWifiMac::Receive: to = " << to << ", GetAddress() = " << GetAddress())

          if (to == GetAddress ())
            {
              NI_LOG_DEBUG("ApWifiMac::Receive: data frame for me");

              NS_LOG_DEBUG ("frame for me from=" << from);
              if (hdr->IsQosData ())
                {
                  if (hdr->IsQosAmsdu ())
                    {
                      NI_LOG_DEBUG("ApWifiMac::Receive: QoSAmsdu, DeaggregateAmsduAndForward");

                      NS_LOG_DEBUG ("Received A-MSDU from=" << from << ", size=" << packet->GetSize ());
                      DeaggregateAmsduAndForward (packet, hdr);
                      packet = 0;
                    }
                  else
                    {
                      NI_LOG_DEBUG("ApWifiMac::Receive: no QoSAmsdu, ForwardUp");

                      ForwardUp (packet, from, bssid);
                    }
                }
              else
                {
                  NI_LOG_DEBUG("ApWifiMac::Receive: no QoS data, ForwardUp");

                  ForwardUp (packet, from, bssid);
                }
            }
          else if (to.IsGroup ()
                   || m_stationManager->IsAssociated (to))
            {
              NI_LOG_DEBUG("ApWifiMac::Receive: for group"
                                    << ", to.IsGroup () = " << to.IsGroup ()
                                    << ", m_stationManager->IsAssociated (to) = " << m_stationManager->IsAssociated (to));

              NS_LOG_DEBUG ("forwarding frame from=" << from << ", to=" << to);
              Ptr<Packet> copy = packet->Copy ();

              //If the frame we are forwarding is of type QoS Data,
              //then we need to preserve the UP in the QoS control
              //header...
              if (hdr->IsQosData ())
                {
                  NI_LOG_DEBUG("ApWifiMac::Receive: QoS data, ForwardDown");

                  ForwardDown (packet, from, to, hdr->GetQosTid ());
                }
              else
                {
                  NI_LOG_DEBUG("ApWifiMac::Receive: no QoS data, ForwardDown");

                  ForwardDown (packet, from, to);
                }

              NI_LOG_DEBUG("ApWifiMac::Receive: for group, ForwardUp");

              ForwardUp (copy, from, to);
            }
          else
            {
              NI_LOG_DEBUG("ApWifiMac::Receive: ForwardUp");

              ForwardUp (packet, from, to);
            }
        }
      else if (hdr->IsFromDs ()
               && hdr->IsToDs ())
        {
          NI_LOG_DEBUG("ApWifiMac::Receive: AP-to-AP frame, drop packet");

          //this is an AP-to-AP frame
          //we ignore for now.
          NotifyRxDrop (packet);
        }
      else
        {
          NI_LOG_DEBUG("ApWifiMac::Receive: frame not targeted to AP, drop packet");

          //we can ignore these frames since
          //they are not targeted at the AP
          NotifyRxDrop (packet);
        }
      return;
    }
  else if (hdr->IsMgt ())
    {
      NI_LOG_DEBUG("ApWifiMac::Receive: hdr->IsMgt ()");

      if (hdr->IsProbeReq ())
        {
          NI_LOG_DEBUG("ApWifiMac::Receive: hdr->IsProbeReq ()");

          NS_ASSERT (hdr->GetAddr1 ().IsBroadcast ());
          SendProbeResp (from);
          return;
        }
      else if (hdr->GetAddr1 () == GetAddress ())
        {
          NI_LOG_DEBUG("ApWifiMac::Receive: hdr->GetAddr1 () == GetAddress ()");

          if (hdr->IsAssocReq ())
            {
              NI_LOG_CONSOLE_DEBUG("WIFI.AP: Received an association request!");

              //first, verify that the the station's supported
              //rate set is compatible with our Basic Rate set
              MgtAssocRequestHeader assocReq;
              packet->RemoveHeader (assocReq);
              CapabilityInformation capabilities = assocReq.GetCapabilities ();
              m_stationManager->AddSupportedPlcpPreamble (from, capabilities.IsShortPreamble ());
              SupportedRates rates = assocReq.GetSupportedRates ();
              bool problem = false;
              bool isHtStation = false;
              bool isOfdmStation = false;
              bool isErpStation = false;
              bool isDsssStation = false;
              for (uint32_t i = 0; i < m_stationManager->GetNBasicModes (); i++)
                {
                  WifiMode mode = m_stationManager->GetBasicMode (i);
                  uint8_t nss = 1; // Assume 1 spatial stream in basic mode
                  if (!rates.IsSupportedRate (mode.GetDataRate (m_phy->GetChannelWidth (), false, nss)))
                    {
                      if ((mode.GetModulationClass () == WIFI_MOD_CLASS_DSSS) || (mode.GetModulationClass () == WIFI_MOD_CLASS_HR_DSSS))
                        {
                          isDsssStation = false;
                        }
                      else if (mode.GetModulationClass () == WIFI_MOD_CLASS_ERP_OFDM)
                        {
                          isErpStation = false;
                        }
                      else if (mode.GetModulationClass () == WIFI_MOD_CLASS_OFDM)
                        {
                          isOfdmStation = false;
                        }
                      if (isDsssStation == false && isErpStation == false && isOfdmStation == false)
                        {
                          problem = true;
                          break;
                        }
                    }
                  else
                    {
                      if ((mode.GetModulationClass () == WIFI_MOD_CLASS_DSSS) || (mode.GetModulationClass () == WIFI_MOD_CLASS_HR_DSSS))
                        {
                          isDsssStation = true;
                        }
                      else if (mode.GetModulationClass () == WIFI_MOD_CLASS_ERP_OFDM)
                        {
                          isErpStation = true;
                        }
                      else if (mode.GetModulationClass () == WIFI_MOD_CLASS_OFDM)
                        {
                          isOfdmStation = true;
                        }
                    }
                }
              m_stationManager->AddSupportedErpSlotTime (from, capabilities.IsShortSlotTime () && isErpStation);
              if (m_htSupported)
                {
                  //check whether the HT STA supports all MCSs in Basic MCS Set
                  HtCapabilities htcapabilities = assocReq.GetHtCapabilities ();
                  if (htcapabilities.GetHtCapabilitiesInfo () != 0)
                    {
                      isHtStation = true;
                      for (uint32_t i = 0; i < m_stationManager->GetNBasicMcs (); i++)
                        {
                          WifiMode mcs = m_stationManager->GetBasicMcs (i);
                          if (!htcapabilities.IsSupportedMcs (mcs.GetMcsValue ()))
                            {
                              problem = true;
                              break;
                            }
                        }
                    }
                }
              if (m_vhtSupported)
                {
                  //check whether the VHT STA supports all MCSs in Basic MCS Set
                  VhtCapabilities vhtcapabilities = assocReq.GetVhtCapabilities ();
                  if (vhtcapabilities.GetVhtCapabilitiesInfo () != 0)
                    {
                      for (uint32_t i = 0; i < m_stationManager->GetNBasicMcs (); i++)
                        {
                          WifiMode mcs = m_stationManager->GetBasicMcs (i);
                          if (!vhtcapabilities.IsSupportedTxMcs (mcs.GetMcsValue ()))
                            {
                              problem = true;
                              break;
                            }
                        }
                    }
                }
              if (problem)
                {
                  NI_LOG_DEBUG("ApWifiMac::Receive: one of the Basic Rate set mode is not supported, SendAssocResp");

                  //One of the Basic Rate set mode is not
                  //supported by the station. So, we return an assoc
                  //response with an error status.
                  SendAssocResp (hdr->GetAddr2 (), false);
                }
              else
                {
                  //station supports all rates in Basic Rate Set.
                  //record all its supported modes in its associated WifiRemoteStation
                  for (uint32_t j = 0; j < m_phy->GetNModes (); j++)
                    {
                      WifiMode mode = m_phy->GetMode (j);
                      uint8_t nss = 1; // Assume 1 spatial stream in basic mode
                      if (rates.IsSupportedRate (mode.GetDataRate (m_phy->GetChannelWidth (), false, nss)))
                        {
                          m_stationManager->AddSupportedMode (from, mode);
                        }
                    }
                  if (m_htSupported)
                    {
                      HtCapabilities htcapabilities = assocReq.GetHtCapabilities ();
                      m_stationManager->AddStationHtCapabilities (from, htcapabilities);
                      for (uint32_t j = 0; j < m_phy->GetNMcs (); j++)
                        {
                          WifiMode mcs = m_phy->GetMcs (j);
                          if (mcs.GetModulationClass () == WIFI_MOD_CLASS_HT && htcapabilities.IsSupportedMcs (mcs.GetMcsValue ()))
                            {
                              m_stationManager->AddSupportedMcs (from, mcs);
                            }
                        }
                    }
                  if (m_vhtSupported)
                    {
                      VhtCapabilities vhtCapabilities = assocReq.GetVhtCapabilities ();
                      m_stationManager->AddStationVhtCapabilities (from, vhtCapabilities);
                      for (uint32_t i = 0; i < m_phy->GetNMcs (); i++)
                        {
                          WifiMode mcs = m_phy->GetMcs (i);
                          if (mcs.GetModulationClass () == WIFI_MOD_CLASS_VHT && vhtCapabilities.IsSupportedTxMcs (mcs.GetMcsValue ()))
                            {
                              m_stationManager->AddSupportedMcs (hdr->GetAddr2 (), mcs);
                              //here should add a control to add basic MCS when it is implemented
                            }
                        }
                    }
                  m_stationManager->RecordWaitAssocTxOk (from);
                  if (!isHtStation)
                    {
                      m_nonHtStations.push_back (hdr->GetAddr2 ());
                    }
                  if (!isErpStation && isDsssStation)
                    {
                      m_nonErpStations.push_back (hdr->GetAddr2 ());
                    }
                  // send assoc response with success status.
                  SendAssocResp (hdr->GetAddr2 (), true);

                  NI_LOG_DEBUG("ApWifiMac::Receive: SendAssocResp with success");
                }
              return;
            }
          else if (hdr->IsDisassociation ())
            {
              NI_LOG_DEBUG("ApWifiMac::Receive: hdr->IsDisassociation ()");

              m_stationManager->RecordDisassociated (from);
              for (std::list<Mac48Address>::iterator i = m_staList.begin (); i != m_staList.end (); i++)
              {
                if ((*i) == from)
                  {
                    m_staList.erase (i);
                    break;
                  }
              }
              for (std::list<Mac48Address>::iterator j = m_nonErpStations.begin (); j != m_nonErpStations.end (); j++)
              {
                if ((*j) == from)
                  {
                    m_nonErpStations.erase (j);
                    break;
                  }
              }
              for (std::list<Mac48Address>::iterator j = m_nonHtStations.begin (); j != m_nonHtStations.end (); j++)
              {
                if ((*j) == from)
                  {
                    m_nonHtStations.erase (j);
                    break;
                  }
              }
              return;
            }
        }
    }

  //Invoke the receive handler of our parent class to deal with any
  //other frames. Specifically, this will handle Block Ack-related
  //Management Action frames.
  RegularWifiMac::Receive (packet, hdr);
}

void
ApWifiMac::DeaggregateAmsduAndForward (Ptr<Packet> aggregatedPacket,
                                       const WifiMacHeader *hdr)
{
  NI_LOG_DEBUG ("ApWifiMac::DeaggregateAmsduAndForward");

  NS_LOG_FUNCTION (this << aggregatedPacket << hdr);
  MsduAggregator::DeaggregatedMsdus packets =
    MsduAggregator::Deaggregate (aggregatedPacket);

  for (MsduAggregator::DeaggregatedMsdusCI i = packets.begin ();
       i != packets.end (); ++i)
    {
      if ((*i).second.GetDestinationAddr () == GetAddress ())
        {
          ForwardUp ((*i).first, (*i).second.GetSourceAddr (),
                     (*i).second.GetDestinationAddr ());
        }
      else
        {
          Mac48Address from = (*i).second.GetSourceAddr ();
          Mac48Address to = (*i).second.GetDestinationAddr ();
          NS_LOG_DEBUG ("forwarding QoS frame from=" << from << ", to=" << to);

          NI_LOG_DEBUG ("ApWifiMac::DeaggregateAmsduAndForward: ForwardDown");

          ForwardDown ((*i).first, from, to, hdr->GetQosTid ());
        }
    }
}

void
ApWifiMac::DoInitialize (void)
{
  NI_LOG_DEBUG("ApWifiMac::DoInitialize");

  NS_LOG_FUNCTION (this);

  // NI API CHANGE: start beacons only for the ns-3 instantiation that shall run as AP
  if ((m_NiWifiMacInterface->GetNiWifiDevType() == NIAPI_AP) ||(m_NiWifiMacInterface->GetNiWifiDevType() == NIAPI_WIFI_ALL))
    {
      m_beaconDca->Initialize ();
      m_beaconEvent.Cancel ();
      if (m_enableBeaconGeneration)
        {
          if (m_enableBeaconJitter)
            {
              int64_t jitter = m_beaconJitter->GetValue (0, m_beaconInterval.GetMicroSeconds ());
              NS_LOG_DEBUG ("Scheduling initial beacon for access point " << GetAddress () << " at time " << jitter << " microseconds");
              m_beaconEvent = Simulator::Schedule (MicroSeconds (jitter), &ApWifiMac::SendOneBeacon, this);
              NI_LOG_DEBUG("ApWifiMac::DoInitialize: scheduled SendOneBeacon with jitter = " << jitter);
            }
          else
            {
              NS_LOG_DEBUG ("Scheduling initial beacon for access point " << GetAddress () << " at time 0");
              m_beaconEvent = Simulator::ScheduleNow (&ApWifiMac::SendOneBeacon, this);
               NI_LOG_DEBUG("ApWifiMac::DoInitialize: scheduled SendOneBeacon without jitter");
            }
        }
      RegularWifiMac::DoInitialize ();
   }
}

bool
ApWifiMac::GetUseNonErpProtection (void) const
{
  bool useProtection = !m_nonErpStations.empty () && m_enableNonErpProtection;
  m_stationManager->SetUseNonErpProtection (useProtection);
  return useProtection;
}

} //namespace ns3
