"""Copyright (c) 2026 Project CHIP Authors

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.

Raw-capture oracles for Cirque E2E tests (re-exports).

The implementations live in cirque: generic pcap oracles in
cirque.pcap.oracles and the Matter ICD Check-In parsers in
cirque.protocols.matter.icd. This module keeps the old import path without
requiring cirque at import time (see helper/_cirque_reexport.py).
"""

from helper._cirque_reexport import reexport

_ORACLES_ALL = [
    'DLT_BLUETOOTH_HCI_H4_WITH_PHDR',
    'DLT_EN10MB',
    'DLT_IEEE802_15_4',
    'DLT_IEEE802_15_4_NOFCS',
    'MLE_CMD_ADVERTISEMENT',
    'MLE_CMD_CHILD_ID_REQUEST',
    'MLE_CMD_CHILD_ID_RESPONSE',
    'MLE_CMD_CHILD_UPDATE_REQUEST',
    'MLE_CMD_CHILD_UPDATE_RESPONSE',
    'MLE_CMD_PARENT_REQUEST',
    'MLE_CMD_PARENT_RESPONSE',
    'acked_mac_commands',
    'dhcp_ack_addresses',
    'eapol_frame_count',
    'hci_le_connection_complete_handles',
    'ieee802154_ack_frames',
    'ieee802154_mac_command_frames',
    'iter_pcap_records',
    'iter_pcap_records_with_ts',
    'thread_decrypted_udp_frames',
    'thread_mle_frames',
]
_ICD_ALL = [
    'matter_icd_checkin_packets',
    'parse_matter_icd_checkin',
    'thread_icd_checkin_frames',
]
reexport(globals(), 'cirque.pcap.oracles', _ORACLES_ALL)
reexport(globals(), 'cirque.protocols.matter.icd', _ICD_ALL)

__all__ = _ORACLES_ALL + _ICD_ALL
