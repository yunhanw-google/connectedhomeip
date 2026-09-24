#!/usr/bin/env python3

#
#    Copyright (c) 2026 Project CHIP Authors
#    All rights reserved.
#
#    Licensed under the Apache License, Version 2.0 (the "License");
#    you may not use this file except in compliance with the License.
#    You may obtain a copy of the License at
#
#        http://www.apache.org/licenses/LICENSE-2.0
#
#    Unless required by applicable law or agreed to in writing, software
#    distributed under the License is distributed on an "AS IS" BASIS,
#    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#    See the License for the specific language governing permissions and
#    limitations under the License.
#

# BLE Commissioning and Virtual Wi-Fi Provisioning test between MobileDevice,
# WiFiAPNode, and CHIPEndDevice.

import asyncio
import logging
import os
import sys

import base
from base import (
    BaseTestHelper,
    DEFAULT_REPL_STORAGE_PATH,
    FailIfNot,
    SetTestSet,
    TestFail,
    TestTimeout,
    logger,
)
import click
import coloredlogs
import matter.CertificateAuthority
from matter.ChipStack import ChipStack
import matter.clusters as Clusters
from matter.clusters.Types import NullValue
import matter.FabricAdmin
import matter.logging
import matter.native
from matter.storage import PersistentStorageJSON
from matter.tracing import TracingContext

DEFAULT_WIFI_SSID = "CHIP-VirtualWiFi-AP"
DEFAULT_WIFI_PASSWORD = "ChipWiFiPassword123"
TEST_DISCRIMINATOR = 3840
TEST_SETUPPIN = 20202021

ENDPOINT_ID = 0
LIGHTING_ENDPOINT_ID = 1

TEST_CONTROLLER_NODE_ID = 112233
TEST_DEVICE_NODE_ID = 1


class BleWiFiTestHelper(BaseTestHelper):

  def __init__(
      self,
      nodeId: int,
      paaTrustStorePath: str,
      bleAdapter: int = 0,
      testCommissioner: bool = False,
  ):
    matter.native.Init(bluetoothAdapter=bleAdapter)

    self.chipStack = ChipStack(
        PersistentStorageJSON(DEFAULT_REPL_STORAGE_PATH),
        enableServerInteractions=True,
    )
    self.certificateAuthorityManager = (
        matter.CertificateAuthority.CertificateAuthorityManager(
            chipStack=self.chipStack
        )
    )
    self.certificateAuthority = (
        self.certificateAuthorityManager.NewCertificateAuthority()
    )
    self.fabricAdmin = self.certificateAuthority.NewFabricAdmin(
        vendorId=0xFFF1, fabricId=1
    )
    self.devCtrl = self.fabricAdmin.NewController(
        nodeId, paaTrustStorePath, testCommissioner
    )
    self.controllerNodeId = nodeId
    self.logger = logger
    self.paaTrustStorePath = paaTrustStorePath
    logging.getLogger().setLevel(logging.DEBUG)


async def ble_wifi_commissioning(
    test: BleWiFiTestHelper,
    discriminator: int,
    setup_pin: int,
    device_nodeid: int,
    ssid: str,
    password: str,
):
  logger.info(
      "Starting BLE Commissioning + Wi-Fi Provisioning (discriminator=%d,"
      " pin=%d, nodeId=%d, ssid=%s)",
      discriminator,
      setup_pin,
      device_nodeid,
      ssid,
  )
  commissioned_node_id = await test.devCtrl.CommissionBleWiFi(
      discriminator,
      setup_pin,
      device_nodeid,
      ssid,
      password,
  )
  FailIfNot(
      commissioned_node_id == device_nodeid,
      "CommissionBleWiFi failed or returned unexpected nodeId:"
      f" {commissioned_node_id}",
  )
  logger.info(
      "BLE Commissioning and Wi-Fi Provisioning succeeded for nodeId=%d",
      commissioned_node_id,
  )

  logger.info(
      "Verifying NetworkCommissioning cluster state provisioned over BLE"
  )
  res = await test.devCtrl.ReadAttribute(
      nodeId=device_nodeid,
      attributes=[
          (ENDPOINT_ID, Clusters.NetworkCommissioning.Attributes.Networks),
          (
              ENDPOINT_ID,
              Clusters.NetworkCommissioning.Attributes.LastNetworkID,
          ),
          (
              ENDPOINT_ID,
              Clusters.NetworkCommissioning.Attributes.LastNetworkingStatus,
          ),
          (
              ENDPOINT_ID,
              Clusters.NetworkCommissioning.Attributes.LastConnectErrorValue,
          ),
      ],
      returnClusterObject=True,
  )
  net_cluster = res[ENDPOINT_ID][Clusters.NetworkCommissioning]
  logger.info("Provisioned NetworkCommissioning attributes: %s", net_cluster)
  expected_net_id = ssid.encode("utf-8")
  FailIfNot(
      len(net_cluster.networks) == 1,
      f"Expected 1 provisioned Wi-Fi network, got {len(net_cluster.networks)}",
  )
  FailIfNot(
      net_cluster.networks[0].networkID == expected_net_id,
      f"Expected networkID {expected_net_id!r}, got"
      f" {net_cluster.networks[0].networkID!r}",
  )
  FailIfNot(
      net_cluster.networks[0].connected,
      "Expected Wi-Fi network to be marked connected=True",
  )
  net_enums = Clusters.NetworkCommissioning.Enums
  k_success = net_enums.NetworkCommissioningStatusEnum.kSuccess
  FailIfNot(
      net_cluster.lastNetworkingStatus == k_success,
      "Expected LastNetworkingStatus=kSuccess, got"
      f" {net_cluster.lastNetworkingStatus}",
  )
  FailIfNot(
      net_cluster.lastNetworkID == expected_net_id,
      f"Expected LastNetworkID={expected_net_id!r}, got"
      f" {net_cluster.lastNetworkID!r}",
  )
  FailIfNot(
      net_cluster.lastConnectErrorValue == NullValue,
      "Expected LastConnectErrorValue=NullValue, got"
      f" {net_cluster.lastConnectErrorValue}",
  )

  logger.info(
      "Verifying WiFiNetworkDiagnostics cluster state over Wi-Fi CASE session"
  )
  diag_res = await test.devCtrl.ReadAttribute(
      nodeId=device_nodeid,
      attributes=[
          (ENDPOINT_ID, Clusters.WiFiNetworkDiagnostics.Attributes.Bssid),
          (
              ENDPOINT_ID,
              Clusters.WiFiNetworkDiagnostics.Attributes.SecurityType,
          ),
          (ENDPOINT_ID, Clusters.WiFiNetworkDiagnostics.Attributes.WiFiVersion),
      ],
      returnClusterObject=True,
  )
  wifi_diag = diag_res[ENDPOINT_ID][Clusters.WiFiNetworkDiagnostics]
  logger.info("WiFiNetworkDiagnostics attributes: %s", wifi_diag)
  FailIfNot(
      isinstance(wifi_diag.bssid, bytes) and len(wifi_diag.bssid) == 6,
      f"Expected 6-byte BSSID, got {wifi_diag.bssid!r}",
  )


@base.test_case
def TestDatamodel(test: BleWiFiTestHelper, device_nodeid: int):
  logger.info("Testing datamodel functions over Wi-Fi/IPv6 operational CASE")

  logger.info("Testing on off cluster over Wi-Fi")
  FailIfNot(
      asyncio.run(
          test.TestOnOffCluster(
              nodeId=device_nodeid, endpoint=LIGHTING_ENDPOINT_ID
          )
      ),
      "Failed to test on off cluster over Wi-Fi",
  )

  logger.info("Testing resolve node over Wi-Fi")
  FailIfNot(
      test.TestResolve(nodeId=device_nodeid),
      "Failed to resolve nodeId over Wi-Fi",
  )

  logger.info("Testing attribute writing over Wi-Fi")
  FailIfNot(
      asyncio.run(
          test.TestWriteBasicAttributes(
              nodeId=device_nodeid, endpoint=ENDPOINT_ID
          )
      ),
      "Failed to test Write Basic Attributes over Wi-Fi",
  )

  logger.info("Testing attribute reading over Wi-Fi")
  FailIfNot(
      asyncio.run(
          test.TestReadBasicAttributes(
              nodeId=device_nodeid, endpoint=ENDPOINT_ID
          )
      ),
      "Failed to test Read Basic Attributes over Wi-Fi",
  )


@click.command()
@click.option(
    "--controller-node-id",
    type=int,
    default=TEST_CONTROLLER_NODE_ID,
    help="Node ID to use for this controller.",
)
@click.option(
    "--device-node-id",
    type=int,
    default=TEST_DEVICE_NODE_ID,
    help="Node ID to assign to the commissioned device.",
)
@click.option(
    "--discriminator",
    type=int,
    default=TEST_DISCRIMINATOR,
    help="Setup discriminator of the device to commission over BLE.",
)
@click.option(
    "--setup-pin",
    type=int,
    default=TEST_SETUPPIN,
    help="Setup passcode of the device to commission over BLE.",
)
@click.option(
    "--ssid",
    type=str,
    default=DEFAULT_WIFI_SSID,
    help="Wi-Fi SSID to provision over BLE.",
)
@click.option(
    "--wifi-password",
    type=str,
    default=DEFAULT_WIFI_PASSWORD,
    help="Wi-Fi PSK password to provision over BLE.",
)
@click.option(
    "--ble-adapter",
    type=int,
    default=0,
    help="Bluetooth HCI adapter index (e.g. 0 for hci0).",
)
@click.option(
    "-t",
    "--timeout",
    type=int,
    default=300,
    help="The program will return with timeout after specified seconds.",
)
@click.option(
    "--paa-trust-store-path",
    type=str,
    default="",
    help="Path that contains valid and invalid PAA Root Certificates.",
)
@click.option(
    "--test-set",
    type=click.Choice(["TestDatamodel"], case_sensitive=False),
    default=["TestDatamodel"],
    multiple=True,
    help="Test sets to execute.",
)
def main(
    controller_node_id: int,
    device_node_id: int,
    discriminator: int,
    setup_pin: int,
    ssid: str,
    wifi_password: str,
    ble_adapter: int,
    timeout: int,
    paa_trust_store_path: str,
    test_set,
):
  coloredlogs.install(
      level="DEBUG", fmt="%(asctime)s %(name)s %(levelname)-7s %(message)s"
  )
  matter.logging.RedirectToPythonLogging()

  if timeout:
    test_timeout = TestTimeout(timeout)
    test_timeout.start()

  SetTestSet(test_set, [])

  test = BleWiFiTestHelper(
      nodeId=controller_node_id,
      paaTrustStorePath=paa_trust_store_path,
      bleAdapter=ble_adapter,
  )

  with TracingContext() as tracing_ctx:
    for dest in ("json:log",):
      tracing_ctx.StartFromString(dest)

    asyncio.run(
        ble_wifi_commissioning(
            test,
            discriminator,
            setup_pin,
            device_node_id,
            ssid,
            wifi_password,
        )
    )
    TestDatamodel(test, device_node_id)

  if timeout:
    test_timeout.stop()

  test.devCtrl.Shutdown()
  test.chipStack.Shutdown()
  logger.info("Test finished")
  os._exit(0)


if __name__ == "__main__":
  try:
    main()
  except Exception as ex:
    logger.exception(ex)
    TestFail("Exception occurred when running tests.")
