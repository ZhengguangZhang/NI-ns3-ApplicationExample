#!/bin/bash

EXIT_CODE=0

# time out for each test run
TIMEOUT="120s 120s"

# test lists for a two node loopback scenario 
declare -a NODE1_START_CMD_LIST=(
                     "./ni_start.sh Adhoc Sta1 config_wifi_local_loopback"
                     "./ni_start.sh Infra Ap config_wifi_local_loopback"
                     "./ni_start.sh LteWiFi BS config_lwa_lwip_ext_local_loopback"
                     "./ni_start.sh Lte BS config_lte_local_loopback"
                     "./ni_start.sh LteWiFi BS config_lwa_lwip_ext__ref_lte__lte_loopback"
                     "./ni_start.sh LteWiFi BS config_lwa_lwip_ext__ref_wifi__wifi_loopback"
                     "./ni_start.sh LteWiFi BS config_lwa_lwip_ext__ref_lte_lwa_split__wifi_loopback"
                     "./ni_start.sh LteWiFi BS config_lwa_lwip_ext__ref_lte_lwa_split__lte_wifi_loopback"
                     "./ni_start.sh LteWiFi BS config_lwa_lwip_ext__ref_lte_lwa_full__wifi_loopback"
                     "./ni_start.sh LteWiFi BS config_lwa_lwip_ext__ref_lte_lwip__wifi_loopback"
                     "./ni_start.sh LteWiFi BS config_lwa_lwip_ext__ref_lte__no_loopback"
                     "./ni_start.sh LteWiFi BS config_lwa_lwip_ext__ref_lte__lte_wifi_loopback"
                     "./ni_start.sh LteWiFi BS config_lwa_lwip_ext__ref_wifi__no_loopback"
                     "./ni_start.sh LteWiFi BS config_lwa_lwip_ext__ref_wifi__lte_wifi_loopback"
                     )
declare -a NODE2_START_CMD_LIST=(
                     "./ni_start.sh Adhoc Sta2 config_wifi_local_loopback"
                     "./ni_start.sh Infra Sta config_wifi_local_loopback"
                     "./ni_start.sh LteWiFi TS config_lwa_lwip_ext_local_loopback"
                     "./ni_start.sh Lte TS config_lte_local_loopback"
                     "./ni_start.sh LteWiFi TS config_lwa_lwip_ext__ref_lte__lte_loopback"
                     "./ni_start.sh LteWiFi TS config_lwa_lwip_ext__ref_wifi__wifi_loopback"
                     "./ni_start.sh LteWiFi TS config_lwa_lwip_ext__ref_lte_lwa_split__wifi_loopback"
                     "./ni_start.sh LteWiFi TS config_lwa_lwip_ext__ref_lte_lwa_split__lte_wifi_loopback"
                     "./ni_start.sh LteWiFi TS config_lwa_lwip_ext__ref_lte_lwa_full__wifi_loopback"
                     "./ni_start.sh LteWiFi TS config_lwa_lwip_ext__ref_lte_lwip__wifi_loopback"
                     "./ni_start.sh LteWiFi TS config_lwa_lwip_ext__ref_lte__no_loopback"
                     "./ni_start.sh LteWiFi TS config_lwa_lwip_ext__ref_lte__lte_wifi_loopback"
                     "./ni_start.sh LteWiFi TS config_lwa_lwip_ext__ref_wifi__no_loopback"
                     "./ni_start.sh LteWiFi TS config_lwa_lwip_ext__ref_wifi__lte_wifi_loopback"
                     )

# check for maching lists sizes
if [ "${#NODE1_START_CMD_LIST[@]}" -ne ${#NODE2_START_CMD_LIST[@]} ]; then
  echo "ERROR: test script vector sizes doesn't mach"
  EXIT_CODE=1
  cd -
  exit $EXIT_CODE
fi
NUMTESTS=${#NODE1_START_CMD_LIST[@]}

# go to ns-3 root folder
cd ../../../

# loop over test lists
for (( i=1; i<${NUMTESTS}+1; i++ ));
do
  echo ""
  echo "###############################################################################"
  echo "Test $i / ${NUMTESTS} :"
  echo "  ${NODE1_START_CMD_LIST[$i-1]}"
  echo "  ${NODE2_START_CMD_LIST[$i-1]}"
  echo "  executing..."
  # execute start command for first node in background considerung a execution time out
  timeout -k $TIMEOUT ${NODE1_START_CMD_LIST[$i-1]} &> test_output_${i-1}_a.log & P1=$!
  sleep 1
  # execute start command for second node in background considerung a execution time out
  timeout -k $TIMEOUT ${NODE2_START_CMD_LIST[$i-1]} &> test_output_${i-1}_b.log & P2=$!

  # wait for first returning process and collect exit status
  wait -n
  EXIT_SCRIPT_1=$?
  if [ "$EXIT_SCRIPT_1" -eq 124 ]; then
    echo "  ...stopped on TIMEOUT!"
  fi
  # wait for second returning process and collect exit status
  wait -n
  EXIT_SCRIPT_2=$?
  if [ "$EXIT_SCRIPT_1" -eq 124 ]; then
    echo "  ...stopped on TIMEOUT!"
  fi
  
  echo "  ...finished with exit code 1: $EXIT_SCRIPT_1, exit code 2: $EXIT_SCRIPT_2"

  # check exit status of both nodes
  if [ "$EXIT_SCRIPT_1" -ne 0 ] || [ "$EXIT_SCRIPT_2" -ne 0 ]; then
    EXIT_CODE=1
    echo "  FAIL"
    echo ""
    echo "  ---- Displaying test output of file test_output_${i-1}_a.log ---------------------"
    echo ""
    cat test_output_${i-1}_a.log
    echo ""
    echo "  ---- Displaying test output of file test_output_${i-1}_b.log ---------------------"
    echo ""
    cat test_output_${i-1}_b.log
    echo ""
    echo "  -----------------------------------------------------------------------------"
  else
    echo "  PASS"
  fi
done

echo ""

if [ "$EXIT_CODE" -eq 0 ]; then
  echo "###############################################################################"
  echo "ALL TESTS PASSED"
  echo "###############################################################################"
else
  echo "###############################################################################"
  echo "SOME TESTS FAILED"
  echo "###############################################################################"
fi

cd -
exit $EXIT_CODE

