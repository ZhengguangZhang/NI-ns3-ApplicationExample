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

#include "sta-wifi-mac.h"
#include "ns3/log.h"
#include "ns3/simulator.h"
#include "ns3/string.h"
#include "ns3/pointer.h"
#include "ns3/boolean.h"
#include "ns3/trace-source-accessor.h"
#include "mac-low.h"
#include "dcf-manager.h"
#include "mac-rx-middle.h"
#include "mac-tx-middle.h"
#include "wifi-mac-header.h"
#include "msdu-aggregator.h"
#include "amsdu-subframe-header.h"
#include "mgt-headers.h"
#include "ht-capabilities.h"
#include "ht-operations.h"
#include "vht-capabilities.h"

/*
 * The state machine for this STA is:
 --------------                                          -----------
 | Associated |   <--------------------      ------->    | Refused |
 --------------                        \    /            -----------
    \                                   \  /
     \    -----------------     -----------------------------
      \-> | Beacon Missed | --> | Wait Association Response |
          -----------------     -----------------------------
                \                       ^
                 \                      |
                  \    -----------------------
                   \-> | Wait Probe Response |
                       -----------------------
 */

namespace ns3 {

NS_LOG_COMPONENT_DEFINE ("StaWifiMac");

NS_OBJECT_ENSURE_REGISTERED (StaWifiMac);

TypeId
StaWifiMac::GetTypeId (void)
{
  static TypeId tid = TypeId ("ns3::StaWifiMac")
    .SetParent<RegularWifiMac> ()
    .SetGroupName ("Wifi")
    .AddConstructor<StaWifiMac> ()
    .AddAttribute ("ProbeRequestTimeout", "The interval between two consecutive probe request attempts.",
                   TimeValue (Seconds (0.05)),
                   MakeTimeAccessor (&StaWifiMac::m_probeRequestTimeout),
                   MakeTimeChecker ())
    .AddAttribute ("AssocRequestTimeout", "The interval between two consecutive assoc request attempts.",
                   TimeValue (Seconds (0.5)),
                   MakeTimeAccessor (&StaWifiMac::m_assocRequestTimeout),
                   MakeTimeChecker ())
    .AddAttribute ("MaxMissedBeacons",
                   "Number of beacons which much be consecutively missed before "
                   "we attempt to restart association.",
                   UintegerValue (10),
                   MakeUintegerAccessor (&StaWifiMac::m_maxMissedBeacons),
                   MakeUintegerChecker<uint32_t> ())
    .AddAttribute ("ActiveProbing",
                   "If true, we send probe requests. If false, we don't."
                   "NOTE: if more than one STA in your simulation is using active probing, "
                   "you should enable it at a different simulation time for each STA, "
                   "otherwise all the STAs will start sending probes at the same time resulting in collisions. "
                   "See bug 1060 for more info.",
                   BooleanValue (false),
                   MakeBooleanAccessor (&StaWifiMac::SetActiveProbing, &StaWifiMac::GetActiveProbing),
                   MakeBooleanChecker ())
    .AddTraceSource ("Assoc", "Associated with an access point.",
                     MakeTraceSourceAccessor (&StaWifiMac::m_assocLogger),
                     "ns3::Mac48Address::TracedCallback")
    .AddTraceSource ("DeAssoc", "Association with an access point lost.",
                     MakeTraceSourceAccessor (&StaWifiMac::m_deAssocLogger),
                     "ns3::Mac48Address::TracedCallback")
  ;
  return tid;
}

StaWifiMac::StaWifiMac ()
  : m_state (BEACON_MISSED),
    m_probeRequestEvent (),
    m_assocRequestEvent (),
    m_beaconWatchdogEnd (Seconds (0.0))
{
  NS_LOG_FUNCTION (this);

    //Let the lower layers know that we are acting as a non-AP STA in
    //an infrastructure BSS.
    SetTypeOfStation (STA);
    // NI API CHANGE
    NI_LOG_DEBUG ("Create StaWifiMac");
    // create the NiWifiMacInterface object
    m_NiWifiMAcInterface = CreateObject <NiWifiMacInterface> (NS3_STA);
    // create callbacks from ni wifi sublayer tx interface
    m_NiWifiMAcInterface->SetNiApWifiRxDataEndOkCallback (MakeCallback (&StaWifiMac::Receive, this));
  }

StaWifiMac::~StaWifiMac ()
{
  NS_LOG_FUNCTION (this);
}

void
StaWifiMac::SetMaxMissedBeacons (uint32_t missed)
{
  NS_LOG_FUNCTION (this << missed);
  m_maxMissedBeacons = missed;
}

void
StaWifiMac::SetProbeRequestTimeout (Time timeout)
{
  NS_LOG_FUNCTION (this << timeout);
  m_probeRequestTimeout = timeout;
}

void
StaWifiMac::SetAssocRequestTimeout (Time timeout)
{
  NS_LOG_FUNCTION (this << timeout);
  m_assocRequestTimeout = timeout;
}

void
StaWifiMac::StartActiveAssociation (void)
{
  NS_LOG_FUNCTION (this);
  TryToEnsureAssociated ();
}

void
StaWifiMac::SetActiveProbing (bool enable)
{
  NS_LOG_FUNCTION (this << enable);
  if (enable)
    {
      Simulator::ScheduleNow (&StaWifiMac::TryToEnsureAssociated, this);
    }
  else
    {
      m_probeRequestEvent.Cancel ();
    }
  m_activeProbing = enable;
}

bool StaWifiMac::GetActiveProbing (void) const
{
  return m_activeProbing;
}

void
StaWifiMac::SendProbeRequest (void)
{
  NI_LOG_DEBUG ("StaWifiMac::SendProbeRequest");

  NS_LOG_FUNCTION (this);
  WifiMacHeader hdr;
  hdr.SetProbeReq ();
  hdr.SetAddr1 (Mac48Address::GetBroadcast ());
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (Mac48Address::GetBroadcast ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  Ptr<Packet> packet = Create<Packet> ();
  MgtProbeRequestHeader probe;
  probe.SetSsid (GetSsid ());
  probe.SetSupportedRates (GetSupportedRates ());
  if (m_htSupported || m_vhtSupported)
    {
      probe.SetHtCapabilities (GetHtCapabilities ());
      hdr.SetNoOrder ();

      NI_LOG_DEBUG ("StaWifiMac::SendProbeRequest: probe is HT capable"<< hdr<< packet);
    }
  if (m_vhtSupported)
    {
      probe.SetVhtCapabilities (GetVhtCapabilities ());

     NI_LOG_DEBUG ("StaWifiMac::SendProbeRequest: probe is VHT capable"<< hdr<< packet);
    }
  packet->AddHeader (probe);

  // NI API CHANGE
  if (m_NiWifiMAcInterface->GetNiApiEnable())
    {
      //std::cout << "STA: Send a probe request!" << std::endl;
      m_NiWifiMAcInterface->NiStartTxCtrlDataFrame(packet, hdr);

      NI_LOG_DEBUG ("WIFI.STA: Send an probe request");
    }
  else
    {
      //The standard is not clear on the correct queue for management
      //frames if we are a QoS AP. The approach taken here is to always
      //use the DCF for these regardless of whether we have a QoS
      //association or not.
      m_dca->Queue (packet, hdr);
    }

  if (m_probeRequestEvent.IsRunning ())
    {
      m_probeRequestEvent.Cancel ();
    }
  m_probeRequestEvent = Simulator::Schedule (m_probeRequestTimeout,
                                             &StaWifiMac::ProbeRequestTimeout, this);
}

void
StaWifiMac::SendAssociationRequest (void)
{
  NI_LOG_DEBUG ("StaWifiMac::SendAssociationRequest: m_state = " << m_state);

  NS_LOG_FUNCTION (this << GetBssid ());
  WifiMacHeader hdr;
  hdr.SetAssocReq ();
  hdr.SetAddr1 (GetBssid ());
  hdr.SetAddr2 (GetAddress ());
  hdr.SetAddr3 (GetBssid ());
  hdr.SetDsNotFrom ();
  hdr.SetDsNotTo ();
  Ptr<Packet> packet = Create<Packet> ();
  MgtAssocRequestHeader assoc;
  assoc.SetSsid (GetSsid ());
  assoc.SetSupportedRates (GetSupportedRates ());
  assoc.SetCapabilities (GetCapabilities ());
  if (m_htSupported || m_vhtSupported)
    {
      assoc.SetHtCapabilities (GetHtCapabilities ());
      hdr.SetNoOrder ();

      NI_LOG_DEBUG ("StaWifiMac::SendAssociationRequest: association request is HT capable");
    }
  if (m_vhtSupported)
    {
      assoc.SetVhtCapabilities (GetVhtCapabilities ());

      NI_LOG_DEBUG ("StaWifiMac::SendAssociationRequest: association request is VHT capable");
    }
  packet->AddHeader (assoc);

  // NI API CHANGE
  if (m_NiWifiMAcInterface->GetNiApiEnable())
    {
      m_NiWifiMAcInterface->NiStartTxCtrlDataFrame(packet, hdr);

      NI_LOG_CONSOLE_DEBUG ("WIFI.STA: Send an association request!");
    }

  else
    {
      //The standard is not clear on the correct queue for management
      //frames if we are a QoS AP. The approach taken here is to always
      //use the DCF for these regardless of whether we have a QoS
      //association or not.
      m_dca->Queue (packet, hdr);
    }

  if (m_assocRequestEvent.IsRunning ())
    {
      NI_LOG_DEBUG ("StaWifiMac::SendAssociationRequest: m_assocRequestEvent.IsRunning () = " << m_assocRequestEvent.IsRunning ());

      m_assocRequestEvent.Cancel ();
    }
  m_assocRequestEvent = Simulator::Schedule (m_assocRequestTimeout,
                                             &StaWifiMac::AssocRequestTimeout, this);
}

void
StaWifiMac::TryToEnsureAssociated (void)
{
  NI_LOG_DEBUG ("StaWifiMac::TryToEnsureAssociated: m_state = " << m_state);

  NS_LOG_FUNCTION (this);
  switch (m_state)
    {
    case ASSOCIATED:
      return;
      break;
    case WAIT_PROBE_RESP:
      /* we have sent a probe request earlier so we
         do not need to re-send a probe request immediately.
         We just need to wait until probe-request-timeout
         or until we get a probe response
       */
      break;
    case BEACON_MISSED:
      /* we were associated but we missed a bunch of beacons
       * so we should assume we are not associated anymore.
       * We try to initiate a probe request now.
       */
      m_linkDown ();
      if (m_activeProbing)
        {
          SetState (WAIT_PROBE_RESP);
          SendProbeRequest ();
        }
      break;
    case WAIT_ASSOC_RESP:
      /* we have sent an assoc request so we do not need to
         re-send an assoc request right now. We just need to
         wait until either assoc-request-timeout or until
         we get an assoc response.
       */
      break;
    case REFUSED:
      /* we have sent an assoc request and received a negative
         assoc resp. We wait until someone restarts an
         association with a given ssid.
       */
      break;
    }
}

void
StaWifiMac::AssocRequestTimeout (void)
{
  NI_LOG_DEBUG ("StaWifiMac::AssocRequestTimeout");

  NS_LOG_FUNCTION (this);
  SetState (WAIT_ASSOC_RESP);
  SendAssociationRequest ();
}

void
StaWifiMac::ProbeRequestTimeout (void)
{
  NI_LOG_DEBUG ("StaWifiMac::ProbeRequestTimeout");

  NS_LOG_FUNCTION (this);
  SetState (WAIT_PROBE_RESP);
  SendProbeRequest ();
}

void
StaWifiMac::MissedBeacons (void)
{
  NI_LOG_DEBUG ("StaWifiMac::MissedBeacons: m_beaconWatchdogEnd = " << m_beaconWatchdogEnd
                    << ", Simulator::Now ()) = " << Simulator::Now ());

  NS_LOG_FUNCTION (this);
  if (m_beaconWatchdogEnd > Simulator::Now ())
    {
      if (m_beaconWatchdog.IsRunning ())
        {
          m_beaconWatchdog.Cancel ();
        }
      m_beaconWatchdog = Simulator::Schedule (m_beaconWatchdogEnd - Simulator::Now (),
                                              &StaWifiMac::MissedBeacons, this);
      return;
    }
  NS_LOG_DEBUG ("beacon missed");
  SetState (BEACON_MISSED);
  TryToEnsureAssociated ();
}

void
StaWifiMac::RestartBeaconWatchdog (Time delay)
{
  NI_LOG_DEBUG ("StaWifiMac::RestartBeaconWatchdog");

  NS_LOG_FUNCTION (this << delay);
  m_beaconWatchdogEnd = std::max (Simulator::Now () + delay, m_beaconWatchdogEnd);
  if (Simulator::GetDelayLeft (m_beaconWatchdog) < delay
      && m_beaconWatchdog.IsExpired ())
    {
      NS_LOG_DEBUG ("really restart watchdog.");
      m_beaconWatchdog = Simulator::Schedule (delay, &StaWifiMac::MissedBeacons, this);
    }
}

bool
StaWifiMac::IsAssociated (void) const
{
  return m_state == ASSOCIATED;
}

bool
StaWifiMac::IsWaitAssocResp (void) const
{
  return m_state == WAIT_ASSOC_RESP;
}

void
StaWifiMac::Enqueue (Ptr<const Packet> packet, Mac48Address to)
{
  NI_LOG_DEBUG("StaWifiMac::Enqueue");

  NS_LOG_FUNCTION (this << packet << to);
  if (!IsAssociated ())
    {
      NI_LOG_DEBUG("StaWifiMac::Enqueue: not associated, drop packet");

      NotifyTxDrop (packet);
      TryToEnsureAssociated ();
      return;
    }
  WifiMacHeader hdr;

  //If we are not a QoS AP then we definitely want to use AC_BE to
  //transmit the packet. A TID of zero will map to AC_BE (through \c
  //QosUtilsMapTidToAc()), so we use that as our default here.
  uint8_t tid = 0;

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
      //Transmission of multiple frames in the same TXOP is not
      //supported for now
      hdr.SetQosTxopLimit (0);

      //Fill in the QoS control field in the MAC header
      tid = QosUtilsGetTidForPacket (packet);
      //Any value greater than 7 is invalid and likely indicates that
      //the packet had no QoS tag, so we revert to zero, which'll
      //mean that AC_BE is used.
      if (tid > 7)
        {
          tid = 0;
        }
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

  hdr.SetAddr1 (GetBssid ());
  hdr.SetAddr2 (m_low->GetAddress ());
  hdr.SetAddr3 (to);
  hdr.SetDsNotFrom ();
  hdr.SetDsTo ();

  NI_LOG_DEBUG ("StaWifiMac::Enqueue: to = " << to << ", from = " << m_low->GetAddress ());

  if (m_qosSupported)
    {
      // NI API CHANGE
      if (m_NiWifiMAcInterface->GetNiApiEnable())
        {
          //std::cout << "STA: Send data packet!" << std::endl;
          m_NiWifiMAcInterface->NiStartTxCtrlDataFrame(packet, hdr);

          NI_LOG_DEBUG ("StaWifiMac::Enqueue: packet incl hdr sent to NI WiFi");
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
      if (m_NiWifiMAcInterface->GetNiApiEnable())
        {
          //std::cout << "STA: Send NQoS data packet!" << std::endl;
          m_NiWifiMAcInterface->NiStartTxCtrlDataFrame(packet, hdr);

          NI_LOG_DEBUG ("StaWifiMac::Enqueue: packet incl sent to NI WiFi");
        }
      else
        {
          m_dca->Queue (packet, hdr);
        }
    }
}

void
StaWifiMac::Receive (Ptr<Packet> packet, const WifiMacHeader *hdr)
{
  NI_LOG_DEBUG ("StaWifiMac::Receive: already associated: " << IsAssociated () << ", m_state = " << m_state);

  NS_LOG_FUNCTION (this << packet << hdr);
  NS_ASSERT (!hdr->IsCtl ());
  if (hdr->GetAddr3 () == GetAddress ())
    {
      NI_LOG_DEBUG ("StaWifiMac::Receive: packet by myself");

      NS_LOG_LOGIC ("packet sent by us.");
      return;
    }
  else if (hdr->GetAddr1 () != GetAddress ()
           && !hdr->GetAddr1 ().IsGroup ())
    {
      NI_LOG_DEBUG ("StaWifiMac::Receive: packet not for me (STA)");

      NS_LOG_LOGIC ("packet is not for us");
      NotifyRxDrop (packet);
      return;
    }
  else if (hdr->IsData ())
    {
      NI_LOG_DEBUG ("StaWifiMac::Receive: received data frame");

      if (!IsAssociated ())
        {
          NI_LOG_DEBUG ("StaWifiMac::Receive: received data frame while not associated: ignore");

          NS_LOG_LOGIC ("Received data frame while not associated: ignore");
          NotifyRxDrop (packet);
          return;
        }
      if (!(hdr->IsFromDs () && !hdr->IsToDs ()))
        {
          NI_LOG_DEBUG ("StaWifiMac::Receive: received data frame not from the DS: ignore");

          NS_LOG_LOGIC ("Received data frame not from the DS: ignore");
          NotifyRxDrop (packet);
          return;
        }
      if (hdr->GetAddr2 () != GetBssid ())
        {
          NI_LOG_DEBUG ("StaWifiMac::Receive-: received data frame not from the BSS we are associated with: ignore");

          NS_LOG_LOGIC ("Received data frame not from the BSS we are associated with: ignore");
          NotifyRxDrop (packet);
          return;
        }
      if (hdr->IsQosData ())
        {
          NI_LOG_DEBUG ("StaWifiMac::Receive: received data frame with QoS");

          if (hdr->IsQosAmsdu ())
            {
              NI_LOG_DEBUG ("StaWifiMac::Receive: received data frame is QosAmsdu, deaggregate AMSDU and forward up");

              NS_ASSERT (hdr->GetAddr3 () == GetBssid ());
              DeaggregateAmsduAndForward (packet, hdr);
              packet = 0;
            }
          else
            {
              NI_LOG_DEBUG ("StaWifiMac::Receive: received data frame is not QosAmsdu, just forward up");

              ForwardUp (packet, hdr->GetAddr3 (), hdr->GetAddr1 ());
            }
        }
      else
        {
          NI_LOG_DEBUG ("StaWifiMac::Receive: received data frame is not Qos data, just forward up");

          ForwardUp (packet, hdr->GetAddr3 (), hdr->GetAddr1 ());
        }
      return;
    }
  else if (hdr->IsProbeReq ()
           || hdr->IsAssocReq ())
    {
      NI_LOG_DEBUG ("StaWifiMac::Receive: probe packet received");

      //This is a frame aimed at an AP, so we can safely ignore it.
      NotifyRxDrop (packet);
      return;
    }
  else if (hdr->IsBeacon ())
    {
      NI_LOG_DEBUG ("StaWifiMac::Receive: beacon received");;

      MgtBeaconHeader beacon;
      packet->RemoveHeader (beacon);
      CapabilityInformation capabilities = beacon.GetCapabilities ();
      bool goodBeacon = false;
      if (GetSsid ().IsBroadcast ()
          || beacon.GetSsid ().IsEqual (GetSsid ()))
        {
          NI_LOG_DEBUG ("StaWifiMac::Receive: beacon is for our SSID");

          NS_LOG_LOGIC ("Beacon is for our SSID");
          goodBeacon = true;
        }
      SupportedRates rates = beacon.GetSupportedRates ();
      bool bssMembershipSelectorMatch = false;
      for (uint32_t i = 0; i < m_phy->GetNBssMembershipSelectors (); i++)
        {
          uint32_t selector = m_phy->GetBssMembershipSelector (i);
          if (rates.IsBssMembershipSelectorRate (selector))
            {
              NS_LOG_LOGIC ("Beacon is matched to our BSS membership selector");
              bssMembershipSelectorMatch = true;
            }
        }
      if (m_phy->GetNBssMembershipSelectors () > 0 && bssMembershipSelectorMatch == false)
        {
          NI_LOG_DEBUG ("StaWifiMac::Receive: beacon is not matched to our BSS membership selector");

          NS_LOG_LOGIC ("No match for BSS membership selector");
          goodBeacon = false;
        }
      if ((IsWaitAssocResp () || IsAssociated ()) && hdr->GetAddr3 () != GetBssid ())
        {
          NI_LOG_DEBUG ("StaWifiMac::Receive: beacon is not for us as state is WaitAssocresp");

          NS_LOG_LOGIC ("Beacon is not for us");
          goodBeacon = false;
        }
      if (goodBeacon)
        {
          NI_LOG_DEBUG ("StaWifiMac::Receive: beacon.GetBeaconIntervalUs () = " << beacon.GetBeaconIntervalUs ()
                        << ", m_maxMissedBeacons = " << m_maxMissedBeacons);

          Time delay = MicroSeconds (beacon.GetBeaconIntervalUs () * m_maxMissedBeacons);
          RestartBeaconWatchdog (delay);
          SetBssid (hdr->GetAddr3 ());
          bool isShortPreambleEnabled = capabilities.IsShortPreamble ();
          if (m_erpSupported)
            {
              ErpInformation erpInformation = beacon.GetErpInformation ();
              isShortPreambleEnabled &= !erpInformation.GetBarkerPreambleMode ();
              if (erpInformation.GetUseProtection() == true)
                {
                  m_stationManager->SetUseNonErpProtection (true);
                }
              else
                {
                  m_stationManager->SetUseNonErpProtection (false);
                }
              if (capabilities.IsShortSlotTime () == true)
                {
                  //enable short slot time
                  SetSlot (MicroSeconds (9));
                }
              else
                {
                  //disable short slot time
                  SetSlot (MicroSeconds (20));
                }
            }
            if (m_qosSupported)
            {
              EdcaParameterSet edcaParameters = beacon.GetEdcaParameterSet ();
              //The value of the TXOP Limit field is specified as an unsigned integer, with the least significant octet transmitted first, in units of 32 μs.
              SetEdcaParameters (AC_BE, edcaParameters.GetBeCWmin(), edcaParameters.GetBeCWmax(), edcaParameters.GetBeAifsn(), 32 * MicroSeconds (edcaParameters.GetBeTXOPLimit()));
              SetEdcaParameters (AC_BK, edcaParameters.GetBkCWmin(), edcaParameters.GetBkCWmax(), edcaParameters.GetBkAifsn(), 32 * MicroSeconds (edcaParameters.GetBkTXOPLimit()));
              SetEdcaParameters (AC_VI, edcaParameters.GetViCWmin(), edcaParameters.GetViCWmax(), edcaParameters.GetViAifsn(), 32 * MicroSeconds (edcaParameters.GetViTXOPLimit()));
              SetEdcaParameters (AC_VO, edcaParameters.GetVoCWmin(), edcaParameters.GetVoCWmax(), edcaParameters.GetVoAifsn(), 32 * MicroSeconds (edcaParameters.GetVoTXOPLimit()));
            }
          m_stationManager->SetShortPreambleEnabled (isShortPreambleEnabled);
          m_stationManager->SetShortSlotTimeEnabled (capabilities.IsShortSlotTime ());
        }
      if (goodBeacon && m_state == BEACON_MISSED)
        {
          NI_LOG_DEBUG ("StaWifiMac::Receive: goodBeacon && m_state == BEACON_MISSED, SendAssociationRequest");

          SetState (WAIT_ASSOC_RESP);
          SendAssociationRequest ();
        }
      return;
    }
  else if (hdr->IsProbeResp ())
    {
      NI_LOG_DEBUG ("StaWifiMac::Receive: hdr->IsProbeResp ()");

      if (m_state == WAIT_PROBE_RESP)
        {
          MgtProbeResponseHeader probeResp;
          packet->RemoveHeader (probeResp);
          CapabilityInformation capabilities = probeResp.GetCapabilities ();
          if (!probeResp.GetSsid ().IsEqual (GetSsid ()))
            {
              //not a probe resp for our ssid.
              return;
            }
          SupportedRates rates = probeResp.GetSupportedRates ();
          for (uint32_t i = 0; i < m_phy->GetNBssMembershipSelectors (); i++)
            {
              uint32_t selector = m_phy->GetBssMembershipSelector (i);
              if (!rates.IsSupportedRate (selector))
                {
                  return;
                }
            }
          for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
            {
              WifiMode mode = m_phy->GetMode (i);
              uint8_t nss = 1; // Assume 1 spatial stream
              if (rates.IsSupportedRate (mode.GetDataRate (m_phy->GetChannelWidth (), false, nss)))
                {
                  m_stationManager->AddSupportedMode (hdr->GetAddr2 (), mode);
                  if (rates.IsBasicRate (mode.GetDataRate (m_phy->GetChannelWidth (), false, nss)))
                    {
                      m_stationManager->AddBasicMode (mode);
                    }
                }
            }
            
          bool isShortPreambleEnabled = capabilities.IsShortPreamble ();
          if (m_erpSupported)
            {
              bool isErpAllowed = false;
              for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
              {
                WifiMode mode = m_phy->GetMode (i);
                if (mode.GetModulationClass () == WIFI_MOD_CLASS_ERP_OFDM && rates.IsSupportedRate (mode.GetDataRate (m_phy->GetChannelWidth (), false, 1)))
                  {
                    isErpAllowed = true;
                    break;
                  }
              }
              if (!isErpAllowed)
                {
                  //disable short slot time and set cwMin to 31
                  SetSlot (MicroSeconds (20));
                  ConfigureContentionWindow (31, 1023);
                }
              else
                {
                  ErpInformation erpInformation = probeResp.GetErpInformation ();
                  isShortPreambleEnabled &= !erpInformation.GetBarkerPreambleMode ();
                  if (m_stationManager->GetShortSlotTimeEnabled ())
                    {
                      //enable short slot time
                      SetSlot (MicroSeconds (9));
                    }
                  else
                    {
                      //disable short slot time
                      SetSlot (MicroSeconds (20));
                    }
                  ConfigureContentionWindow (15, 1023);
                }
            }
          m_stationManager->SetShortPreambleEnabled (isShortPreambleEnabled);
          m_stationManager->SetShortSlotTimeEnabled (capabilities.IsShortSlotTime ());
          SetBssid (hdr->GetAddr3 ());
          Time delay = MicroSeconds (probeResp.GetBeaconIntervalUs () * m_maxMissedBeacons);
          RestartBeaconWatchdog (delay);
          if (m_probeRequestEvent.IsRunning ())
            {
              m_probeRequestEvent.Cancel ();
            }
          SetState (WAIT_ASSOC_RESP);
          SendAssociationRequest ();
        }
      return;
    }
  else if (hdr->IsAssocResp ())
    {
      NI_LOG_CONSOLE_DEBUG ("WIFI.STA: Received an association response!");

      if (m_state == WAIT_ASSOC_RESP)
        {
          MgtAssocResponseHeader assocResp;
          packet->RemoveHeader (assocResp);
          if (m_assocRequestEvent.IsRunning ())
            {
              NI_LOG_DEBUG ("StaWifiMac::Receive: m_assocRequestEvent.IsRunning () = " << m_assocRequestEvent.IsRunning ());

              m_assocRequestEvent.Cancel ();
            }
          if (assocResp.GetStatusCode ().IsSuccess ())
            {
              NI_LOG_DEBUG ("StaWifiMac::Receive: associated");

              SetState (ASSOCIATED);
              NS_LOG_DEBUG ("assoc completed");
              CapabilityInformation capabilities = assocResp.GetCapabilities ();
              SupportedRates rates = assocResp.GetSupportedRates ();
              bool isShortPreambleEnabled = capabilities.IsShortPreamble ();
              if (m_erpSupported)
                {
                  bool isErpAllowed = false;
                  for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
                  {
                    WifiMode mode = m_phy->GetMode (i);
                    if (mode.GetModulationClass () == WIFI_MOD_CLASS_ERP_OFDM && rates.IsSupportedRate (mode.GetDataRate (m_phy->GetChannelWidth (), false, 1)))
                      {
                        isErpAllowed = true;
                        break;
                      }
                  }
                  if (!isErpAllowed)
                    {
                      //disable short slot time and set cwMin to 31
                      SetSlot (MicroSeconds (20));
                      ConfigureContentionWindow (31, 1023);
                    }
                  else
                    {
                      ErpInformation erpInformation = assocResp.GetErpInformation ();
                      isShortPreambleEnabled &= !erpInformation.GetBarkerPreambleMode ();
                      if (m_stationManager->GetShortSlotTimeEnabled ())
                        {
                          //enable short slot time
                          SetSlot (MicroSeconds (9));
                        }
                      else
                        {
                          //disable short slot time
                          SetSlot (MicroSeconds (20));
                        }
                      ConfigureContentionWindow (15, 1023);
                    }
                }
              m_stationManager->SetShortPreambleEnabled (isShortPreambleEnabled);
              m_stationManager->SetShortSlotTimeEnabled (capabilities.IsShortSlotTime ());
              if (m_qosSupported)
                {
                  EdcaParameterSet edcaParameters = assocResp.GetEdcaParameterSet ();
                  //The value of the TXOP Limit field is specified as an unsigned integer, with the least significant octet transmitted first, in units of 32 μs.
                  SetEdcaParameters (AC_BE, edcaParameters.GetBeCWmin(), edcaParameters.GetBeCWmax(), edcaParameters.GetBeAifsn(), 32 * MicroSeconds (edcaParameters.GetBeTXOPLimit()));
                  SetEdcaParameters (AC_BK, edcaParameters.GetBkCWmin(), edcaParameters.GetBkCWmax(), edcaParameters.GetBkAifsn(), 32 * MicroSeconds (edcaParameters.GetBkTXOPLimit()));
                  SetEdcaParameters (AC_VI, edcaParameters.GetViCWmin(), edcaParameters.GetViCWmax(), edcaParameters.GetViAifsn(), 32 * MicroSeconds (edcaParameters.GetViTXOPLimit()));
                  SetEdcaParameters (AC_VO, edcaParameters.GetVoCWmin(), edcaParameters.GetVoCWmax(), edcaParameters.GetVoAifsn(), 32 * MicroSeconds (edcaParameters.GetVoTXOPLimit()));
                }
              if (m_htSupported)
                {
                  HtCapabilities htcapabilities = assocResp.GetHtCapabilities ();
                  HtOperations htOperations = assocResp.GetHtOperations ();
                  m_stationManager->AddStationHtCapabilities (hdr->GetAddr2 (), htcapabilities);
                }
              if (m_vhtSupported)
                {
                  VhtCapabilities vhtcapabilities = assocResp.GetVhtCapabilities ();
                  m_stationManager->AddStationVhtCapabilities (hdr->GetAddr2 (), vhtcapabilities);
                }

              for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
                {
                  WifiMode mode = m_phy->GetMode (i);
                  uint8_t nss = 1; // Assume 1 spatial stream
                  if (rates.IsSupportedRate (mode.GetDataRate (m_phy->GetChannelWidth (), false, nss)))
                    {
                      m_stationManager->AddSupportedMode (hdr->GetAddr2 (), mode);
                      if (rates.IsBasicRate (mode.GetDataRate (m_phy->GetChannelWidth (), false, nss)))
                        {
                          m_stationManager->AddBasicMode (mode);
                        }
                    }
                }
              if (m_htSupported)
                {
                  HtCapabilities htcapabilities = assocResp.GetHtCapabilities ();
                  for (uint32_t i = 0; i < m_phy->GetNMcs (); i++)
                    {
                      WifiMode mcs = m_phy->GetMcs (i);
                      if (mcs.GetModulationClass () == WIFI_MOD_CLASS_HT && htcapabilities.IsSupportedMcs (mcs.GetMcsValue ()))
                        {
                          m_stationManager->AddSupportedMcs (hdr->GetAddr2 (), mcs);
                          //here should add a control to add basic MCS when it is implemented
                        }
                    }
                }
              if (m_vhtSupported)
                {
                  VhtCapabilities vhtcapabilities = assocResp.GetVhtCapabilities ();
                  for (uint32_t i = 0; i < m_phy->GetNMcs (); i++)
                    {
                      WifiMode mcs = m_phy->GetMcs (i);
                      if (mcs.GetModulationClass () == WIFI_MOD_CLASS_VHT && vhtcapabilities.IsSupportedTxMcs (mcs.GetMcsValue ()))
                        {
                          m_stationManager->AddSupportedMcs (hdr->GetAddr2 (), mcs);
                          //here should add a control to add basic MCS when it is implemented
                        }
                    }
                }
              if (!m_linkUp.IsNull ())
                {
                  m_linkUp ();
                }
            }
          else
            {
              NI_LOG_DEBUG ("StaWifiMac::Receive: association refused");

              NS_LOG_DEBUG ("assoc refused");
              SetState (REFUSED);
            }
        }
      return;
    }

  //Invoke the receive handler of our parent class to deal with any
  //other frames. Specifically, this will handle Block Ack-related
  //Management Action frames.
  RegularWifiMac::Receive (packet, hdr);
}

SupportedRates
StaWifiMac::GetSupportedRates (void) const
{
  SupportedRates rates;
  uint8_t nss = 1;  // Number of spatial streams is 1 for non-MIMO modes
  if (m_htSupported || m_vhtSupported)
    {
      for (uint32_t i = 0; i < m_phy->GetNBssMembershipSelectors (); i++)
        {
          rates.AddBssMembershipSelectorRate (m_phy->GetBssMembershipSelector (i));
        }
    }
  for (uint32_t i = 0; i < m_phy->GetNModes (); i++)
    {
      WifiMode mode = m_phy->GetMode (i);
      uint64_t modeDataRate = mode.GetDataRate (m_phy->GetChannelWidth (), false, nss);
      NS_LOG_DEBUG ("Adding supported rate of " << modeDataRate);
      rates.AddSupportedRate (modeDataRate);

      NI_LOG_DEBUG ("StaWifiMac::GetSupportedRates: adding supported rate of " << modeDataRate);
    }
  return rates;
}

CapabilityInformation
StaWifiMac::GetCapabilities (void) const
{
  CapabilityInformation capabilities;
  capabilities.SetShortPreamble (m_phy->GetShortPlcpPreambleSupported () || m_erpSupported);
  capabilities.SetShortSlotTime (GetShortSlotTimeSupported () && m_erpSupported);
  return capabilities;
}

void
StaWifiMac::SetState (MacState value)
{
  NI_LOG_DEBUG ("StaWifiMac::SetState: m_state = " << m_state << ", value = " << value);

  if (value == ASSOCIATED
      && m_state != ASSOCIATED)
    {
      m_assocLogger (GetBssid ());
    }
  else if (value != ASSOCIATED
           && m_state == ASSOCIATED)
    {
      m_deAssocLogger (GetBssid ());
    }
  m_state = value;
}

void
StaWifiMac::SetEdcaParameters (AcIndex ac, uint8_t cwMin, uint8_t cwMax, uint8_t aifsn, Time txopLimit)
{
  Ptr<EdcaTxopN> edca = m_edca.find (ac)->second;
  edca->SetMinCw (cwMin);
  edca->SetMaxCw (cwMax);
  edca->SetAifsn (aifsn);
  edca->SetTxopLimit (txopLimit);
}

} //namespace ns3
